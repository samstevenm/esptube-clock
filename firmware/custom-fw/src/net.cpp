// net.cpp — roaming WiFi + recovery AP. See net.h for the behaviour contract.
#include "net.h"
#include <WiFi.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <esp_wifi.h>
#include "config.h"
#include "identity.h"    // runtime hostname + unique recovery-AP SSID
#include "control.h"     // one bounded nixie scroll when the recovery AP comes up

namespace Net {
namespace {

const char*    AP_SSID         = "esptube-setup";   // reassigned to Id::apSsid() in begin() (unique per device)
const uint32_t JOIN_TIMEOUT_MS = 15000;    // per candidate network
const uint32_t RETRY_MS        = 60000;    // rescan while nothing known is reachable
const uint32_t LOST_GRACE_MS   = 20000;    // let the driver's auto-reconnect try first
const uint32_t AP_GRACE_MS     = 90000;    // keep the (automatic) AP this long after STA is back
const uint32_t ANNOUNCE_MS     = 45000;    // the nixie scroll stops itself after this
const uint32_t WARN_HOLD_MS    = 60000;    // weak-signal warning: only after the smoothed RSSI has been under the bar this long
const uint32_t WARN_EVERY_MS   = 600000;   // ...and at most once per 10 min
const uint32_t WARN_SHOW_MS    = 9000;     // ...holding the glass this long (one scroll of "WIFI WEAK -82 DBM")

Entry     g_nets[MAX_NETS]; int g_n = 0;
String    g_apPass = "esptube-setup";
String    g_last;                          // ssid that last connected: tried blind (no scan) first
bool      g_blind = false;                 // the current attempt is a blind one
DNSServer g_dns;
bool      g_ap = false, g_apForced = false;
uint32_t  g_apStaSince = 0, g_announceUntil = 0;
int       g_warnDbm = -78;                 // weak-signal nixie warning below this (0 = off; NVS net/warn)
int       g_rssiAvg = 0; uint32_t g_rssiAt = 0, g_weakSince = 0, g_lastWarn = 0;

enum State { S_IDLE, S_SCANNING, S_CONNECTING, S_CONNECTED, S_LOST, S_WAITING };
State    g_state = S_IDLE; uint32_t g_t = 0;
int      g_cand[MAX_NETS]; int g_ncand = 0, g_ci = 0, g_current = -1;
String   g_scan; uint32_t g_scanAt = 0; int g_scanTries = 0;
volatile bool g_gotIp = false, g_dropped = false;

String jsonEsc(const String& s) {
    String o; o.reserve(s.length() + 4);
    for (size_t i = 0; i < s.length(); ++i) { char c = s[i]; if (c == '"' || c == '\\') { o += '\\'; o += c; } else if ((uint8_t)c < 0x20) o += ' '; else o += c; }
    return o;
}

void load() {
    Preferences p; if (!p.begin("net", true)) return;
    g_n = p.getInt("n", 0); if (g_n < 0 || g_n > MAX_NETS) g_n = 0;
    for (int i = 0; i < g_n; ++i) { char k[8]; snprintf(k, sizeof k, "s%d", i); g_nets[i].ssid = p.getString(k, ""); snprintf(k, sizeof k, "p%d", i); g_nets[i].pass = p.getString(k, ""); }
    g_apPass = p.getString("appass", "esptube-setup"); g_last = p.getString("last", ""); g_warnDbm = p.getInt("warn", -78); p.end();
    int w = 0; for (int i = 0; i < g_n; ++i) if (g_nets[i].ssid.length()) g_nets[w++] = g_nets[i]; g_n = w;
}
void save() {
    Preferences p; if (!p.begin("net", false)) return;
    p.clear(); p.putInt("n", g_n);
    for (int i = 0; i < g_n; ++i) { char k[8]; snprintf(k, sizeof k, "s%d", i); p.putString(k, g_nets[i].ssid); snprintf(k, sizeof k, "p%d", i); p.putString(k, g_nets[i].pass); }
    p.putString("appass", g_apPass); p.putString("last", g_last); p.putInt("warn", g_warnDbm); p.end();
}
void saveLast(const String& ssid) { if (g_last == ssid) return; g_last = ssid; Preferences p; if (p.begin("net", false)) { p.putString("last", g_last); p.end(); } }
int find(const String& ssid) { for (int i = 0; i < g_n; ++i) if (g_nets[i].ssid == ssid) return i; return -1; }

// Firmware <= 1.8 used WiFiManager, which left its one network in the WiFi driver's own NVS.
// Import it once so an upgrade never loses the home network.
void importLegacy() {
    if (g_n) return;
    wifi_config_t c; if (esp_wifi_get_config(WIFI_IF_STA, &c) != ESP_OK) return;
    String s((const char*)c.sta.ssid), pw((const char*)c.sta.password); if (!s.length()) return;
    g_nets[0].ssid = s; g_nets[0].pass = pw; g_n = 1; save();
    Serial.printf("[net] imported the previously saved network \"%s\"\n", s.c_str());
}

void onEvent(WiFiEvent_t ev) {
    if (ev == ARDUINO_EVENT_WIFI_STA_GOT_IP) g_gotIp = true;
    else if (ev == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) g_dropped = true;
}
void setState(State s) { g_state = s; g_t = millis(); }
void startScan() { g_blind = false; WiFi.scanDelete(); WiFi.scanNetworks(true, false); setState(S_SCANNING); Serial.println("[net] scanning for known networks"); }
// Try the network that worked last time without scanning: a boot-time scan often misses an AP
// that is right there, and a hidden SSID never shows up in one. Falls through to the scan.
bool tryLastBlind() {
    int k = g_last.length() ? find(g_last) : -1;
    if (k < 0 && g_n) k = 0;                                   // nothing connected yet: the first saved one (the imported home network)
    if (k < 0) return false;
    g_blind = true; g_current = k; g_ncand = 0; g_ci = 0;
    Serial.printf("[net] trying \"%s\" first (%s)\n", g_nets[k].ssid.c_str(), g_last.length() ? "last known good" : "first saved");
    g_gotIp = false; g_dropped = false;
    WiFi.begin(g_nets[k].ssid.c_str(), g_nets[k].pass.length() ? g_nets[k].pass.c_str() : nullptr);
    setState(S_CONNECTING); return true;
}

void buildScanJson(int n) {
    String j = "[";
    for (int i = 0; i < n; ++i) {
        if (i) j += ",";
        j += "{\"ssid\":\""; j += jsonEsc(WiFi.SSID(i)); j += "\",\"rssi\":"; j += WiFi.RSSI(i); j += ",\"ch\":"; j += WiFi.channel(i);
        j += ",\"enc\":"; j += WiFi.encryptionType(i) != WIFI_AUTH_OPEN ? "true" : "false";
        j += ",\"known\":"; j += find(WiFi.SSID(i)) >= 0 ? "true" : "false"; j += "}";
    }
    j += "]"; g_scan = j; g_scanAt = millis();
}
// candidates = saved networks that the scan saw, strongest first
void pickCandidates(int n) {
    int rssi[MAX_NETS]; g_ncand = 0;
    for (int i = 0; i < n; ++i) {
        int k = find(WiFi.SSID(i)); if (k < 0) continue;
        bool dup = false; for (int c = 0; c < g_ncand; ++c) if (g_cand[c] == k) { dup = true; if (WiFi.RSSI(i) > rssi[c]) rssi[c] = WiFi.RSSI(i); }
        if (dup) continue; g_cand[g_ncand] = k; rssi[g_ncand] = WiFi.RSSI(i); g_ncand++;
    }
    for (int a = 0; a < g_ncand; ++a) for (int b = a + 1; b < g_ncand; ++b) if (rssi[b] > rssi[a]) { int t = g_cand[a]; g_cand[a] = g_cand[b]; g_cand[b] = t; t = rssi[a]; rssi[a] = rssi[b]; rssi[b] = t; }
    g_ci = 0;
}

void announce() {
    g_announceUntil = 0;
    if (!strcmp(Control::modeName(), "manual")) { Serial.println("[net] (glass announce skipped: pushed/streamed content is showing)"); return; }
    String m = "SETUP AP "; m += AP_SSID; m += " PASS "; m += g_apPass.length() ? g_apPass : String("NONE"); m += " AT 192.168.4.1"; m.toUpperCase();
    if (Control::nixieText(m.c_str(), "scroll", 250)) g_announceUntil = millis() + ANNOUNCE_MS;
}
void apUp(bool ann) {
    if (g_ap) { if (ann) announce(); return; }
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
    bool ok = WiFi.softAP(AP_SSID, g_apPass.length() >= 8 ? g_apPass.c_str() : nullptr);
    g_dns.setErrorReplyCode(DNSReplyCode::NoError); g_dns.start(53, "*", IPAddress(192, 168, 4, 1));
    g_ap = ok; g_apStaSince = 0;
    Serial.printf("[net] recovery AP \"%s\" %s at 192.168.4.1 (%s)\n", AP_SSID, ok ? "up" : "FAILED", g_apPass.length() >= 8 ? "WPA2" : "open");
    if (ok && ann) announce();
}
void onConnected() {
    Serial.printf("[net] connected to \"%s\" as %s (rssi %d)\n", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
    saveLast(WiFi.SSID()); g_blind = false;
    setState(S_CONNECTED);
    if (g_ap) { g_apStaSince = millis(); if (!g_apForced) Serial.println("[net] recovery AP closes in 90 s"); }
}
void tryNext() {
    if (g_ci >= g_ncand) {
        Serial.printf("[net] no known network reachable (%d saved) — %s\n", g_n, g_ap ? "recovery AP stays up" : "bringing up the recovery AP");
        if (!g_ap) apUp(true);
        setState(S_WAITING); return;
    }
    g_current = g_cand[g_ci++]; const Entry& e = g_nets[g_current];
    Serial.printf("[net] joining \"%s\"\n", e.ssid.c_str());
    g_gotIp = false; g_dropped = false;
    WiFi.begin(e.ssid.c_str(), e.pass.length() ? e.pass.c_str() : nullptr);
    setState(S_CONNECTING);
}

}  // namespace

void begin() {
    AP_SSID = Id::apSsid();               // unique per device: "esptube-setup-<mac3>" (two clocks in recovery are tellable apart)
    WiFi.mode(WIFI_STA);                 // (persistent storage stays on so importLegacy() can read the old creds)
    WiFi.setHostname(Id::hostname());
    WiFi.setSleep(false);                // modem sleep costs ~100 ms per TCP ack — the stream channel needs it off
    WiFi.setAutoReconnect(true);
    WiFi.onEvent(onEvent);
    load(); importLegacy();
    Serial.printf("[net] %d saved network(s)", g_n);
    for (int i = 0; i < g_n; ++i) Serial.printf("%s \"%s\"", i ? "," : ":", g_nets[i].ssid.c_str());
    Serial.println();
    if (!g_n) { Serial.println("[net] nothing saved — recovery AP"); apUp(true); setState(S_WAITING); }
    else if (!tryLastBlind()) startScan();
}

/* Weak-signal watchdog: sample RSSI every 5 s into a ~20 s EMA; once it has sat below g_warnDbm
   for WARN_HOLD_MS, scroll "WIFI WEAK -82 DBM" once — only while the clock draws its own face
   (never over pushed/streamed content, never over another message, never on "off"), at most every
   WARN_EVERY_MS, and pump() hands the glass back after WARN_SHOW_MS like the AP announce. */
void weakTick(uint32_t now) {
    if (now - g_rssiAt < 5000) return; g_rssiAt = now;
    int r = WiFi.RSSI(); if (r >= 0) return;                       // 0 = no reading
    g_rssiAvg = g_rssiAvg ? (g_rssiAvg * 3 + r) / 4 : r;
    if (!g_warnDbm || g_rssiAvg > g_warnDbm) { g_weakSince = 0; return; }
    if (!g_weakSince) { g_weakSince = now; return; }
    if (now - g_weakSince < WARN_HOLD_MS) return;
    if (g_lastWarn && now - g_lastWarn < WARN_EVERY_MS) return;
    g_lastWarn = now;
    Serial.printf("[net] weak signal: %d dBm avg for %lus (warn below %d)\n", g_rssiAvg, (unsigned long)((now - g_weakSince) / 1000), g_warnDbm);
    if (strcmp(Control::modeName(), "clock")) { Serial.println("[net] (glass warning skipped: not on the clock face)"); return; }
    char m[40]; snprintf(m, sizeof m, "WIFI WEAK %d DBM", g_rssiAvg);
    if (Control::nixieText(m, "scroll", 250)) g_announceUntil = now + WARN_SHOW_MS;
}

void pump() {
    uint32_t now = millis();
    if (g_ap) g_dns.processNextRequest();
    if (g_announceUntil && (int32_t)(now - g_announceUntil) >= 0) {           // bounded announce: give the glass back
        g_announceUntil = 0;
        if (!strcmp(Control::modeName(), "nixie") && !strcmp(Control::nixieKindName(), "scroll")) Control::nixieStop();
    }
    switch (g_state) {
        case S_SCANNING: {
            int n = WiFi.scanComplete();
            if (n == WIFI_SCAN_RUNNING) { if (now - g_t > 20000) { WiFi.scanDelete(); startScan(); } break; }
            // a scan started too soon after WiFi init fails (-2) or comes back empty: try again a couple of times
            if (n <= 0 && g_scanTries < 3) { if (now - g_t < 1500) break; g_scanTries++; Serial.printf("[net] scan %s — retry %d\n", n < 0 ? "failed" : "empty", g_scanTries); WiFi.scanDelete(); WiFi.scanNetworks(true, false); g_t = now; break; }
            g_scanTries = 0;
            if (n < 0) n = 0;
            buildScanJson(n); pickCandidates(n);
            Serial.printf("[net] scan: %d network(s), %d known\n", n, g_ncand);
            tryNext(); break; }
        case S_CONNECTING:
            if (g_gotIp || WiFi.status() == WL_CONNECTED) onConnected();
            else if (now - g_t > JOIN_TIMEOUT_MS || (g_dropped && now - g_t > 4000)) {
                Serial.printf("[net] \"%s\" did not let us in\n", g_nets[g_current].ssid.c_str());
                WiFi.disconnect(false, false);
                if (g_blind) startScan(); else tryNext();
            }
            break;
        case S_CONNECTED:
            if (WiFi.status() != WL_CONNECTED) { Serial.println("[net] link lost — waiting for auto-reconnect"); setState(S_LOST); }
            else { if (g_ap && !g_apForced && g_apStaSince && now - g_apStaSince > AP_GRACE_MS) stopAp(); weakTick(now); }
            break;
        case S_LOST:
            if (WiFi.status() == WL_CONNECTED) { Serial.println("[net] link back"); setState(S_CONNECTED); }
            else if (now - g_t > LOST_GRACE_MS) startScan();
            break;
        case S_WAITING:
            if (g_n && now - g_t > RETRY_MS) { static bool flip = false; flip = !flip; if (!(flip && tryLastBlind())) startScan(); }
            break;
        default: break;
    }
}

bool        connected() { return WiFi.status() == WL_CONNECTED; }
String      ssid()      { return connected() ? WiFi.SSID() : String(); }
IPAddress   ip()        { return connected() ? WiFi.localIP() : IPAddress(); }
const char* stateName() {
    switch (g_state) { case S_SCANNING: return "scanning"; case S_CONNECTING: return "connecting"; case S_CONNECTED: return "connected";
                       case S_LOST: return "lost"; case S_WAITING: return "waiting"; default: return "idle"; }
}
bool        apActive()  { return g_ap; }
const char* apSsid()    { return AP_SSID; }
String      apPass()    { return g_apPass; }
IPAddress   apIp()      { return IPAddress(192, 168, 4, 1); }
int         apClients() { return g_ap ? (int)WiFi.softAPgetStationNum() : 0; }
void startAp(bool announce) { g_apForced = true; apUp(announce); }
void stopAp() {
    if (!g_ap) return;
    g_dns.stop(); WiFi.softAPdisconnect(true); WiFi.mode(WIFI_STA);
    g_ap = false; g_apForced = false; g_apStaSince = 0;
    Serial.println("[net] recovery AP closed");
}
void setApPass(const String& pass) { if (pass.length() && pass.length() < 8) return; g_apPass = pass; save(); if (g_ap) { bool f = g_apForced; stopAp(); g_apForced = f; apUp(false); } }

int          count()            { return g_n; }
int          warnDbm()          { return g_warnDbm; }
int          rssiAvg()          { return g_rssiAvg; }
void         setWarnDbm(int d)  { g_warnDbm = (d > -30 || d < -95) ? 0 : d; g_weakSince = 0; save(); Serial.printf("[net] weak-signal warning %s\n", g_warnDbm ? (String("below ") + g_warnDbm + " dBm").c_str() : "off"); }
const Entry& entry(int i)       { static Entry none; return (i >= 0 && i < g_n) ? g_nets[i] : none; }
bool         known(const String& s) { return find(s) >= 0; }
bool add(const String& ssid, const String& pass) {
    if (!ssid.length() || ssid.length() > 32 || pass.length() > 63) return false;
    int k = find(ssid);
    if (k < 0) { if (g_n >= MAX_NETS) { for (int i = 1; i < g_n; ++i) g_nets[i - 1] = g_nets[i]; g_n--; } k = g_n++; }   // oldest falls off
    g_nets[k].ssid = ssid; g_nets[k].pass = pass; save();
    Serial.printf("[net] saved \"%s\" (%d/%d)\n", ssid.c_str(), g_n, MAX_NETS);
    return join(ssid);
}
bool forget(const String& ssid) {
    int k = find(ssid); if (k < 0) return false;
    for (int i = k + 1; i < g_n; ++i) g_nets[i - 1] = g_nets[i]; g_n--; save();
    Serial.printf("[net] forgot \"%s\"\n", ssid.c_str());
    if (connected() && WiFi.SSID() == ssid) reconnect();
    return true;
}
bool join(const String& ssid) {
    int k = find(ssid); if (k < 0) return false;
    g_ncand = 1; g_cand[0] = k; g_ci = 0;
    WiFi.disconnect(false, false); tryNext();
    return true;
}
void reconnect() { WiFi.disconnect(false, false); if (!tryLastBlind()) startScan(); }

int scanSync() {
    int n;
    if (g_state == S_SCANNING) { uint32_t t = millis(); while ((n = WiFi.scanComplete()) == WIFI_SCAN_RUNNING && millis() - t < 8000) delay(50); }
    else n = WiFi.scanNetworks(false, false);
    if (n < 0) n = 0;
    buildScanJson(n); return n;
}
String scanJson() { return g_scan.length() ? g_scan : String("[]"); }

String statusJson() {
    bool c = connected();
    String j = "{\"connected\":"; j += c ? "true" : "false";
    j += ",\"ssid\":\""; j += c ? jsonEsc(WiFi.SSID()) : String(); j += "\",\"ip\":\""; j += c ? WiFi.localIP().toString() : String();
    j += "\",\"rssi\":"; j += c ? (int)WiFi.RSSI() : 0; j += ",\"rssi_avg\":"; j += c ? g_rssiAvg : 0; j += ",\"warn_dbm\":"; j += g_warnDbm; j += ",\"state\":\""; j += stateName(); j += "\",\"hostname\":\""; j += Id::hostname(); j += "\"";
    // ap_pass is deliberately NOT emitted here (net.h contract: "no passwords"). It is a real
    // credential and GET /wifi is unauthenticated; the UART shell (physical access) still prints it.
    j += ",\"ap\":"; j += g_ap ? "true" : "false"; j += ",\"ap_ssid\":\""; j += AP_SSID; j += "\"";
    j += ",\"ap_ip\":\"192.168.4.1\",\"ap_clients\":"; j += apClients();
    j += ",\"saved\":["; for (int i = 0; i < g_n; ++i) { if (i) j += ","; j += "\""; j += jsonEsc(g_nets[i].ssid); j += "\""; } j += "]";
    j += ",\"scan_age_s\":"; j += g_scanAt ? (int)((millis() - g_scanAt) / 1000) : -1; j += ",\"scan\":"; j += scanJson(); j += "}";
    return j;
}
void printStatus(Print& o) {
    if (connected()) o.printf("wifi: connected to \"%s\" ip=%s rssi=%d (avg %d; weak-signal warning %s%d)\n", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(), (int)WiFi.RSSI(), g_rssiAvg, g_warnDbm ? "below " : "off", g_warnDbm ? g_warnDbm : 0);
    else o.printf("wifi: not connected (%s)\n", stateName());
    o.printf("recovery ap: %s — ssid \"%s\" pass \"%s\" at 192.168.4.1%s\n", g_ap ? "UP" : "down", AP_SSID, g_apPass.c_str(), g_ap ? (String(" clients=") + apClients()).c_str() : "");
    o.printf("saved (%d/%d):", g_n, MAX_NETS); for (int i = 0; i < g_n; ++i) o.printf(" \"%s\"", g_nets[i].ssid.c_str()); o.println();
}

}  // namespace Net
