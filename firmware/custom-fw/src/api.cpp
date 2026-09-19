// ============================================================================
// api.cpp — REST API implementation.
//
// Web server: the Arduino-ESP32 built-in `WebServer` (synchronous, in-core,
// zero extra dependencies). Path parameters use its UriBraces matcher, and
// firmware/image uploads use its streaming upload callback so large bodies are
// never buffered into a String.
// ============================================================================
#include "api.h"
#include "config.h"
#include "tubes.h"
#include "leds.h"
#include "control.h"
#include "esp1.h"
#include "shell.h"
#include "net.h"
#include "identity.h"
#include "peers.h"
#include "build_stamp.h"
#include "rtc.h"
#include "web_index.h"   // INDEX_HTML (PROGMEM), generated from data/index.html
#include "web_helper.h"  // HELPER_GZ: the full tools/helper.html, gzipped (generated)
#include "web_icon.h"    // ICON_PNG: PWA / home-screen icon (generated from data/icon.png)

#include <Arduino.h>
#include <WiFi.h>
#include <LittleFS.h>
#include <Update.h>
#include <uri/UriBraces.h>
#include <uri/UriGlob.h>
#include <esp_system.h>
#include <sys/time.h>
#include <Preferences.h>
#include <vector>
#include <functional>

namespace {

WebServer* g_srv = nullptr;

// Map a REST /button/{name} to a Control action (same handler physical buttons
// use). Returns false for an unknown name. Legacy left/right map to Up/Down.
bool parseButton(const String& s, Control::Button& out) {
    if (s == "mode")  { out = Control::Button::Mode;  return true; }
    if (s == "power") { out = Control::Button::Power; return true; }
    if (s == "up"   || s == "left")  { out = Control::Button::Up;   return true; }
    if (s == "down" || s == "right") { out = Control::Button::Down; return true; }
    return false;
}

// ---- Deferred reboot after OTA --------------------------------------------
bool     g_rebootPending = false;
uint32_t g_rebootAt      = 0;
bool     g_otaAuthed     = false;   // set at OTA upload START; gates writing the image partition

// ---- Image (BMP) upload accumulator ---------------------------------------
// Multipart streaming only. Capped so a big body can't OOM/reboot the device.
std::vector<uint8_t> g_imgBuf;
bool g_imgOverflow = false;
const size_t IMG_CAP = 110u * 1024u;   // ~110 KB hard cap

// ---- Raw RGB565 streaming state (POST /tube/{}/raw565) --------------------
const size_t RAW565_EXPECT = (size_t)PANEL_W * PANEL_H * 2;   // 64800 bytes
uint8_t  g_rawLine[PANEL_W * 2];   // one row of wire bytes (270 B)
uint16_t g_rawPix[PANEL_W];        // decoded row
int      g_rawFill   = 0;          // bytes buffered into g_rawLine
size_t   g_rawTotal  = 0;          // total bytes received
int      g_rawTube   = -1;
bool     g_rawActive = false;

// ---- tiny JSON helpers -----------------------------------------------------
String jsonError(const char* msg) {
    String s = "{\"ok\":false,\"error\":\"";
    s += msg;
    s += "\"}";
    return s;
}

// Minimal, dependency-free extraction of an integer field from a flat JSON
// object body, e.g. {"r":10,"g":0,"b":255}. Returns fallback if absent.
long jsonInt(const String& body, const char* key, long fallback) {
    String pat = "\"";
    pat += key;
    pat += "\"";
    int k = body.indexOf(pat);
    if (k < 0) return fallback;
    int c = body.indexOf(':', k + pat.length());
    if (c < 0) return fallback;
    int i = c + 1;
    while (i < (int)body.length() && (body[i] == ' ' || body[i] == '\t')) i++;
    bool neg = false;
    if (i < (int)body.length() && (body[i] == '-' || body[i] == '+')) {
        neg = body[i] == '-';
        i++;
    }
    long v = 0;
    bool any = false;
    while (i < (int)body.length() && isdigit((int)body[i])) {
        v = v * 10 + (body[i] - '0');
        i++;
        any = true;
    }
    if (!any) return fallback;
    return neg ? -v : v;
}

// Escape a string for embedding as a JSON string value.
String jsonEscape(const String& in) {
    String o;
    o.reserve(in.length() + 8);
    for (size_t i = 0; i < in.length(); ++i) {
        char c = in[i];
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n";  break;
            case '\r': o += "\\r";  break;
            case '\t': o += "\\t";  break;
            default:
                if ((uint8_t)c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
                else o += c;
        }
    }
    return o;
}

// Extract a quoted string field from a flat JSON object, e.g. {"tz":"..."}.
// Returns "" if the key is absent or not a quoted string. No escape handling
// (TZ / mode values contain none).
String jsonStr(const String& body, const char* key) {
    String pat = "\"";
    pat += key;
    pat += "\"";
    int k = body.indexOf(pat);
    if (k < 0) return String();
    int c = body.indexOf(':', k + pat.length());
    if (c < 0) return String();
    int i = c + 1;
    while (i < (int)body.length() && (body[i] == ' ' || body[i] == '\t')) i++;
    if (i >= (int)body.length() || body[i] != '"') return String();
    int start = i + 1;
    int end = body.indexOf('"', start);
    if (end < 0) return String();
    return body.substring(start, end);
}

uint8_t clamp8(long v) {
    if (v < 0)   return 0;
    if (v > 255) return 255;
    return (uint8_t)v;
}

// ---- Admin auth ------------------------------------------------------------
// Local-first and opt-in: with no admin token set the API behaves exactly as
// before (open on the LAN). Once a token is set — the precondition for exposing
// the device beyond the LAN — every MUTATING route needs `Authorization: Bearer
// <token>`; GET status/health/config stay open. The token is set over the UART
// shell (physical access) or by the first REST caller (LAN bootstrap); changing
// it thereafter needs the current token (the setter is guarded like any other).
String g_adminTok;
void   adminLoad()  { Preferences p; if (p.begin("esptube", true)) { g_adminTok = p.getString("admin", ""); p.end(); } }
void   adminStore() { Preferences p; if (!p.begin("esptube", false)) return; if (g_adminTok.length()) p.putString("admin", g_adminTok); else p.remove("admin"); p.end(); }
bool   adminSet()   { return g_adminTok.length() > 0; }

// Compare without an early-out on the first mismatched byte (timing hygiene).
bool ctEq(const String& a, const String& b) {
    if (a.length() != b.length()) return false;
    uint8_t d = 0; for (size_t i = 0; i < a.length(); ++i) d |= (uint8_t)(a[i] ^ b[i]);
    return d == 0;
}
bool bearerOk() {
    if (!adminSet()) return true;                          // open until a token exists
    String h = g_srv->header("Authorization");
    if (h.startsWith("Bearer ")) h = h.substring(7);
    h.trim();
    return h.length() && ctEq(h, g_adminTok);
}
void send401() {
    g_srv->sendHeader("WWW-Authenticate", "Bearer");
    g_srv->send(401, "application/json", "{\"ok\":false,\"error\":\"admin token required\"}");
}
// Wrap a POST handler so it 401s when a token is set and the caller is not authorized.
std::function<void()> guard(std::function<void()> h) {
    return [h]() { if (!bearerOk()) { send401(); return; } h(); };
}

// Filesystem path safety for /fs/put and /fs/delete: absolute, no traversal, no
// embedded control byte, bounded length. Rewrites p in place (adds a leading /).
bool safeFsPath(String& p) {
    if (!p.length() || p.length() > 128) return false;
    if (!p.startsWith("/")) p = "/" + p;
    if (p.indexOf("..") >= 0 || p.indexOf("//") >= 0) return false;
    for (size_t i = 0; i < p.length(); ++i) if ((uint8_t)p[i] < 0x20) return false;
    return true;
}

String collectBody();   // defined below (body regardless of Content-Type)

// The sanitized nixie message with '°' restored for display (UTF-8).
String nixieDisplayText() {
    String s; const char* m = Control::nixieMessage();
    for (size_t i = 0; m[i]; ++i) { if (m[i] == NIXIE_CH_DEGREE) s += "\xC2\xB0"; else s += m[i]; }
    return s;
}

// POST /nixie/text {"text":"HELLO","effect":"static|flash|scroll","ms":500}
// (also text=&effect=&ms= form params). Zero pixels pushed: the clock renders it.
void handleNixieText() {
    String body = collectBody();
    String text = jsonStr(body, "text");   if (!text.length() && g_srv->hasArg("text"))   text = g_srv->arg("text");
    String eff  = jsonStr(body, "effect"); if (!eff.length()  && g_srv->hasArg("effect")) eff  = g_srv->arg("effect");
    long ms = jsonInt(body, "ms", -1);     if (ms < 0 && g_srv->hasArg("ms")) ms = g_srv->arg("ms").toInt();
    if (!text.length()) { g_srv->send(400, "application/json", jsonError("missing text")); return; }
    if (!Control::nixieText(text.c_str(), eff.length() ? eff.c_str() : "static", ms < 0 ? 500 : (uint32_t)ms)) {
        g_srv->send(400, "application/json", jsonError("bad effect (static|flash|scroll)")); return; }
    String r = "{\"ok\":true,\"mode\":\"nixie\",\"kind\":\""; r += Control::nixieKindName();
    r += "\",\"text\":\""; r += jsonEscape(nixieDisplayText()); r += "\",\"ms\":"; r += (uint32_t)Control::nixieMs(); r += "}";
    g_srv->send(200, "application/json", r);
}
// POST /nixie/countdown {"seconds":N}
void handleNixieCountdown() {
    String body = collectBody();
    long s = jsonInt(body, "seconds", -1); if (s < 0 && g_srv->hasArg("seconds")) s = g_srv->arg("seconds").toInt();
    if (s < 0) { g_srv->send(400, "application/json", jsonError("missing seconds")); return; }
    Control::nixieCountdown((uint32_t)s);
    String r = "{\"ok\":true,\"mode\":\"nixie\",\"kind\":\"countdown\",\"seconds\":"; r += (uint32_t)s; r += "}";
    g_srv->send(200, "application/json", r);
}
void handleNixieStop() { Control::nixieStop(); g_srv->send(200, "application/json", "{\"ok\":true,\"mode\":\"clock\"}"); }

// POST /test/clocksweep {"from":0,"to":86399,"step_ms":0,"fade":1} — drive every
// time in the range through the real clock render path. /test/stop ends it early.
void handleTestSweep() {
    String body = collectBody();
    long from = jsonInt(body, "from", 0), to = jsonInt(body, "to", 86399);
    long step = jsonInt(body, "step_ms", 0), fade = jsonInt(body, "fade", 1);
    if (g_srv->hasArg("from")) from = g_srv->arg("from").toInt();
    if (g_srv->hasArg("to")) to = g_srv->arg("to").toInt();
    if (g_srv->hasArg("step_ms")) step = g_srv->arg("step_ms").toInt();
    if (g_srv->hasArg("fade")) fade = g_srv->arg("fade").toInt();
    if (from < 0 || to < 0 || step < 0) { g_srv->send(400, "application/json", jsonError("bad range")); return; }
    Control::clockSweepStart((uint32_t)from, (uint32_t)to, (uint16_t)(step > 60000 ? 60000 : step), fade != 0);
    String r = "{\"ok\":true,\"sweep\":{\"from\":"; r += (uint32_t)from; r += ",\"to\":"; r += (uint32_t)to;
    r += ",\"total\":"; r += Control::clockSweepTotal(); r += ",\"fade\":"; r += fade ? "true" : "false"; r += "}}";
    g_srv->send(200, "application/json", r);
}
void handleTestStop() { Control::clockSweepStop(); g_srv->send(200, "application/json", "{\"ok\":true}"); }

// Human-readable cause of the last reset (crash vs. power-cycle vs. OTA), so a
// reboot can be diagnosed from /status without a serial console.
const char* resetReasonStr() {
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON:   return "poweron";
        case ESP_RST_SW:        return "software";
        case ESP_RST_PANIC:     return "panic";
        case ESP_RST_INT_WDT:   return "int_wdt";
        case ESP_RST_TASK_WDT:  return "task_wdt";
        case ESP_RST_WDT:       return "wdt";
        case ESP_RST_BROWNOUT:  return "brownout";
        case ESP_RST_DEEPSLEEP: return "deepsleep";
        case ESP_RST_EXT:       return "external";
        default:                return "unknown";
    }
}

// Return a searchable representation of the request body REGARDLESS of
// Content-Type. For application/json the raw JSON is in the "plain" arg. For
// application/x-www-form-urlencoded the WebServer already split the body into
// args — so a JSON body sent that way (e.g. curl --data '{"r":..}') lands as an
// arg NAME with an empty value; reassembling name=value&... lets the JSON
// parser still find the fields, and also exposes real r=&g=&b= form params.
String collectBody() {
    if (g_srv->hasArg("plain")) {
        String b = g_srv->arg("plain");
        if (b.length()) return b;
    }
    String b;
    for (int i = 0; i < g_srv->args(); ++i) {
        b += g_srv->argName(i);
        b += '=';
        b += g_srv->arg(i);
        b += '&';
    }
    return b;
}

// Resolve one color component: prefer JSON ("key":N in the body), else fall
// back to a form/query param (key=N). Returns 0 if neither is present.
uint8_t colorComponent(const String& body, const char* key) {
    long v = jsonInt(body, key, -1);          // -1 sentinel = not found in JSON
    if (v < 0 && g_srv->hasArg(key)) v = g_srv->arg(key).toInt();
    return clamp8(v < 0 ? 0 : v);
}

int pathTubeIndex() {  // first {} in /tube/{}/...
    return g_srv->pathArg(0).toInt();
}

// ---------------------------------------------------------------------------
// Handlers
// ---------------------------------------------------------------------------
/* LittleFS.usedBytes() walks the whole 12.5 MB partition (~250 ms with the sidecar on it) and the
   helper polls /status every 5 s — that stall was eating stream acks. Cache it; /fs writes refresh it. */
static uint32_t g_fsUsed = 0, g_fsTotal = 0, g_fsStatAt = 0;
static void fsStatRefresh(bool force = false) {
    if (!force && g_fsStatAt && millis() - g_fsStatAt < 60000) return;
    g_fsUsed = LittleFS.usedBytes(); g_fsTotal = LittleFS.totalBytes(); g_fsStatAt = millis() ? millis() : 1;
}
void handlePing() { String r = "{\"ok\":true,\"t\":"; r += (uint32_t)millis(); r += "}"; g_srv->send(200, "application/json", r); }
void handleStatus() {
    fsStatRefresh();
    String j = "{";
    j += "\"uptime_ms\":";  j += (uint32_t)millis();
    j += ",\"wifi\":";      j += (WiFi.status() == WL_CONNECTED) ? "true" : "false";
    j += ",\"ip\":\"";      j += WiFi.localIP().toString(); j += "\"";
    j += ",\"ssid\":\"";    j += Net::ssid(); j += "\"";
    j += ",\"net_state\":\""; j += Net::stateName(); j += "\"";
    j += ",\"ap\":";        j += Net::apActive() ? "true" : "false";          // recovery AP esptube-setup up?
    j += ",\"rtc\":";       j += Rtc::present() ? "true" : "false";          // DS1302 holds a valid time
    j += ",\"idle_min\":";  j += (uint32_t)Control::idleMinutes();            // MANUAL -> clock after this long without content
    j += ",\"hostname\":\""; j += Id::hostname(); j += "\"";
    j += ",\"mac\":\"";      j += Id::mac(); j += "\"";                     // stable device id (unique per clock)
    j += ",\"chip\":\"";     j += Id::chip(); j += "\"";
    j += ",\"model\":\"";    j += Id::model(); j += "\"";                   // hardware model (discovery / layout)
    j += ",\"version\":\"" FW_VERSION "\"";                                // comparable build id (git short SHA)
    j += ",\"admin\":";      j += adminSet() ? "true" : "false";           // mutating routes require Authorization: Bearer <token>
    j += ",\"rssi\":";       j += (int32_t)WiFi.RSSI();
    j += ",\"rssi_avg\":";   j += Net::rssiAvg();                              // ~20 s smoothed
    j += ",\"netwarn\":";    j += Net::warnDbm();                              // weak-signal nixie warning below this dBm (0 = off)
    j += ",\"wifi_sleep\":"; j += WiFi.getSleep() ? "true" : "false";
    j += ",\"free_heap\":"; j += (uint32_t)ESP.getFreeHeap();
    j += ",\"fs_used\":";   j += g_fsUsed;                                   // cached 60 s (see fsStatRefresh)
    j += ",\"fs_total\":";  j += g_fsTotal;
    j += ",\"tubes\":[";
    for (uint8_t i = 0; i < TUBE_COUNT; ++i) {
        if (i) j += ",";
        uint8_t lr, lg, lb; Leds::getTube(i, lr, lg, lb);
        j += "{\"index\":"; j += i;
        j += ",\"populated\":"; j += Tubes::isPopulated(i) ? "true" : "false";
        j += ",\"content\":{\"kind\":\""; j += Tubes::contentKindStr(i); j += "\"";
        j += ",\"text\":\""; j += jsonEscape(Tubes::contentText(i)); j += "\"}";
        j += ",\"led\":{\"r\":"; j += lr; j += ",\"g\":"; j += lg; j += ",\"b\":"; j += lb; j += "}";
        j += "}";
    }
    // Dead = not currently alive (not populated in the persisted mask OR
    // manually forced off).
    j += "],\"dead_tubes\":[";
    bool first = true;
    for (uint8_t i = 0; i < TUBE_COUNT; ++i) {
        if (!Tubes::alive(i)) {
            if (!first) j += ",";
            j += i; first = false;
        }
    }
    j += "],\"populated_mask\":"; j += (uint32_t)Tubes::populatedMask();
    j += ",\"mode\":\"";       j += Control::modeName(); j += "\"";
    j += ",\"preset\":";       j += (uint32_t)Control::preset();
    j += ",\"preset_name\":\""; j += Control::presetName(Control::preset()); j += "\"";
    j += ",\"preset_count\":"; j += (uint32_t)Control::presetCount();
    j += ",\"led_effect\":\""; j += Control::effectName(); j += "\"";
    { uint8_t sr, sg, sb; Control::solidColor(sr, sg, sb);
      j += ",\"led_solid\":{\"r\":"; j += sr; j += ",\"g\":"; j += sg; j += ",\"b\":"; j += sb; j += "}"; }
    j += ",\"brightness\":";   j += (uint32_t)Control::brightness();
    j += ",\"tz\":\"";         j += Control::tz(); j += "\"";
    j += ",\"time_valid\":";   j += Control::timeValid() ? "true" : "false";
    j += ",\"draw_ms\":";      j += (uint32_t)(Tubes::lastDrawUs() / 1000);   // last nixie draw/fade
    j += ",\"reset_reason\":\""; j += resetReasonStr(); j += "\"";            // why the last boot happened
    j += ",\"build\":\"" BUILD_STAMP "\"";                          // which image is actually running
    if (Control::mode() == Control::Mode::Nixie) {                             // device-rendered message
        j += ",\"nixie\":{\"kind\":\""; j += Control::nixieKindName(); j += "\"";
        j += ",\"text\":\"";  j += jsonEscape(nixieDisplayText()); j += "\"";
        j += ",\"ms\":";      j += (uint32_t)Control::nixieMs();
        j += ",\"remaining\":"; j += (int32_t)Control::nixieRemaining(); j += "}";
    }
    {   const Esp1::Stats& st = Esp1::stats();                              // ESP/1 stream channel
        j += ",\"stream\":{\"port\":"; j += (uint32_t)Esp1::PORT; j += ",\"tcp\":"; j += Esp1::tcpConnected() ? "true" : "false";
        j += ",\"token\":"; j += Esp1::token().length() ? "true" : "false";
        j += ",\"fps\":"; j += String(st.fps, 1); j += ",\"kbps\":"; j += st.kbps;
        j += ",\"frames\":"; j += st.frames; j += ",\"bad\":"; j += st.bad; j += ",\"last_ms_ago\":"; j += st.lastFrameMs ? (uint32_t)(millis() - st.lastFrameMs) : 0;
        j += ",\"clients\":"; j += (uint32_t)Esp1::tcpClients(); j += ",\"busy_pct\":"; j += (uint32_t)st.busyPct; j += ",\"loops_per_s\":"; j += st.loopsPerSec;
        j += ",\"uart\":"; j += Shell::uartJson(); j += ",\"proto\":\"1.1\",\"epoch\":"; j += (uint32_t)Tubes::epoch(); j += ",\"superseded\":"; j += st.superseded; j += ",\"stalled\":"; j += st.stalled; j += "}"; }
    if (Control::clockSweepActive()) {                                          // test in progress
        j += ",\"sweep\":{\"active\":true,\"sec\":"; j += Control::clockSweepSec();
        j += ",\"done\":"; j += Control::clockSweepDone(); j += ",\"total\":"; j += Control::clockSweepTotal();
        j += ",\"max_draw_ms\":"; j += (uint32_t)(Control::clockSweepMaxDrawUs() / 1000); j += "}";
    } else {
        j += ",\"sweep\":{\"active\":false,\"done\":"; j += Control::clockSweepDone();
        j += ",\"total\":"; j += Control::clockSweepTotal();
        j += ",\"max_draw_ms\":"; j += (uint32_t)(Control::clockSweepMaxDrawUs() / 1000); j += "}";
    }
    j += "}";
    g_srv->send(200, "application/json", j);
}

void handleHealth() {
    g_srv->send(200, "application/json", "{\"ok\":true}");
}

// GET /peers — other _esptube._tcp nodes on the LAN (cached; browse runs on its own task).
// This is the browser helper's discovery oracle (a browser can't do mDNS itself).
void handlePeers() {
    g_srv->send(200, "application/json", Peers::json());
}

// Serve the embedded web control panel (PROGMEM). send_P streams from flash.
void handleRoot() {
    g_srv->send_P(200, "text/html", INDEX_HTML);
}
// The full helper, served by the clock itself: a phone on the recovery hotspot (192.168.4.1/helper)
// or on the LAN (esptube.local/helper) gets the whole console; "Add to Home Screen" makes it an app.
void handleHelper() {
    g_srv->sendHeader("Content-Encoding", "gzip");
    g_srv->sendHeader("Cache-Control", "no-cache");
    g_srv->send_P(200, "text/html", (const char*)HELPER_GZ, HELPER_GZ_LEN);
}
void handleManifest() {
    g_srv->send(200, "application/manifest+json",
        "{\"name\":\"ESPTube Helper\",\"short_name\":\"ESPTube\",\"start_url\":\"/helper\",\"scope\":\"/\",\"display\":\"standalone\","
        "\"background_color\":\"#0c0e13\",\"theme_color\":\"#ffb454\",\"icons\":[{\"src\":\"/icon.png\",\"sizes\":\"180x180\",\"type\":\"image/png\"}]}");
}
void handleIcon() {
    g_srv->sendHeader("Cache-Control", "max-age=86400");
    g_srv->send_P(200, "image/png", (const char*)ICON_PNG, ICON_PNG_LEN);
}
// ---- LittleFS as a small asset store: POST /fs/put/samples/characters.js (raw body) writes that
// file (the path rides in the URI: the core parses ?query args only AFTER a raw body, so they are
// invisible at RAW_START); GET /fs lists; /samples/* is served statically (the characters sidecar
// for the hosted helper).
static File g_fsPut; static size_t g_fsPutBytes = 0; static String g_fsPutPath;
void handleFsPutChunk() {
    HTTPRaw& r = g_srv->raw();
    if (r.status == RAW_START) {
        if (!bearerOk()) { g_fsPutPath = ""; g_fsPut = File(); return; }   // done-handler is guarded; don't write flash first
        g_fsPutPath = g_srv->uri().substring(7);               // "/fs/put" + "/samples/x.js" — uri() is known before the body
        if (!safeFsPath(g_fsPutPath)) { g_fsPutPath = ""; return; }
        int slash = g_fsPutPath.lastIndexOf('/'); if (slash > 0) LittleFS.mkdir(g_fsPutPath.substring(0, slash));
        g_fsPut = LittleFS.open(g_fsPutPath, "w"); g_fsPutBytes = 0;
        Serial.printf("[fs] put %s\n", g_fsPutPath.c_str());
    } else if (r.status == RAW_WRITE) {
        if (g_fsPut) { g_fsPut.write(r.buf, r.currentSize); g_fsPutBytes += r.currentSize; }
    } else if (r.status == RAW_END || r.status == RAW_ABORTED) {
        if (g_fsPut) g_fsPut.close();
        if (r.status == RAW_ABORTED) { LittleFS.remove(g_fsPutPath); Serial.println("[fs] put aborted"); }
        else Serial.printf("[fs] put done: %u bytes\n", (unsigned)g_fsPutBytes);
    }
}
void handleFsPutDone() {
    fsStatRefresh(true);
    if (!g_fsPutPath.length() || !LittleFS.exists(g_fsPutPath)) { g_srv->send(400, "application/json", jsonError("no file written (path? body?)")); return; }
    String r = "{\"ok\":true,\"path\":\""; r += g_fsPutPath; r += "\",\"bytes\":"; r += (uint32_t)g_fsPutBytes; r += ",\"fs_used\":"; r += (uint32_t)LittleFS.usedBytes(); r += "}";
    g_srv->send(200, "application/json", r);
}
void handleFsList() {
    String j = "{\"used\":"; j += (uint32_t)LittleFS.usedBytes(); j += ",\"total\":"; j += (uint32_t)LittleFS.totalBytes(); j += ",\"files\":[";
    bool first = true;
    std::function<void(const String&)> walk = [&](const String& dir) {
        File d = LittleFS.open(dir); if (!d || !d.isDirectory()) return;
        for (File f = d.openNextFile(); f; f = d.openNextFile()) {
            String path = String(f.path());
            if (f.isDirectory()) { walk(path); continue; }
            if (!first) j += ","; first = false;
            j += "{\"path\":\""; j += path; j += "\",\"bytes\":"; j += (uint32_t)f.size(); j += "}";
        }
    };
    walk("/"); j += "]}"; g_srv->send(200, "application/json", j);
}
void handleFsDelete() {
    fsStatRefresh(true);
    String p = g_srv->hasArg("path") ? g_srv->arg("path") : String();
    if (!safeFsPath(p)) { g_srv->send(400, "application/json", jsonError("bad path")); return; }
    g_srv->send(LittleFS.remove(p) ? 200 : 404, "application/json", LittleFS.exists(p) ? jsonError("not deleted") : "{\"ok\":true}");
}

// Build the config JSON (populated set + mode/brightness/tz for UI init).
String configJson() {
    String j = "{\"populated_mask\":";
    j += (uint32_t)Tubes::populatedMask();
    j += ",\"tubes\":[";
    for (uint8_t i = 0; i < TUBE_COUNT; ++i) {
        if (i) j += ",";
        j += "{\"index\":"; j += i;
        j += ",\"populated\":"; j += Tubes::isPopulated(i) ? "true" : "false";
        j += "}";
    }
    j += "],\"mode\":\"";       j += Control::modeName(); j += "\"";
    j += ",\"led_effect\":\""; j += Control::effectName(); j += "\"";
    { uint8_t sr, sg, sb; Control::solidColor(sr, sg, sb);
      j += ",\"led_solid\":{\"r\":"; j += sr; j += ",\"g\":"; j += sg; j += ",\"b\":"; j += sb; j += "}"; }
    j += ",\"brightness\":";  j += (uint32_t)Control::brightness();
    j += ",\"tz\":\"";        j += Control::tz(); j += "\"";
    j += "}";
    return j;
}

void handleConfigGet() {
    g_srv->send(200, "application/json", configJson());
}

// POST /config/populated  {"mask":N}  (N=0..63; also accepts mask=N form param)
void handleConfigPopulated() {
    String body = collectBody();
    long m = jsonInt(body, "mask", -1);
    if (m < 0 && g_srv->hasArg("mask")) m = g_srv->arg("mask").toInt();
    if (m < 0) { g_srv->send(400, "application/json", jsonError("missing mask")); return; }
    uint8_t mask = (uint8_t)(m & 0x3F);
    Tubes::setPopulatedMask(mask);
    Control::forceClockRedraw();   // re-lay-out the clock across the new set
    String r = "{\"ok\":true,\"mask\":"; r += (uint32_t)mask; r += "}";
    g_srv->send(200, "application/json", r);
}

// POST /config/brightness  {"value":N}  (0..255; also accepts value=N)
void handleConfigBrightness() {
    String body = collectBody();
    long v = jsonInt(body, "value", -1);
    if (v < 0 && g_srv->hasArg("value")) v = g_srv->arg("value").toInt();
    if (v < 0) { g_srv->send(400, "application/json", jsonError("missing value")); return; }
    uint8_t b = clamp8(v);
    Control::setBrightness(b);
    String r = "{\"ok\":true,\"value\":"; r += (uint32_t)b; r += "}";
    g_srv->send(200, "application/json", r);
}

// POST /config/tz  {"tz":"PST8PDT,M3.2.0,M11.1.0"}  (also accepts tz=... form)
void handleConfigTz() {
    String body = collectBody();
    String tz = jsonStr(body, "tz");
    if (tz.length() == 0 && g_srv->hasArg("tz")) tz = g_srv->arg("tz");
    if (tz.length() == 0) { g_srv->send(400, "application/json", jsonError("missing tz")); return; }
    Control::setTz(tz);
    String r = "{\"ok\":true,\"tz\":\""; r += tz; r += "\"}";
    g_srv->send(200, "application/json", r);
}

// POST /config/token {"token":"..."} (empty = open) — shared secret for the ESP/1 TCP channel.
void handleConfigToken() {
    String body = collectBody();
    String t = jsonStr(body, "token"); if (!t.length() && g_srv->hasArg("token")) t = g_srv->arg("token");
    Esp1::setToken(t);
    g_srv->send(200, "application/json", t.length() ? "{\"ok\":true,\"token\":true}" : "{\"ok\":true,\"token\":false}");
}

// POST /config/admin {"token":"..."} (empty = clear) — the REST/OTA admin secret.
// Bootstrap: open while unset (guard() passes), protected once set. Also settable
// over the UART shell (`admin <x|clear>`). ArduinoOTA adopts it as its password at boot.
void handleConfigAdmin() {
    String body = collectBody();
    String t = jsonStr(body, "token"); if (!t.length() && g_srv->hasArg("token")) t = g_srv->arg("token");
    g_adminTok = t; adminStore();
    g_srv->send(200, "application/json", t.length() ? "{\"ok\":true,\"admin\":true}" : "{\"ok\":true,\"admin\":false}");
}

// POST /mode/{clock|off|manual}  — display layer
void handleMode() {
    String m = g_srv->pathArg(0);
    if (!Control::setModeByName(m.c_str())) {
        g_srv->send(400, "application/json", jsonError("bad mode")); return;
    }
    String r = "{\"ok\":true,\"mode\":\""; r += Control::modeName(); r += "\"}";
    g_srv->send(200, "application/json", r);
}

// POST /led/{off|solid|rainbow|breathe|comet}  — LED layer.
// For solid, an optional {"r","g","b"} (or r=&g=&b=) sets the solid color.
void handleLed() {
    String e = g_srv->pathArg(0);
    if (e == "solid") {
        String body = collectBody();
        if (jsonInt(body, "r", -1) >= 0 || jsonInt(body, "g", -1) >= 0 ||
            jsonInt(body, "b", -1) >= 0 || g_srv->hasArg("r") ||
            g_srv->hasArg("g") || g_srv->hasArg("b")) {
            Control::setSolidColor(colorComponent(body, "r"),
                                   colorComponent(body, "g"),
                                   colorComponent(body, "b"));
        } else {
            Control::setEffect(Control::Effect::Solid);   // reuse stored color
        }
    } else if (!Control::setEffectByName(e.c_str())) {
        g_srv->send(400, "application/json", jsonError("bad effect")); return;
    }
    uint8_t sr, sg, sb; Control::solidColor(sr, sg, sb);
    String r = "{\"ok\":true,\"led_effect\":\""; r += Control::effectName();
    r += "\",\"led_solid\":{\"r\":"; r += sr; r += ",\"g\":"; r += sg; r += ",\"b\":"; r += sb; r += "}}";
    g_srv->send(200, "application/json", r);
}

// POST /preset/next  or  /preset/{index} — cycle/select a device-native preset.
void handlePreset() {
    String a = g_srv->pathArg(0);
    if (a == "next") {
        Control::nextPreset();
    } else if (a.length() && a[0] >= '0' && a[0] <= '9' && Control::setPresetByIndex(a.toInt())) {
        // applied
    } else {
        g_srv->send(400, "application/json", jsonError("bad preset (use next or 0..count-1)")); return;
    }
    String r = "{\"ok\":true,\"preset\":"; r += (uint32_t)Control::preset();
    r += ",\"preset_name\":\""; r += Control::presetName(Control::preset()); r += "\"}";
    g_srv->send(200, "application/json", r);
}

void handleTubeText() {
    int i = pathTubeIndex();
    if (!tubeIndexValid(i)) { g_srv->send(400, "application/json", jsonError("bad tube index")); return; }
    String body = g_srv->hasArg("plain") ? g_srv->arg("plain") : String();
    Control::notifyManualContent();   // stop the clock overwriting pushed content
    Tubes::drawText((uint8_t)i, body);
    String r = "{\"ok\":true,\"tube\":"; r += i;
    r += ",\"alive\":"; r += Tubes::alive((uint8_t)i) ? "true" : "false"; r += "}";
    g_srv->send(200, "application/json", r);
}

void handleTubeRgb() {
    int i = pathTubeIndex();
    if (!tubeIndexValid(i)) { g_srv->send(400, "application/json", jsonError("bad tube index")); return; }
    // Accept {"r":N,"g":N,"b":N} regardless of Content-Type, and r=&g=&b= as a
    // fallback. Clamp 0-255.
    String body = collectBody();
    uint8_t r = colorComponent(body, "r");
    uint8_t g = colorComponent(body, "g");
    uint8_t b = colorComponent(body, "b");
    // Underglow (LED layer) is independent of the display — do NOT switch the
    // panel to MANUAL. An active animated effect will override this next tick.
    Leds::setTube((uint8_t)i, r, g, b);
    Leds::show();
    Serial.printf("[rgb] tube %d -> r=%u g=%u b=%u\n", i, r, g, b);
    String resp = "{\"ok\":true,\"tube\":"; resp += i;
    resp += ",\"r\":"; resp += r; resp += ",\"g\":"; resp += g; resp += ",\"b\":"; resp += b; resp += "}";
    g_srv->send(200, "application/json", resp);
}

// /tube/{}/image — MULTIPART ONLY. The upload callback streams the BMP into a
// capped g_imgBuf; this done-handler decodes it row-by-row, then frees it.
void handleTubeImageDone() {
    int i = pathTubeIndex();
    if (!tubeIndexValid(i)) { g_imgBuf.clear(); g_imgBuf.shrink_to_fit();
        g_srv->send(400, "application/json", jsonError("bad tube index")); return; }

    if (g_imgOverflow) {
        g_imgBuf.clear(); g_imgBuf.shrink_to_fit(); g_imgOverflow = false;
        g_srv->send(400, "application/json", jsonError("image too large"));
        return;
    }
    if (g_imgBuf.empty()) {
        g_srv->send(400, "application/json", jsonError("no multipart image (use multipart/form-data)"));
        return;
    }

    Control::notifyManualContent();   // pushed content -> MANUAL
    const size_t len = g_imgBuf.size();
    bool ok = Tubes::drawImageBMP((uint8_t)i, g_imgBuf.data(), len);  // row-by-row, no full-frame alloc
    g_imgBuf.clear();
    g_imgBuf.shrink_to_fit();         // release the upload buffer immediately

    String resp = "{\"ok\":"; resp += ok ? "true" : "false";
    resp += ",\"tube\":"; resp += i; resp += ",\"bytes\":"; resp += (uint32_t)len; resp += "}";
    g_srv->send(ok ? 200 : 400, "application/json", resp);
}

// Streaming upload callback for /tube/{}/image (multipart file part). Capped.
void handleTubeImageUpload() {
    HTTPUpload& up = g_srv->upload();
    if (up.status == UPLOAD_FILE_START) {
        g_imgBuf.clear();
        g_imgOverflow = false;
        g_imgBuf.reserve(4096);
    } else if (up.status == UPLOAD_FILE_WRITE) {
        if (g_imgOverflow) return;
        if (g_imgBuf.size() + up.currentSize > IMG_CAP) {   // refuse, don't OOM
            g_imgOverflow = true;
            g_imgBuf.clear();
            g_imgBuf.shrink_to_fit();
            return;
        }
        g_imgBuf.insert(g_imgBuf.end(), up.buf, up.buf + up.currentSize);
    }
}

// -----------------------------------------------------------------------------
// POST /tube/{}/raw565 — body = PANEL_W*PANEL_H*2 = 64800 bytes of RGB565,
// row-major, TOP-DOWN, each pixel 2 bytes HIGH byte first (big-endian). Streamed
// straight to the panel one row at a time during the upload callback — NO
// full-frame allocation.
// The full-panel push shares the region-blit path: the panel lock is held per 16-row block, not
// for the whole HTTP upload (an upload the client aborted used to keep the lock forever and stall
// every ESP/1 stream), there is no black pre-fill flash, and rows go out 16 at a time, not 1.
Tubes::BlitCtx g_blitCtx;         // REST serves one request at a time: /raw565 and /blit share it
void handleRaw565Done() {
    if (g_rawActive) { Tubes::endBlit(g_blitCtx); g_rawActive = false; }
    bool ok = (g_rawTube >= 0) && (g_rawTotal >= RAW565_EXPECT);
    String resp = "{\"ok\":"; resp += ok ? "true" : "false";
    resp += ",\"tube\":"; resp += g_rawTube;
    resp += ",\"bytes\":"; resp += (uint32_t)g_rawTotal;
    resp += ",\"expected\":"; resp += (uint32_t)RAW565_EXPECT; resp += "}";
    g_srv->send(ok ? 200 : 400, "application/json", resp);
    g_rawTube = -1;
}

// Parse the tube index straight out of the URI ("/tube/<n>/raw565"), since this
// runs inside the upload callback.
int rawTubeFromUri() {
    String u = g_srv->uri();
    int a = u.indexOf("/tube/");
    if (a < 0) return -1;
    a += 6;
    int n = 0; bool any = false;
    while (a < (int)u.length() && isdigit((int)u[a])) { n = n * 10 + (u[a] - '0'); a++; any = true; }
    return any ? n : -1;
}

void handleRaw565Upload() {
    HTTPUpload& up = g_srv->upload();
    if (up.status == UPLOAD_FILE_START) {
        g_rawTube = rawTubeFromUri();
        g_rawFill = 0; g_rawTotal = 0; g_rawActive = false;
        if (tubeIndexValid(g_rawTube) && Tubes::beginBlit(g_blitCtx, (uint8_t)g_rawTube, 0, 0, PANEL_W, PANEL_H)) {
            Control::notifyManualContent();
            g_rawActive = true;
        }
    } else if (up.status == UPLOAD_FILE_ABORTED) {
        if (g_rawActive) { Tubes::abortBlit(g_blitCtx); g_rawActive = false; Serial.println("[api] raw565 upload aborted"); }
    } else if (up.status == UPLOAD_FILE_WRITE) {
        if (!g_rawActive) return;
        size_t n = up.currentSize;
        const uint8_t* p = up.buf;
        g_rawTotal += n;
        while (n > 0) {
            int want = (PANEL_W * 2) - g_rawFill;
            int take = (int)n < want ? (int)n : want;
            memcpy(g_rawLine + g_rawFill, p, take);
            g_rawFill += take; p += take; n -= take;
            if (g_rawFill == PANEL_W * 2) {                  // one full row
                for (int x = 0; x < PANEL_W; ++x) {
                    g_rawPix[x] = ((uint16_t)g_rawLine[2 * x] << 8) | g_rawLine[2 * x + 1]; // HIGH byte first
                }
                Tubes::pushBlitLine(g_blitCtx, g_rawPix);
                g_rawFill = 0;
            }
        }
    }
    // END handled in handleRaw565Done().
}

// -----------------------------------------------------------------------------
// POST /tube/{}/blit?x=&y=&w=&h= — body (multipart file part) = w*h*2 bytes of
// RGB565 big-endian, row-major, top-down, written into that sub-window only.
// The "send deltas, never frames" primitive: a scrolling strip ships ~5% of a frame.
// Rows wider than the clamped width are trimmed; short bodies leave the rest untouched.
int    g_blitReqW = 0;            // width the CLIENT sent (row stride in the body)
size_t g_blitTotal = 0, g_blitExpect = 0;
bool   g_blitActive = false;
int    g_blitTube = -1;

void handleBlitDone() {
    if (g_blitActive) { Tubes::endBlit(g_blitCtx); g_blitActive = false; }
    bool ok = (g_blitTube >= 0) && g_blitExpect && (g_blitTotal >= g_blitExpect);
    String resp = "{\"ok\":"; resp += ok ? "true" : "false";
    resp += ",\"tube\":"; resp += g_blitTube;
    resp += ",\"bytes\":"; resp += (uint32_t)g_blitTotal;
    resp += ",\"expected\":"; resp += (uint32_t)g_blitExpect; resp += "}";
    g_srv->send(ok ? 200 : 400, "application/json", resp);
    g_blitTube = -1;
}
void handleBlitUpload() {
    HTTPUpload& up = g_srv->upload();
    if (up.status == UPLOAD_FILE_START) {
        g_blitTube = rawTubeFromUri();
        g_rawFill = 0; g_blitTotal = 0; g_blitActive = false; g_blitExpect = 0;
        const int x = g_srv->arg("x").toInt(), y = g_srv->arg("y").toInt();
        const int w = g_srv->arg("w").toInt(), h = g_srv->arg("h").toInt();
        g_blitReqW = w;
        if (w > 0 && w <= PANEL_W && h > 0 && h <= PANEL_H &&
            tubeIndexValid(g_blitTube) && Tubes::beginBlit(g_blitCtx, (uint8_t)g_blitTube, x, y, w, h)) {
            Control::notifyManualContent();
            g_blitActive = true;
            g_blitExpect = (size_t)w * (size_t)h * 2u;
        }
    } else if (up.status == UPLOAD_FILE_ABORTED) {
        if (g_blitActive) { Tubes::abortBlit(g_blitCtx); g_blitActive = false; }
    } else if (up.status == UPLOAD_FILE_WRITE) {
        if (!g_blitActive) return;
        size_t n = up.currentSize; const uint8_t* p = up.buf;
        g_blitTotal += n;
        const int stride = g_blitReqW * 2;                     // bytes per row as sent
        while (n > 0) {
            int want = stride - g_rawFill;
            int take = (int)n < want ? (int)n : want;
            memcpy(g_rawLine + g_rawFill, p, take);
            g_rawFill += take; p += take; n -= take;
            if (g_rawFill == stride) {                          // one full row (as sent)
                const int bw = Tubes::blitWidth(g_blitCtx);     // clamped width actually drawn
                for (int xx = 0; xx < bw; ++xx)
                    g_rawPix[xx] = ((uint16_t)g_rawLine[2 * xx] << 8) | g_rawLine[2 * xx + 1];
                Tubes::pushBlitLine(g_blitCtx, g_rawPix);
                g_rawFill = 0;
            }
        }
    }
}

// MASTER CLEAR: every panel black, every glow off, and nothing left running that would repaint —
// the message/countdown engine, a clock sweep and the LED effect all stop. Stays MANUAL (dark) until
// something is pushed or the idle timer hands back to the clock. (A streaming bridge/helper keeps
// its own loop — the helper's 🧹 stops those first.)
void handleTubesClear() {
    Control::masterClear();           // supersedes in-flight stream frames first, so they cannot repaint over the clear
    String r = "{\"ok\":true,\"cleared\":\"all\",\"epoch\":"; r += (uint32_t)Tubes::epoch(); r += "}";
    g_srv->send(200, "application/json", r);
}
void handleConfigIdle() {
    String body = collectBody(); long m = -1;
    int k = body.indexOf("\"minutes\""); if (k >= 0) { int c = body.indexOf(':', k); if (c >= 0) m = strtol(body.c_str() + c + 1, nullptr, 10); }
    if (m < 0 && g_srv->hasArg("minutes")) m = g_srv->arg("minutes").toInt();
    if (m < 0 || m > 1440) { g_srv->send(400, "application/json", jsonError("minutes 0..1440 (0 = never)")); return; }
    Control::setIdleMinutes((uint16_t)m);
    String r = "{\"ok\":true,\"idle_min\":"; r += m; r += "}"; g_srv->send(200, "application/json", r);
}
// POST /config/netwarn {"dbm":-78}  (0 = off): the weak-signal nixie warning threshold, persisted
void handleConfigNetwarn() {
    String body = collectBody(); long d = 1;
    int k = body.indexOf("\"dbm\""); if (k >= 0) { int c = body.indexOf(':', k); if (c >= 0) d = strtol(body.c_str() + c + 1, nullptr, 10); }
    if (d == 1 && g_srv->hasArg("dbm")) d = g_srv->arg("dbm").toInt();
    if (d != 0 && (d > -30 || d < -95)) { g_srv->send(400, "application/json", jsonError("dbm -95..-30 (0 = off)")); return; }
    Net::setWarnDbm((int)d);
    String r = "{\"ok\":true,\"netwarn\":"; r += Net::warnDbm(); r += "}"; g_srv->send(200, "application/json", r);
}

// Virtual button — routes to the SAME handler as the physical buttons.
void handleButton() {
    String which = g_srv->pathArg(0);
    Control::Button b;
    if (!parseButton(which, b)) { g_srv->send(400, "application/json", jsonError("unknown button")); return; }
    Control::onButton(b);
    String r = "{\"ok\":true,\"button\":\""; r += which; r += "\"}";
    g_srv->send(200, "application/json", r);
}

// /ota — firmware image upload straight into the Update partition, so future
// re-flashes can skip USB. (ArduinoOTA is also started separately in main.)
void handleOtaDone() {
    bool ok = !Update.hasError();
    g_srv->sendHeader("Connection", "close");
    g_srv->send(ok ? 200 : 500, "application/json",
                ok ? "{\"ok\":true,\"rebooting\":true}" : jsonError("update failed"));
    if (ok) {
        g_rebootPending = true;
        g_rebootAt = millis() + 800;   // let the response flush first
    }
}

// POST /reboot — restart the device. Deferred (same path as OTA) so the
// response flushes before ESP.restart() drops the socket.
void handleReboot() {
    g_srv->sendHeader("Connection", "close");
    g_srv->send(200, "application/json", "{\"ok\":true,\"rebooting\":true}");
    g_rebootPending = true;
    g_rebootAt = millis() + 500;
}

// One chunk of firmware, whichever way the body arrived.
static void otaChunk(bool start, bool write, bool end, bool aborted, uint8_t* buf, size_t n, size_t total) {
    if (start) {
        g_otaAuthed = bearerOk();                       // headers are parsed before the body
        if (!g_otaAuthed) { Serial.println("[ota] refused: admin token required"); return; }
        Serial.println("[ota] start");
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) Update.printError(Serial);
        String md5 = g_srv->header("X-OTA-MD5"); md5.trim();   // optional integrity: Update.end() verifies it
        if (md5.length() == 32) { Update.setMD5(md5.c_str()); Serial.printf("[ota] expecting md5 %s\n", md5.c_str()); }
    } else if (write) {
        if (!g_otaAuthed) return;
        if (Update.write(buf, n) != n) Update.printError(Serial);
    } else if (end) {
        if (!g_otaAuthed) return;
        if (Update.end(true)) Serial.printf("[ota] success: %u bytes\n", (unsigned)total);
        else Update.printError(Serial);
    } else if (aborted) {
        if (g_otaAuthed) { Serial.println("[ota] aborted"); Update.abort(); }
    }
}

// The core's WebServer hands a multipart/form-data body to this callback through upload(), but a
// plain body (curl --data-binary) through raw() — and the *other* accessor is a null unique_ptr,
// so reading the wrong one is an instant LoadProhibited panic (it looked like a successful OTA:
// the node rebooted... into the old image). Content-Type decides which one is live.
void handleOtaUpload() {
    if (g_srv->header("Content-Type").startsWith("multipart/")) {
        HTTPUpload& up = g_srv->upload();
        otaChunk(up.status == UPLOAD_FILE_START, up.status == UPLOAD_FILE_WRITE, up.status == UPLOAD_FILE_END,
                 up.status == UPLOAD_FILE_ABORTED, up.buf, up.currentSize, up.totalSize);
    } else {
        HTTPRaw& r = g_srv->raw();
        otaChunk(r.status == RAW_START, r.status == RAW_WRITE, r.status == RAW_END,
                 r.status == RAW_ABORTED, r.buf, r.currentSize, r.totalSize);
    }
}

// ---- /wifi: roaming + recovery (see net.h). Passwords go in, never out. ----
void handleWifiGet()   { g_srv->send(200, "application/json", Net::statusJson()); }
void handleWifiScan()  { Net::scanSync(); g_srv->send(200, "application/json", Net::statusJson()); }
void handleWifiAdd() {
    String body = collectBody(); String ssid = jsonStr(body, "ssid"), pass = jsonStr(body, "pass");
    if (!ssid.length() && g_srv->hasArg("ssid")) { ssid = g_srv->arg("ssid"); pass = g_srv->arg("pass"); }
    if (!ssid.length()) { g_srv->send(400, "application/json", jsonError("missing ssid")); return; }
    if (!Net::add(ssid, pass)) { g_srv->send(400, "application/json", jsonError("bad ssid/pass")); return; }
    g_srv->send(200, "application/json", "{\"ok\":true,\"joining\":true}");
}
void handleWifiForget() {
    String body = collectBody(); String ssid = jsonStr(body, "ssid"); if (!ssid.length() && g_srv->hasArg("ssid")) ssid = g_srv->arg("ssid");
    g_srv->send(Net::forget(ssid) ? 200 : 404, "application/json", Net::known(ssid) ? jsonError("not saved") : "{\"ok\":true}");
}
void handleWifiJoin() {
    String body = collectBody(); String ssid = jsonStr(body, "ssid"); if (!ssid.length() && g_srv->hasArg("ssid")) ssid = g_srv->arg("ssid");
    if (!Net::join(ssid)) { g_srv->send(404, "application/json", jsonError("not saved — add it first")); return; }
    g_srv->send(200, "application/json", "{\"ok\":true,\"joining\":true}");
}
void handleWifiAp() {
    String body = collectBody(); bool off = body.indexOf("false") >= 0 || (g_srv->hasArg("on") && g_srv->arg("on") == "false");
    if (off) Net::stopAp(); else Net::startAp(true);
    g_srv->send(200, "application/json", Net::statusJson());
}
void handleWifiApPass() {
    String body = collectBody(); String p = jsonStr(body, "pass");
    if (p.length() && p.length() < 8) { g_srv->send(400, "application/json", jsonError("8+ chars (or empty for open)")); return; }
    Net::setApPass(p); g_srv->send(200, "application/json", "{\"ok\":true}");
}
// POST /config/time {"epoch":N} — set the clock (and the DS1302) from the browser: the offline path.
void handleConfigTime() {
    String body = collectBody(); unsigned long e = 0;
    int k = body.indexOf("\"epoch\""); if (k >= 0) { int c = body.indexOf(':', k); if (c >= 0) e = strtoul(body.c_str() + c + 1, nullptr, 10); }
    if (!e && g_srv->hasArg("epoch")) e = strtoul(g_srv->arg("epoch").c_str(), nullptr, 10);
    if (e < 1700000000UL) { g_srv->send(400, "application/json", jsonError("epoch seconds required")); return; }
    struct timeval tv = { (time_t)e, 0 }; settimeofday(&tv, nullptr);
    bool rtc = Rtc::saveSystemTime(); Control::forceClockRedraw();
    String r = "{\"ok\":true,\"rtc\":"; r += rtc ? "true" : "false"; r += "}";
    g_srv->send(200, "application/json", r);
}
void handleWifiReconnect() { Net::reconnect(); g_srv->send(200, "application/json", "{\"ok\":true}"); }

void handleNotFound() {
    // CORS preflight: any OPTIONS request lands here (routes are method-specific).
    // enableCORS(true) already appends Access-Control-Allow-Origin: *; add the
    // method/header allowances a preflight expects and reply 204.
    if (g_srv->method() == HTTP_OPTIONS) {
        g_srv->sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
        g_srv->sendHeader("Access-Control-Allow-Headers", "Content-Type, Authorization, X-OTA-MD5");
        g_srv->sendHeader("Access-Control-Max-Age", "600");
        g_srv->send(204);
        return;
    }
    // Captive portal: while the recovery AP is up, a phone's connectivity probe
    // (/generate_204, /hotspot-detect.html, ...) or any unknown URL for a foreign host
    // is redirected to our page, which makes the phone pop the sign-in sheet.
    if (Net::apActive()) {
        String host = g_srv->hostHeader();
        if (host != "192.168.4.1" && host != String(Id::hostname()) + ".local" && !g_srv->uri().startsWith("/tube")) {
            g_srv->sendHeader("Location", "http://192.168.4.1/", true);
            g_srv->send(302, "text/plain", "");
            return;
        }
    }
    g_srv->send(404, "application/json", jsonError("not found"));
}

}  // namespace

namespace Api {

void begin(WebServer& server) {
    fsStatRefresh(true);   // the cold LittleFS walk costs 2-3 s: pay it during boot, not inside the first /status poll
    adminLoad();           // opt-in admin token: when set, guard() gates every mutating route (see bearerOk)
    g_srv = &server;

    // Append Access-Control-Allow-Origin: * to every response (future-proofs
    // external tools; the embedded UI is same-origin and doesn't need it).
    server.enableCORS(true);
    // handleOtaUpload needs Content-Type (raw vs multipart); bearerOk needs Authorization;
    // otaChunk reads the optional X-OTA-MD5 integrity header.
    static const char* kHdrs[] = {"Content-Type", "Authorization", "X-OTA-MD5"};
    server.collectHeaders(kHdrs, 3);

    // ---- Open GET routes (info only; never mutate) ----
    server.on("/", HTTP_GET, handleRoot);
    server.on("/helper",               HTTP_GET,  handleHelper);
    server.on("/helper.html",          HTTP_GET,  handleHelper);
    server.on("/manifest.webmanifest", HTTP_GET,  handleManifest);
    server.on("/icon.png",             HTTP_GET,  handleIcon);
    server.on("/fs",                   HTTP_GET,  handleFsList);
    server.serveStatic("/samples/", LittleFS, "/samples/", "max-age=86400");
    // Where every display physically is (esptube.layout/1, millimetres, a few hundred bytes): written by the
    // helper with POST /fs/put/layout.json, read back here by phones, the bridge, other computers.
    server.serveStatic("/layout.json", LittleFS, "/layout.json", "no-cache");
    server.on("/status", HTTP_GET, handleStatus);
    server.on("/ping",   HTTP_GET, handlePing);          // the cheapest round trip (helper link test)
    server.on("/health", HTTP_GET, handleHealth);
    server.on("/peers",  HTTP_GET, handlePeers);         // other _esptube._tcp clocks on the LAN (cached)
    server.on("/config", HTTP_GET, handleConfigGet);
    server.on("/wifi",   HTTP_GET, handleWifiGet);        // no passwords in the body (see net.h)

    // ---- Mutating routes: guard() 401s each once an admin token is set ----
    server.on(UriGlob("/fs/put/*"), HTTP_POST, guard(handleFsPutDone), handleFsPutChunk);   // (UriRegex would pull std::regex in: +235 KB)
    server.on("/fs/delete",            HTTP_POST, guard(handleFsDelete));

    server.on("/config/populated",  HTTP_POST, guard(handleConfigPopulated));
    server.on("/config/brightness", HTTP_POST, guard(handleConfigBrightness));
    server.on("/config/tz",         HTTP_POST, guard(handleConfigTz));
    server.on("/config/token",      HTTP_POST, guard(handleConfigToken));
    server.on("/config/admin",      HTTP_POST, guard(handleConfigAdmin));   // open only while no token is set (bootstrap)
    server.on("/config/time",       HTTP_POST, guard(handleConfigTime));
    server.on("/config/idle",       HTTP_POST, guard(handleConfigIdle));
    server.on("/config/netwarn",    HTTP_POST, guard(handleConfigNetwarn));
    server.on("/wifi/scan",         HTTP_POST, guard(handleWifiScan));
    server.on("/wifi/add",          HTTP_POST, guard(handleWifiAdd));
    server.on("/wifi/forget",       HTTP_POST, guard(handleWifiForget));
    server.on("/wifi/join",         HTTP_POST, guard(handleWifiJoin));
    server.on("/wifi/ap",           HTTP_POST, guard(handleWifiAp));
    server.on("/wifi/appass",       HTTP_POST, guard(handleWifiApPass));
    server.on("/wifi/reconnect",    HTTP_POST, guard(handleWifiReconnect));

    // Display mode + independent LED-effect layer.
    server.on(UriBraces("/mode/{}"),   HTTP_POST, guard(handleMode));
    server.on(UriBraces("/led/{}"),    HTTP_POST, guard(handleLed));
    server.on(UriBraces("/preset/{}"), HTTP_POST, guard(handlePreset));

    // Device-rendered nixie messages (no pixels pushed) + the clock-sweep test.
    server.on("/nixie/text",      HTTP_POST, guard(handleNixieText));
    server.on("/nixie/countdown", HTTP_POST, guard(handleNixieCountdown));
    server.on("/nixie/stop",      HTTP_POST, guard(handleNixieStop));
    server.on("/test/clocksweep", HTTP_POST, guard(handleTestSweep));
    server.on("/test/stop",       HTTP_POST, guard(handleTestStop));

    server.on(UriBraces("/tube/{}/text"),  HTTP_POST, guard(handleTubeText));
    server.on(UriBraces("/tube/{}/rgb"),   HTTP_POST, guard(handleTubeRgb));
    // Image (BMP): multipart streaming upload, decoded row-by-row.
    server.on(UriBraces("/tube/{}/image"), HTTP_POST, guard(handleTubeImageDone), handleTubeImageUpload);
    // Raw RGB565 full-frame: streamed straight to the panel (no full-frame alloc).
    server.on(UriBraces("/tube/{}/raw565"), HTTP_POST, guard(handleRaw565Done), handleRaw565Upload);
    // Region blit: only the changed rectangle (x,y,w,h query params; body = w*h*2 bytes).
    server.on(UriBraces("/tube/{}/blit"),   HTTP_POST, guard(handleBlitDone),   handleBlitUpload);

    server.on("/tubes/clear", HTTP_POST, guard(handleTubesClear));

    server.on(UriBraces("/button/{}"), HTTP_POST, guard(handleButton));

    // OTA firmware upload + plain restart. handleOtaUpload also gates on bearerOk()
    // at START so an unauthorized body never touches the image partition.
    server.on("/ota", HTTP_POST, guard(handleOtaDone), handleOtaUpload);
    server.on("/reboot", HTTP_POST, guard(handleReboot));

    // Catches unmatched routes AND all OPTIONS preflight requests.
    server.onNotFound(handleNotFound);
}

// The current admin token (empty = none). main.cpp adopts it as the ArduinoOTA
// password at boot so port 3232 is not a second unauthenticated reflash path.
String adminToken() { return g_adminTok; }
void   setAdminToken(const String& t) { g_adminTok = t; adminStore(); }   // UART shell `admin`

void pump() {
    // Button presses (physical + REST) are handled synchronously via
    // Control::onButton, so pump() only services the deferred reboot (OTA, /reboot).
    if (g_rebootPending && (int32_t)(millis() - g_rebootAt) >= 0) {
        g_rebootPending = false;
        Serial.println("[api] rebooting...");
        delay(50);
        ESP.restart();
    }
}

}  // namespace Api
