// ============================================================================
// control.cpp — mode state machine, NTP clock rendering, brightness, timezone.
// ============================================================================
#include <atomic>
#include "control.h"
#include "config.h"
#include "tubes.h"
#include "leds.h"
#include "esp1.h"
#include "net.h"
#include "rtc.h"
#include <WiFi.h>
#include <esp_sntp.h>

#include <Preferences.h>
#include <time.h>

namespace {

Control::Mode   g_mode       = Control::Mode::Clock;   // display layer
Control::Mode   g_lastNonOff = Control::Mode::Clock;   // for POWER toggle
Control::Effect g_effect     = Control::Effect::Off;   // LED layer (independent)
uint8_t g_solidR = 255, g_solidG = 140, g_solidB = 40; // warm default for SOLID
uint8_t g_bootEffect       = 0;    // saved LED effect, restored after the preset in begin()
uint8_t g_bright           = 180;
String  g_tz               = "PST8PDT,M3.2.0,M11.1.0";   // US Pacific default

// Device-native presets: firmware-rendered faces the clock cycles by itself
// (short MODE / POST /preset). 0 = Wide Nixie clock (default).
uint8_t g_preset     = 0;
bool    g_clockNixie = true;    // clock digit style for the current preset

Preferences g_prefs;
const char* NVS_NS   = "esptube";
const char* K_BRT    = "bright";
const char* K_TZ     = "tz";
const char* K_PRESET = "preset";
const char* K_EFFECT = "effect";
const char* K_SOLR   = "solr";
const char* K_SOLG   = "solg";
const char* K_SOLB   = "solb";

uint16_t g_idleMin = 30;    // minutes of no content before MANUAL hands back to the clock (0 = never)
char g_shown[TUBE_COUNT];   // last char drawn per tube (0 => force redraw)
int  g_lastSec = -1;
bool g_repaint = false;     // repaint shown digits in place (no fade) — LUT changed

volatile bool g_rtcSave = false;   // set from the SNTP task; the RTC write happens in tick()
void applyNtp() {
    // Non-blocking: starts SNTP; time becomes valid once the network syncs.
    // Every sync is written back to the DS1302 so the clock is right offline next time.
    esp_sntp_set_time_sync_notification_cb([](struct timeval*) { g_rtcSave = true; });
    configTzTime(g_tz.c_str(), "pool.ntp.org", "time.nist.gov");
}

void loadCfg() {
    if (g_prefs.begin(NVS_NS, /*readOnly=*/true)) {
        g_bright = g_prefs.getUChar(K_BRT, 180);
        g_tz     = g_prefs.getString(K_TZ, g_tz);
        g_preset = g_prefs.getUChar(K_PRESET, 0);   // default = Wide Nixie
        g_bootEffect = g_prefs.getUChar(K_EFFECT, 0);   // default = off
        g_idleMin    = g_prefs.getUShort("idle", 30);
        if (g_bootEffect > (uint8_t)Control::Effect::Comet) g_bootEffect = 0;
        g_solidR = g_prefs.getUChar(K_SOLR, g_solidR);
        g_solidG = g_prefs.getUChar(K_SOLG, g_solidG);
        g_solidB = g_prefs.getUChar(K_SOLB, g_solidB);
        g_prefs.end();
    }
}
void saveBrightness(uint8_t v) {
    if (g_prefs.begin(NVS_NS, false)) { g_prefs.putUChar(K_BRT, v); g_prefs.end(); }
}
void savePreset(uint8_t p) {
    if (g_prefs.begin(NVS_NS, false)) { g_prefs.putUChar(K_PRESET, p); g_prefs.end(); }
}
void saveEffect(uint8_t e) {
    if (g_prefs.begin(NVS_NS, false)) { g_prefs.putUChar(K_EFFECT, e); g_prefs.end(); }
}
void saveSolid(uint8_t r, uint8_t g, uint8_t b) {
    if (g_prefs.begin(NVS_NS, false)) {
        g_prefs.putUChar(K_SOLR, r); g_prefs.putUChar(K_SOLG, g); g_prefs.putUChar(K_SOLB, b);
        g_prefs.end();
    }
}
void saveTz(const String& t) {
    if (g_prefs.begin(NVS_NS, false)) { g_prefs.putString(K_TZ, t); g_prefs.end(); }
}

void forceRedraw() {
    for (uint8_t i = 0; i < TUBE_COUNT; ++i) g_shown[i] = 0;
    g_lastSec = -1;
}

void applyBrightness(uint8_t v) {   // set + push to LEDs + nixie LUT (no NVS write)
    g_bright = v;
    Leds::setBrightness(v);
    Leds::show();
    Tubes::setNixieBrightness(v);
    g_repaint = true;               // nixie face dims/brightens visibly
}

// ---- On-device animations -------------------------------------------------
// Autonomous WS2812 animations stepped from tick() at ~ANIM_MS. Panels stay
// blank; brightness scaling happens in Leds::show(), so animations set RAW
// (full-range) colors and the global brightness still applies.
const uint32_t ANIM_MS = 40;
uint32_t g_animMs   = 0;
uint16_t g_animPhase = 0;   // advances each animation step

// Compact HSV->RGB (h,s,v all 0..255).
void hsv2rgb(uint8_t h, uint8_t s, uint8_t v, uint8_t& r, uint8_t& g, uint8_t& b) {
    uint8_t region = h / 43;
    uint8_t rem    = (h - region * 43) * 6;
    uint8_t p = (uint16_t)v * (255 - s) / 255;
    uint8_t q = (uint16_t)v * (255 - ((uint16_t)s * rem) / 255) / 255;
    uint8_t t = (uint16_t)v * (255 - ((uint16_t)s * (255 - rem)) / 255) / 255;
    switch (region) {
        case 0:  r = v; g = t; b = p; break;
        case 1:  r = q; g = v; b = p; break;
        case 2:  r = p; g = v; b = t; break;
        case 3:  r = p; g = q; b = v; break;
        case 4:  r = t; g = p; b = v; break;
        default: r = v; g = p; b = q; break;
    }
}

void animRainbow() {
    for (uint8_t i = 0; i < LED_COUNT; ++i) {
        uint8_t h = (uint8_t)(g_animPhase + i * (256 / LED_COUNT));
        uint8_t r, g, b; hsv2rgb(h, 255, 255, r, g, b);
        Leds::setTube(i, r, g, b);
    }
    Leds::show();
    g_animPhase += 3;
}

void animBreathe() {
    // Triangle envelope 0..255..0; hue drifts slowly.
    uint8_t tri = (g_animPhase & 0x100) ? (255 - (g_animPhase & 0xFF)) : (g_animPhase & 0xFF);
    uint8_t hue = (uint8_t)(g_animPhase >> 2);
    uint8_t r, g, b; hsv2rgb(hue, 255, tri, r, g, b);
    Leds::all(r, g, b);
    Leds::show();
    g_animPhase += 3;
}

void animComet() {
    // A bright head runs across the LEDs leaving a fading tail. Head steps once
    // per few ticks; each LED's value falls off with distance behind the head.
    const uint8_t span = LED_COUNT * 4;          // sub-steps for a slow sweep
    uint8_t head = (g_animPhase / 4) % LED_COUNT;
    uint8_t hue  = (uint8_t)(g_animPhase >> 1);
    for (uint8_t i = 0; i < LED_COUNT; ++i) {
        int dist = (int)head - (int)i;
        if (dist < 0) dist += LED_COUNT;          // wrap: tail trails behind head
        int v = 255 - dist * 90;                  // fade per LED
        if (v < 0) v = 0;
        uint8_t r, g, b; hsv2rgb(hue, 255, (uint8_t)v, r, g, b);
        Leds::setTube(i, r, g, b);
    }
    Leds::show();
    g_animPhase = (g_animPhase + 1) % (span * 64);
}

}  // namespace

namespace Control {

static void applyEffect(Effect e);   // set + paint the underglow, no NVS write (below)

void begin() {
    loadCfg();
    Leds::setBrightness(g_bright);
    Tubes::setNixieBrightness(g_bright);
    Leds::off();                 // LED effect starts OFF
    applyNtp();
    // No WiFi needed to show the right time: seed the system clock from the DS1302 (UTC; the
    // TZ set above does local time). NTP corrects it later and writes the chip back.
    Rtc::begin(PIN_RTC_CE, PIN_RTC_IO, PIN_RTC_SCLK);
    if (!timeValid()) Rtc::seedSystemTime();
    // Deterministic boot: display always starts in CLOCK (not persisted, no init
    // path pushes content); LED effect starts OFF. MANUAL is only ever entered
    // later by a REST content push.
    g_lastNonOff = Mode::Clock;
    applyPreset(g_preset, /*persist=*/false);   // boot into the saved preset (default 0 = Wide Nixie)
    applyEffect((Effect)g_bootEffect);          // the saved underglow wins over the preset's default
    Serial.printf("[control] begin: preset=%u (%s) effect=%s bright=%u tz=%s\n",
                  g_preset, presetName(g_preset), effectName(), g_bright, g_tz.c_str());
}

// ---- Display layer --------------------------------------------------------
Mode mode() { return g_mode; }

const char* modeName() {
    switch (g_mode) {
        case Mode::Clock: return "clock";
        case Mode::Off:   return "off";
        case Mode::Nixie: return "nixie";
        default:          return "manual";
    }
}

uint8_t brightness() { return g_bright; }
String  tz()         { return g_tz; }

bool timeValid() {
    time_t now = time(nullptr);
    struct tm t;
    localtime_r(&now, &t);
    return (t.tm_year + 1900) > 2020;
}

void setBrightness(uint8_t v) {
    applyBrightness(v);
    saveBrightness(v);
    Serial.printf("[control] brightness=%u\n", v);
}

void setTz(const String& t) {
    g_tz = t;
    saveTz(t);
    applyNtp();
    forceRedraw();
    Serial.printf("[control] tz=%s\n", t.c_str());
}

void setMode(Mode m) {
    Tubes::supersede();          // a face change makes any frame still arriving stale (ESP/1.1)
    g_mode = m;
    if (m != Mode::Off) g_lastNonOff = m;

    // Panels only — the LED-effect layer is independent and untouched here.
    if (m == Mode::Off)        Tubes::clearAll();
    else if (m == Mode::Clock) { Tubes::setNixieFadeSteps(4); forceRedraw(); }   // repaint clock on next tick
    else if (m == Mode::Nixie) forceRedraw();   // message engine repaints on next tick
    // MANUAL: leave whatever content is on the panels.
    Serial.printf("[control] mode=%s\n", modeName());
}

bool setModeByName(const char* name) {
    if (!strcmp(name, "clock"))  { setMode(Mode::Clock);  return true; }
    if (!strcmp(name, "off"))    { setMode(Mode::Off);    return true; }
    if (!strcmp(name, "manual")) { setMode(Mode::Manual); return true; }
    return false;
}

// ---- LED-effect layer -----------------------------------------------------
Effect effect() { return g_effect; }

const char* effectName() {
    switch (g_effect) {
        case Effect::Off:     return "off";
        case Effect::Solid:   return "solid";
        case Effect::Rainbow: return "rainbow";
        case Effect::Breathe: return "breathe";
        default:              return "comet";
    }
}

void solidColor(uint8_t& r, uint8_t& g, uint8_t& b) { r = g_solidR; g = g_solidG; b = g_solidB; }

static void applyEffect(Effect e) {   // set + paint (no NVS write)
    g_effect = e;
    g_animPhase = 0;
    g_animMs = 0;                 // step immediately on next tick
    if (e == Effect::Off) {
        Leds::off();              // clear underglow
    } else if (e == Effect::Solid) {
        Leds::all(g_solidR, g_solidG, g_solidB);
        Leds::show();
    }
    // Animated effects paint on the next tick.
    Serial.printf("[control] led effect=%s\n", effectName());
}

void setEffect(Effect e) {
    applyEffect(e);
    saveEffect((uint8_t)e);       // survives a power-cycle
}

void setSolidColor(uint8_t r, uint8_t g, uint8_t b) {
    g_solidR = r; g_solidG = g; g_solidB = b;
    saveSolid(r, g, b);
    setEffect(Effect::Solid);
}

bool setEffectByName(const char* name) {
    if (!strcmp(name, "off"))     { setEffect(Effect::Off);     return true; }
    if (!strcmp(name, "solid"))   { setEffect(Effect::Solid);   return true; }
    if (!strcmp(name, "rainbow")) { setEffect(Effect::Rainbow); return true; }
    if (!strcmp(name, "breathe")) { setEffect(Effect::Breathe); return true; }
    if (!strcmp(name, "comet"))   { setEffect(Effect::Comet);   return true; }
    return false;
}

// MODE button cycle: off -> rainbow -> breathe -> comet -> solid -> off.
void cycleEffect() {
    static const Effect CYCLE[] = { Effect::Off, Effect::Rainbow, Effect::Breathe,
                                    Effect::Comet, Effect::Solid };
    const int N = sizeof(CYCLE) / sizeof(CYCLE[0]);
    int idx = 0;
    for (int k = 0; k < N; ++k) if (CYCLE[k] == g_effect) { idx = k; break; }
    setEffect(CYCLE[(idx + 1) % N]);
}

static uint32_t g_lastContentMs = 0;      // last pushed/streamed frame — the idle timer counts from here
void notifyManualContent() {
    g_lastContentMs = millis();
    if (g_mode != Mode::Manual) {
        g_mode = Mode::Manual;
        g_lastNonOff = Mode::Manual;
        Serial.println("[control] content pushed -> mode=manual");
    }
}
static std::atomic<bool> g_mcPending{false};
void masterClear() {
    Tubes::supersede();
    clockSweepStop();
    setEffectByName("off");
    notifyManualContent();        // stay cleared; don't let the clock redraw
    Tubes::clearAll();
    Leds::off();
    Serial.println("[control] master clear");
}
void masterClearFromTask(bool bumpEpoch) {
    if (bumpEpoch) Tubes::supersede();
    notifyManualContent();
    Tubes::clearAll();
    Leds::off();
    g_mcPending = true;           // sweep stop + effect off (NVS) belong to loop(): next tick()
}
uint16_t idleMinutes() { return g_idleMin; }
void setIdleMinutes(uint16_t m) { g_idleMin = m; if (g_prefs.begin(NVS_NS, false)) { g_prefs.putUShort("idle", m); g_prefs.end(); } Serial.printf("[control] idle return = %u min\n", m); }
// Day-to-day rule: pushed content that nobody refreshes is stale content. After idle_min minutes with
// no push/stream frame, MANUAL goes back to the clock face by itself (the menu counts as content).
static void idleTick() {
    if (g_mode != Mode::Manual || !g_idleMin || !g_lastContentMs) return;
    if ((uint32_t)(millis() - g_lastContentMs) < (uint32_t)g_idleMin * 60000UL) return;
    g_lastContentMs = 0; Serial.printf("[control] no content for %u min — back to the clock\n", g_idleMin);
    setMode(Mode::Clock);
}

// ---- Device-native presets ------------------------------------------------
// Firmware-rendered faces the clock cycles by itself (short MODE / POST /preset).
// The web mirrors + drives the same index. Rich pixel scenes stay a helper push.
static const uint8_t PRESET_N = 5;
const char* presetName(uint8_t p) {
    switch (p) { case 0: return "nixie"; case 1: return "digital";
                 case 2: return "ledshow"; case 4: return "date"; default: return "off"; }
}
uint8_t preset()      { return g_preset; }
uint8_t presetCount() { return PRESET_N; }

// Preset 4 (date): today's date as a static nixie message, e.g. "SEP17" (3-letter
// month + 2-digit day). nixieTick() re-renders it when the local day changes.
static int  g_dateYday = -1;      // day-of-year rendered (-1 = "NTP?" shown)
static bool g_dateFace = false;   // the current nixie message IS the date face
static void showDate() {
    char s[8] = "NTP?";
    g_dateYday = -1;
    if (timeValid()) {
        time_t now = time(nullptr); struct tm t; localtime_r(&now, &t);
        strftime(s, sizeof s, "%b%d", &t);   // "Sep17" -> sanitizeNixie folds to upper
        g_dateYday = t.tm_yday;
    }
    nixieText(s, "static", 500);  // Mode::Nixie, left-aligned on the live tubes
    g_dateFace = true;            // (nixieText clears it; set after)
}

void applyPreset(uint8_t p, bool persist) {
    g_preset = p % PRESET_N;
    switch (g_preset) {
        case 0: g_clockNixie = true;  applyEffect(Effect::Off);   setMode(Mode::Clock); break; // Wide Nixie
        case 1: g_clockNixie = false; applyEffect(Effect::Off);   setMode(Mode::Clock); break; // Big digital
        case 2: g_clockNixie = true;  applyEffect(Effect::Comet); setMode(Mode::Clock); break; // LED show
        case 4: g_clockNixie = true;  applyEffect(Effect::Off);   showDate();           break; // Date (no setMode: it would wipe the text)
        default:                      applyEffect(Effect::Off);   setMode(Mode::Off);   break; // Off
    }
    if (persist) { savePreset(g_preset); saveEffect((uint8_t)g_effect); }
    Serial.printf("[control] preset=%u (%s)\n", g_preset, presetName(g_preset));
}
void nextPreset() { applyPreset((uint8_t)((g_preset + 1) % PRESET_N), true); }
bool setPresetByIndex(int i) { if (i < 0 || i >= PRESET_N) return false; applyPreset((uint8_t)i, true); return true; }

// ---- On-device menu (v2): nixie-letter labels, no browser needed ---------------
// Click-only, context-driven (buttons.cpp sends one click per press; short/long is unreliable on
// this board). From the clock MODE opens the menu. In the list: UP/DOWN move, MODE selects/enters,
// POWER backs out (at the top = close). Selecting FACE/LED/FPS opens that item in place (you STAY in
// the menu): UP/DOWN change the value, MODE confirms + returns to the list, POWER cancels.
// The whole item list is dumped to the serial console on open (openMenu).
// Items: FACE (device face) · LED (underglow effect) · BRI+ / BRI- ·
// WIFI (scrolls the IP in nixie glyphs) · TEST (midnight clock-sweep slice) ·
// FPS (stream frame rate) · BOOT (restart) · EXIT.
static void showChars(const char* per_tube, bool nixie);
static void layoutLeft(const char* s, int offset, char out[TUBE_COUNT]);
static bool g_menuOpen = false;
static int  g_menuIdx  = 0;
static int  g_menuEdit = -1;   // -1 = browsing the list; else the M_* item open for in-place adjust
static int  g_faceSel  = 0;    // FACE editor: preset index being previewed (applied on MODE-confirm)
enum MenuAction { M_FACE, M_LED, M_BRIGHT_UP, M_BRIGHT_DN, M_WIFI, M_AP, M_TEST, M_FPS, M_BOOT, M_EXIT };
struct MenuEntry { const char* label; int action; };
static const MenuEntry MENU[] = {
    {"FACE", M_FACE}, {"LED", M_LED}, {"BRI+", M_BRIGHT_UP}, {"BRI-", M_BRIGHT_DN}, {"WIFI", M_WIFI}, {"AP", M_AP},
    {"TEST", M_TEST}, {"FPS", M_FPS}, {"BOOT", M_BOOT}, {"EXIT", M_EXIT} };
static const int MENU_N = (int)(sizeof(MENU) / sizeof(MENU[0]));

static void drawMenu() {
    char out[TUBE_COUNT]; layoutLeft(MENU[g_menuIdx].label, 0, out);
    showChars(out, true);
    Serial.printf("[menu] item=%s\n", MENU[g_menuIdx].label);
}
static void openMenu() {
    g_menuOpen = true; g_menuIdx = 0; g_menuEdit = -1;
    g_mode = Mode::Manual;         // stop the clock repainting under the menu
    Tubes::setNixieFadeSteps(2); forceRedraw(); drawMenu();
    Serial.println("[menu] open (UP/DOWN move · MODE select · POWER back) — items:");
    for (int i = 0; i < MENU_N; ++i) Serial.printf("[menu]   %d %s\n", i, MENU[i].label);
}
static void closeMenu(bool toClock = true) {
    g_menuOpen = false; g_menuEdit = -1;
    if (toClock) setMode(Mode::Clock);   // restores the 4-frame fade too
    Serial.println("[control] menu close");
}
static void menuNav(int dir) { g_menuIdx = (g_menuIdx + dir + MENU_N) % MENU_N; drawMenu(); }

// ---- In-place item editor: select an item to STAY in it and navigate its options ----
// (MODE used to fire a one-shot and drop out of the menu; you couldn't scroll faces.)
static void menuShowLabel(const char* s) { char out[TUBE_COUNT]; layoutLeft(s, 0, out); showChars(out, true); }
static void menuShowFps() { char b[16]; snprintf(b, sizeof b, "%d FPS", (int)(Esp1::stats().fps + 0.5f)); menuShowLabel(b); }
// FACE navigates by NAME — a clear menu label. (Applying the preset live made "editing FACE" look
// identical to a closed menu, so MODE felt like it just exited.) UP/DOWN scroll the name; MODE applies.
static const char* faceShort(uint8_t p) {
    switch (p) { case 0: return "NIXIE"; case 1: return "DIGITL"; case 2: return "LEDSHW"; case 4: return "DATE"; default: return "OFF"; }
}
static void menuEditEnter(int action) {
    g_menuEdit = action;
    Serial.printf("[menu] enter %s\n", MENU[g_menuIdx].label);
    switch (action) {
        case M_FACE: g_faceSel = g_preset; menuShowLabel(faceShort((uint8_t)g_faceSel)); break;   // NAME label; not applied until MODE
        case M_LED:  break;                                                                       // keep the "LED" label; the underglow is the feedback
        case M_FPS:  menuShowFps(); break;
    }
}
static void menuEditStep(int dir) {
    switch (g_menuEdit) {
        case M_FACE: g_faceSel = (g_faceSel + dir + PRESET_N) % PRESET_N; menuShowLabel(faceShort((uint8_t)g_faceSel)); break;
        case M_LED:  cycleEffect(); break;                                                        // forward-only; UP and DOWN both advance the glow
        case M_FPS:  menuShowFps(); break;                                                        // refresh the live reading
    }
}
static void menuEditExit(bool apply) {                                                            // MODE = confirm+apply, POWER = cancel; both step back to the list
    if (apply && g_menuEdit == M_FACE) applyPreset((uint8_t)g_faceSel, true);   // persist the chosen face (and its preset-default effect)
    // LED persisted itself via cycleEffect(); FPS is read-only
    g_menuEdit = -1; g_mode = Mode::Manual; Tubes::setNixieFadeSteps(2); forceRedraw(); drawMenu();
}

static void menuSelect() {
    switch (MENU[g_menuIdx].action) {
        case M_FACE:      menuEditEnter(M_FACE); break;   // live: UP/DOWN scroll faces, MODE/POWER back to the list
        case M_LED:       menuEditEnter(M_LED);  break;   // live: UP/DOWN cycle the underglow, MODE/POWER back
        case M_BRIGHT_UP: { int v = (int)g_bright + 32; if (v > 255) v = 255; setBrightness((uint8_t)v); forceRedraw(); drawMenu(); break; }
        case M_BRIGHT_DN: { int v = (int)g_bright - 32; if (v < 0)   v = 0;   setBrightness((uint8_t)v); forceRedraw(); drawMenu(); break; }
        case M_WIFI: {    // scroll the address; short MODE afterwards returns to a face
            g_menuOpen = false; Tubes::setNixieFadeSteps(4);
            String ip = Net::connected() ? Net::ip().toString()
                      : Net::apActive() ? String("NO WIFI - AP ") + Net::apSsid() + " AT 192.168.4.1" : String("NO WIFI");
            nixieText(ip.c_str(), "scroll", 300); break; }
        case M_AP: {      // WiFi recovery: host our own network so a phone can teach the clock a new WiFi
            g_menuOpen = false; Tubes::setNixieFadeSteps(4);
            Serial.println("[control] menu: recovery AP"); Net::startAp(true); break; }
        case M_TEST:      g_menuOpen = false; Tubes::setNixieFadeSteps(4); clockSweepStart(86390, 5, 0, true); break;
        case M_FPS:       menuEditEnter(M_FPS);  break;   // shows the live stream fps; UP/DOWN refresh, MODE/POWER back
        case M_BOOT:      Serial.println("[control] menu reboot"); delay(50); ESP.restart(); break;
        case M_EXIT:      closeMenu(true); break;
    }
}

// Click-only, context-driven (buttons.cpp delivers one click per press — short vs long is not reliable
// on this board, so the menu never depends on it). MODE = open/select/confirm, POWER = back/close,
// UP/DOWN = move/adjust. `longPress` is ignored.
void onButton(Button b, bool /*longPress*/) {
    if (!g_menuOpen) {                                  // ---- clock face ----
        switch (b) {
            case Button::Mode:  openMenu(); break;                  // MODE opens the menu (faces live inside it now)
            case Button::Power: setMode(g_mode == Mode::Off ? g_lastNonOff : Mode::Off); break;
            case Button::Up:   { int v = (int)g_bright + 32; if (v > 255) v = 255; setBrightness((uint8_t)v); break; }
            case Button::Down: { int v = (int)g_bright - 32; if (v < 0)   v = 0;   setBrightness((uint8_t)v); break; }
        }
        return;
    }
    if (g_menuEdit >= 0) {                              // ---- inside an item ----
        switch (b) {
            case Button::Up:    menuEditStep(-1);    break;
            case Button::Down:  menuEditStep(+1);    break;
            case Button::Mode:  menuEditExit(true);  break;         // confirm / apply, back to the list
            case Button::Power: menuEditExit(false); break;         // cancel, back to the list
        }
        return;
    }
    switch (b) {                                        // ---- menu list ----
        case Button::Up:    menuNav(-1);     break;
        case Button::Down:  menuNav(+1);     break;
        case Button::Mode:  menuSelect();    break;                 // enter / activate the item
        case Button::Power: closeMenu(true); break;                 // back out of the menu
    }
}

void forceClockRedraw() { forceRedraw(); }

// ---- Shared glyph renderer: show one char per tube, fading only what changed ----
// per_tube[i] is what NATIVE tube i should show (0 or ' ' = unlit). Nixie glyphs
// cross-fade together; the plain digital face draws directly.
static void showChars(const char* per_tube, bool nixie) {
    uint8_t chg[TUBE_COUNT]; char from[TUBE_COUNT], to[TUBE_COUNT]; uint8_t n = 0;
    for (uint8_t i = 0; i < TUBE_COUNT; ++i) {
        if (!Tubes::alive(i)) continue;
        char c = per_tube[i] ? per_tube[i] : ' ';
        if (g_shown[i] == c) continue;
        if (nixie) { chg[n] = i; from[n] = g_shown[i]; to[n] = c; n++; }
        else       Tubes::drawGlyph(i, c);
        g_shown[i] = c;
    }
    if (n) Tubes::drawGlyphsNixieFade(chg, from, to, n);
}
// Positional (clock-style): tube i shows s6[5-i]; unpopulated tubes keep their slot.
static void layoutRight(const char* s6, char out[TUBE_COUNT]) {
    for (uint8_t i = 0; i < TUBE_COUNT; ++i) out[i] = s6[5 - i];
}
// Right-aligned across the POPULATED tubes (last char on the rightmost live tube).
// Used for the countdown so its seconds never land in an empty slot.
static void layoutRightAlive(const char* s, char out[TUBE_COUNT]) {
    const int len = (int)strlen(s); int k = 0;
    for (int i = 0; i < TUBE_COUNT; ++i) {
        out[i] = ' ';
        if (!Tubes::alive((uint8_t)i)) continue;
        int p = len - 1 - k++;
        if (p >= 0) out[i] = s[p];
    }
}
// Left-aligned text across the POPULATED tubes (far-left first), from offset.
static void layoutLeft(const char* s, int offset, char out[TUBE_COUNT]) {
    const int len = (int)strlen(s); int k = 0;
    for (int i = TUBE_COUNT - 1; i >= 0; --i) {
        out[i] = ' ';
        if (!Tubes::alive((uint8_t)i)) continue;
        int p = offset + k++;
        if (p >= 0 && p < len) out[i] = s[p];
    }
}
static int populatedCount() { int n = 0; for (uint8_t i = 0; i < TUBE_COUNT; ++i) if (Tubes::alive(i)) n++; return n; }

// ---- Nixie message engine (Mode::Nixie) ----------------------------------------
static struct {
    NixieKind kind = NixieKind::Static;
    char      text[96] = {0};
    uint32_t  ms = 500, nextAt = 0, endAtMs = 0;
    int       pos = 0;              // scroll offset
    bool      on = true;            // flash phase
    int32_t   lastRem = -1;         // countdown: last rendered remaining seconds
    uint8_t   doneFlashes = 0;      // countdown: end-of-count flashes emitted
    bool      dirty = true;         // (re)render on next tick
} g_nx;

// UTF-8 -> glyph chars: fold case, '°' -> NIXIE_CH_DEGREE, drop other multi-byte.
static void sanitizeNixie(const char* in, char* out, size_t cap) {
    size_t o = 0;
    for (size_t i = 0; in[i] && o + 1 < cap; ) {
        uint8_t b = (uint8_t)in[i];
        if (b == 0xC2 && (uint8_t)in[i + 1] == 0xB0) { out[o++] = NIXIE_CH_DEGREE; i += 2; continue; }
        if (b >= 0x80) { i++; while ((uint8_t)in[i] >= 0x80 && (uint8_t)in[i] < 0xC0) i++; out[o++] = ' '; continue; }
        char c = (char)b; if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        if (c == '\n' || c == '\t') c = ' ';
        out[o++] = c; i++;
    }
    out[o] = 0;
}

bool nixieText(const char* utf8, const char* effect, uint32_t ms) {
    NixieKind k;
    if (!effect || !*effect || !strcmp(effect, "static")) k = NixieKind::Static;
    else if (!strcmp(effect, "flash"))  k = NixieKind::Flash;
    else if (!strcmp(effect, "scroll")) k = NixieKind::Scroll;
    else return false;
    sanitizeNixie(utf8 ? utf8 : "", g_nx.text, sizeof g_nx.text);
    g_dateFace = false;           // an explicit message replaces the preset-4 date face
    g_nx.kind = k; g_nx.ms = ms < 60 ? 60 : ms; g_nx.pos = -populatedCount(); g_nx.on = true;
    g_nx.nextAt = millis() + g_nx.ms; g_nx.dirty = true;
    // A flash/scroll step re-fades every tube (~26 ms × frames × tubes), so a brisk
    // pace gets 2 frames instead of 4; the clock face gets its 4 back on exit.
    Tubes::setNixieFadeSteps((k != NixieKind::Static && g_nx.ms < 600) ? 2 : 4);
    g_mode = Mode::Nixie; g_lastNonOff = Mode::Nixie; forceRedraw();
    Serial.printf("[nixie] %s \"%s\" ms=%u\n", effect ? effect : "static", g_nx.text, (unsigned)g_nx.ms);
    return true;
}
void nixieCountdown(uint32_t seconds) {
    if (seconds > 359999u) seconds = 359999u;          // 99:59:59
    g_nx.kind = NixieKind::Countdown; g_nx.endAtMs = millis() + seconds * 1000u + 999u;
    g_nx.lastRem = -1; g_nx.doneFlashes = 0; g_nx.on = true; g_nx.ms = 500; g_nx.dirty = true;
    g_nx.text[0] = 0;
    g_mode = Mode::Nixie; g_lastNonOff = Mode::Nixie; forceRedraw();
    Serial.printf("[nixie] countdown %u s\n", (unsigned)seconds);
}
void nixieStop() { if (g_mode == Mode::Nixie) setMode(Mode::Clock); }
// (setMode(Clock) restores the 4-frame fade — see below.)
const char* nixieKindName() {
    switch (g_nx.kind) { case NixieKind::Flash: return "flash"; case NixieKind::Scroll: return "scroll";
                         case NixieKind::Countdown: return "countdown"; default: return "static"; }
}
const char* nixieMessage() { return g_nx.text; }
uint32_t    nixieMs()      { return g_nx.ms; }
int32_t     nixieRemaining() {
    if (g_nx.kind != NixieKind::Countdown) return 0;
    int32_t rem = (int32_t)((g_nx.endAtMs - millis()) / 1000u);
    return ((int32_t)(g_nx.endAtMs - millis()) < 0) ? 0 : rem;
}

static void nixieTick() {
    // Preset 4: re-render the date once the day rolls over (or NTP first syncs).
    if (g_preset == 4 && g_dateFace && g_nx.kind == NixieKind::Static && timeValid()) {
        time_t t0 = time(nullptr); struct tm t; localtime_r(&t0, &t);
        if (t.tm_yday != g_dateYday) showDate();
    }
    char out[TUBE_COUNT];
    const uint32_t now = millis();
    switch (g_nx.kind) {
        case NixieKind::Static:
            // No dirty gate: showChars() only touches tubes whose glyph differs, so this
            // also heals a forceRedraw() (POWER off/on, tz change) that blanked g_shown.
            layoutLeft(g_nx.text, 0, out); break;
        case NixieKind::Flash:
            if (!g_nx.dirty && (int32_t)(now - g_nx.nextAt) < 0) return;
            if (!g_nx.dirty) { g_nx.on = !g_nx.on; g_nx.nextAt = now + g_nx.ms; }
            if (g_nx.on) layoutLeft(g_nx.text, 0, out); else memset(out, ' ', sizeof out);
            break;
        case NixieKind::Scroll: {
            if (!g_nx.dirty && (int32_t)(now - g_nx.nextAt) < 0) return;
            if (!g_nx.dirty) { g_nx.nextAt = now + g_nx.ms; g_nx.pos++; }
            const int len = (int)strlen(g_nx.text), n = populatedCount();
            if (g_nx.pos > len) g_nx.pos = -n;                 // wrapped past the end: re-enter from the right
            layoutLeft(g_nx.text, g_nx.pos, out); break; }
        case NixieKind::Countdown: {
            const int32_t left = (int32_t)(g_nx.endAtMs - now);
            if (left > 0) {
                const int32_t rem = left / 1000;
                if (!g_nx.dirty && rem == g_nx.lastRem) return;
                g_nx.lastRem = rem;
                // HMMSS / MMSS, right-aligned on the LIVE tubes (the seconds must be visible)
                char s6[8]; const int h = rem / 3600, m = (rem / 60) % 60, s = rem % 60;
                if (h > 0) snprintf(s6, sizeof s6, "%d%02d%02d", h > 99 ? 99 : h, m, s);
                else       snprintf(s6, sizeof s6, "%02d%02d", m, s);
                layoutRightAlive(s6, out);
            } else {                                            // finished: flash 0000, then back to the clock
                if (!g_nx.dirty && (int32_t)(now - g_nx.nextAt) < 0) return;
                if (!g_nx.dirty) { g_nx.on = !g_nx.on; g_nx.doneFlashes++; }
                g_nx.nextAt = now + g_nx.ms;
                if (g_nx.doneFlashes >= 8) { setMode(Mode::Clock); return; }
                if (g_nx.on) layoutRightAlive("0000", out); else memset(out, ' ', sizeof out);
            }
            break; }
    }
    g_nx.dirty = false;
    showChars(out, true);
}

// ---- Clock sweep test ------------------------------------------------------------
static struct {
    bool active = false, fade = true; uint32_t from = 0, to = 0, sec = 0, done = 0, total = 0, maxDrawUs = 0, nextAt = 0;
    uint16_t stepMs = 0; uint8_t savedSteps = 4;
    uint32_t announceUntil = 0; bool announced = false;   // "TEST" shown on the glass before the digits race
} g_sw;
void clockSweepStart(uint32_t fromSec, uint32_t toSec, uint16_t stepMs, bool fade) {
    fromSec %= 86400u; toSec %= 86400u;
    g_sw.from = fromSec; g_sw.to = toSec; g_sw.sec = fromSec; g_sw.stepMs = stepMs; g_sw.fade = fade;
    g_sw.done = 0; g_sw.maxDrawUs = 0; g_sw.nextAt = 0;
    g_sw.announceUntil = millis() + 1500; g_sw.announced = false;
    g_sw.total = (toSec >= fromSec) ? (toSec - fromSec + 1) : (86400u - fromSec + toSec + 1);
    if (!g_sw.active) g_sw.savedSteps = Tubes::nixieFadeSteps();
    Tubes::setNixieFadeSteps(fade ? g_sw.savedSteps : 1);
    g_sw.active = true;
    g_mode = Mode::Clock; g_lastNonOff = Mode::Clock; forceRedraw();
    Serial.printf("[sweep] %u..%u step=%ums fade=%d (%u steps)\n", (unsigned)fromSec, (unsigned)toSec, stepMs, fade, (unsigned)g_sw.total);
}
void clockSweepStop() {
    if (!g_sw.active) return;
    g_sw.active = false; Tubes::setNixieFadeSteps(g_sw.savedSteps); forceRedraw();
    Serial.printf("[sweep] done: %u steps, max draw %u us\n", (unsigned)g_sw.done, (unsigned)g_sw.maxDrawUs);
}
bool     clockSweepActive()    { return g_sw.active; }
uint32_t clockSweepSec()       { return g_sw.sec; }
uint32_t clockSweepDone()      { return g_sw.done; }
uint32_t clockSweepTotal()     { return g_sw.total; }
uint32_t clockSweepMaxDrawUs() { return g_sw.maxDrawUs; }

static void sweepTick() {
    // Announce: hold "TEST" for ~1.5 s so a watcher knows the racing digits are deliberate.
    if ((int32_t)(millis() - g_sw.announceUntil) < 0) {
        if (!g_sw.announced) { char a[TUBE_COUNT]; layoutLeft("TEST", 0, a); showChars(a, true); g_sw.announced = true; }
        return;
    }
    if (g_sw.announced) { g_sw.announced = false; forceRedraw(); }   // first step repaints every tube
    if (g_sw.stepMs && (int32_t)(millis() - g_sw.nextAt) < 0) return;
    char hhmmss[7], out[TUBE_COUNT];
    const uint32_t s = g_sw.sec;
    snprintf(hhmmss, sizeof hhmmss, "%02u%02u%02u", (unsigned)(s / 3600u), (unsigned)((s / 60u) % 60u), (unsigned)(s % 60u));
    layoutRight(hhmmss, out);
    showChars(out, g_clockNixie);
    const uint32_t d = Tubes::lastDrawUs(); if (d > g_sw.maxDrawUs) g_sw.maxDrawUs = d;
    g_sw.done++;
    if (g_sw.sec == g_sw.to) { clockSweepStop(); return; }
    g_sw.sec = (g_sw.sec + 1) % 86400u;
    g_sw.nextAt = millis() + g_sw.stepMs;
}

void tick() {
    if (g_mcPending.exchange(false)) { clockSweepStop(); setEffectByName("off"); Leds::off(); Serial.println("[control] master clear (stream)"); }
    if (g_rtcSave) { g_rtcSave = false; Rtc::saveSystemTime(); }   // NTP just synced -> chip
    idleTick();
    // ---- LED-effect layer (independent of the display) ----
    // Animated effects repaint the underglow live; off/solid are static and set
    // once in setEffect(), so per-tube /rgb can drive them between calls.
    if (g_effect == Effect::Rainbow || g_effect == Effect::Breathe || g_effect == Effect::Comet) {
        if (millis() - g_animMs >= ANIM_MS) {
            g_animMs = millis();
            if      (g_effect == Effect::Rainbow) animRainbow();
            else if (g_effect == Effect::Breathe) animBreathe();
            else                                  animComet();
        }
    }

    // ---- Display layer ----
    if (g_repaint && (g_mode == Mode::Clock || g_mode == Mode::Nixie)) {   // brightness changed: redraw in place
        g_repaint = false;
        const bool nixie = (g_mode == Mode::Nixie) || g_clockNixie;
        for (uint8_t i = 0; i < TUBE_COUNT; ++i) {
            if (!Tubes::alive(i) || !g_shown[i]) continue;
            if (nixie) Tubes::drawGlyphNixie(i, g_shown[i]); else Tubes::drawGlyph(i, g_shown[i]);
        }
    }
    if (g_mode == Mode::Nixie) { nixieTick(); return; }
    if (g_mode != Mode::Clock) return;
    if (g_sw.active) { sweepTick(); return; }   // test: simulated time through the real path

    time_t now = time(nullptr);
    struct tm t;
    localtime_r(&now, &t);
    if ((t.tm_year + 1900) <= 2020) return;   // NTP not synced yet; keep self-test
    if (t.tm_sec == g_lastSec) return;         // only work when the second changes
    g_lastSec = t.tm_sec;

    // POSITIONAL mapping, NATIVE 0 = far RIGHT: tube i shows hhmmss[5-i] —
    //   i=0 sec-ones, 1 sec-tens, 2 min-ones, 3 min-tens, 4 hr-ones, 5 hr-tens.
    // Unpopulated tubes stay blank; the rest keep their correct clock position.
    // Nixie: the changed digits cross-fade together (old cathode dims as the
    // new one lights) — from a blank plate when nothing was shown yet.
    char hhmmss[7], out[TUBE_COUNT];
    snprintf(hhmmss, sizeof hhmmss, "%02d%02d%02d", t.tm_hour, t.tm_min, t.tm_sec);
    layoutRight(hhmmss, out);
    showChars(out, g_clockNixie);
}

}  // namespace Control
