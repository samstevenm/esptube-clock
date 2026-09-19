// ============================================================================
// tubes.cpp — implementation of the 6x ST7789 shared-bus display layer.
// ============================================================================
#include <atomic>
#include "tubes.h"
#include "config.h"
#include "nixie_render.h"   // plate decode + compose (shared with test/nixie_native.cpp)
#include <TFT_eSPI.h>
#include <Preferences.h>
#include <math.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace {

// Single TFT_eSPI instance drives the shared bus. CS is -1 in the build flags,
// so this object never asserts a chip-select — a 74HC595 shift register does.
TFT_eSPI tft = TFT_eSPI(PANEL_W, PANEL_H);
bool g_ready = false;
SemaphoreHandle_t g_mtx = nullptr;     // panel-bus mutex (recursive)
bool g_dmaInFlight = false;            // a blit block is still transferring by DMA (tube selected)
bool g_dma = false;                    // DMA engine available
int  g_selTube = -1;                   // what the 595 currently selects (-1 = none), so blits skip re-selects
struct Guard { Guard() { Tubes::lock(); } ~Guard() { Tubes::unlock(); } };

// Authoritative populated set (bit i set => LOGICAL tube i present, 0=far left).
// Loaded from NVS at boot and updated via POST /config/populated.
uint8_t g_populated = DEFAULT_POPULATED_MASK;

// Per-tube displayed-content state, for GET /status. Kind + (for text) the
// string; updated wherever we draw/clear.
Tubes::ContentKind g_kind[TUBE_COUNT];
String             g_text[TUBE_COUNT];

// Raw RGB565 streaming state (POST /tube/{}/raw565).
int g_streamTube = -1;
int g_streamRow  = 0;

// NVS-persisted config.
Preferences g_prefs;
const char* NVS_NS   = "esptube";
const char* NVS_KEY  = "popmask";

uint8_t loadPopulatedFromNVS() {
    uint8_t m = DEFAULT_POPULATED_MASK;
    if (g_prefs.begin(NVS_NS, /*readOnly=*/true)) {
        m = g_prefs.getUChar(NVS_KEY, DEFAULT_POPULATED_MASK);
        g_prefs.end();
    }
    m &= 0x3F;
    return m;
}

void savePopulatedToNVS(uint8_t m) {
    if (g_prefs.begin(NVS_NS, /*readOnly=*/false)) {
        g_prefs.putUChar(NVS_KEY, (uint8_t)(m & 0x3F));
        g_prefs.end();
    }
}

std::atomic<uint8_t> g_epoch{1};          // content epoch (see tubes.h); read by blits on the esp1 task, bumped from any task

// ---- 74HC595 chip-select --------------------------------------------------
// digits_map: bit i set => tube i selected (0..5). CS is active-LOW at the
// panels, and the two most-significant 595 outputs (Q7/Q6) are unused, so the
// byte clocked out is (~digits_map) << 2. Mirrors EleksTubeHAX's non-
// CS_DIRECT_GPIO ChipSelect::update().
inline void csWrite(uint8_t digits_map) {
    uint8_t to_shift = (uint8_t)(~digits_map) << 2;
    digitalWrite(CSSR_LATCH_PIN, LOW);
    shiftOut(CSSR_DATA_PIN, CSSR_CLOCK_PIN, LSBFIRST, to_shift);
    digitalWrite(CSSR_LATCH_PIN, HIGH);
}

}  // namespace

namespace Tubes {

namespace { void initNixieCharMap(); }   // defined with the nixie face below

uint8_t epoch() { return g_epoch.load(); }
uint8_t supersede() {
    uint8_t e = g_epoch.load(), n;
    do { n = (uint8_t)(e >= 255 ? 1 : e + 1); } while (!g_epoch.compare_exchange_weak(e, n));
    return n;
}
bool advanceEpochTo(uint8_t gen) {
    uint8_t e = g_epoch.load();
    if (e == gen) return true;
    if ((uint8_t)(e >= 255 ? 1 : e + 1) != gen) return false;
    if (g_epoch.compare_exchange_strong(e, gen)) return true;
    return g_epoch.load() == gen;          // somebody else advanced to the same epoch first
}

// Taking the bus also retires any DMA block still in flight (from a blit flush),
// so whoever draws next finds the bus idle and no tube selected.
void lock() {
    if (g_mtx) xSemaphoreTakeRecursive(g_mtx, portMAX_DELAY);
    if (g_dmaInFlight) { tft.dmaWait(); tft.endWrite(); deselect(); g_dmaInFlight = false; }
}
void unlock() { if (g_mtx) xSemaphoreGiveRecursive(g_mtx); }

void begin() {
    g_mtx = xSemaphoreCreateRecursiveMutex();
    for (uint8_t i = 0; i < TUBE_COUNT; ++i) { g_kind[i] = ContentKind::Blank; g_text[i] = ""; }

    // Shift-register control lines: outputs, idle LOW, then deselect all.
    pinMode(CSSR_DATA_PIN, OUTPUT);
    pinMode(CSSR_CLOCK_PIN, OUTPUT);
    pinMode(CSSR_LATCH_PIN, OUTPUT);
    digitalWrite(CSSR_DATA_PIN, LOW);
    digitalWrite(CSSR_CLOCK_PIN, LOW);
    digitalWrite(CSSR_LATCH_PIN, LOW);
    csWrite(0x00);   // all tubes deselected

    // Display power / backlight-enable MOSFET on (active HIGH).
    pinMode(PIN_TFT_ENABLE, OUTPUT);
    digitalWrite(PIN_TFT_ENABLE, HIGH);

    // TFT_CS=-1, so tft.init() never drives a CS. Select ALL SIX tubes via the
    // 595 so the shared ST7789 init sequence broadcasts to every panel, and
    // hold them selected through init + rotation + the initial clear. The
    // shift-register CS lines don't collide with the SPI pins, so (unlike the
    // direct-GPIO board) no post-init pin reclaim is needed.
    csWrite(0x3F);   // all six selected

    tft.init();
    tft.setRotation(0);          // portrait, applied to all panels at once
    tft.fillScreen(TFT_BLACK);   // clear every panel while all are selected

    deselect();      // release the bus; per-tube drawing selects one at a time

    // Per-tube "kick" (fixes the reported tube-1 glitch). Symptom: a panel
    // ignores its FIRST individual CS transaction right after boot — a SPANNING
    // write (all CS asserted at once, like the broadcast init or a full-row
    // push to every tube) is needed before per-tube updates take. The broadcast
    // init above happens the instant the panels power on, so the very first LONE
    // per-tube select can miss (CS / level-shifter settle timing, worst on the
    // marginal index-1 FPC). Fix: let the panels settle, then select each tube
    // on its own and clear it — so the user's first REAL per-tube update is no
    // longer that panel's first-ever individual selection.
    delay(50);
    for (uint8_t i = 0; i < TUBE_COUNT; ++i) {
        selectTube(i);
        delay(3);                    // let this tube's CS settle before writing
        tft.fillScreen(TFT_BLACK);
        deselect();
    }

    // DMA blit flushes (pushImageDMA) are wired in blitFlush() but OFF: with the
    // vendored fork the stream stalled mid-frame and raw ingest got slower — it
    // needs an eyes-on-the-glass session (DMA stays off; see docs/LEARNINGS.md).
    g_dma = false;
    Serial.println("[tubes] DMA off (blocking pushes)");
    initNixieCharMap();
    setNixieBrightness(255);   // Control::begin() re-scales to the saved brightness

    // Load the AUTHORITATIVE populated set from NVS (default 0x3F = all six).
    g_populated = loadPopulatedFromNVS();
    Serial.printf("[tubes] populated mask loaded from NVS = 0x%02X\n", g_populated);

    // Informational only: log an ST7789 register readback for each position so
    // the mask can be sanity-checked by eye. Does NOT change g_populated.
    detectPopulated();

    Serial.println("[tubes] begin() complete (SI HAI 595 CS; init + clear + NVS load done)");
    g_ready = true;
}

// INFORMATIONAL ONLY. Probe each position by selecting its CS and reading
// ST7789 status/ID registers over the shared bus (possible because we build
// with TFT_SDA_READ), and LOG the raw bytes. Reliable auto-detect isn't
// possible on this hardware, so this does NOT set the populated mask — the
// NVS-persisted mask is authoritative. Kept purely as a calibration aid.
void detectPopulated() {
    for (uint8_t i = 0; i < TUBE_COUNT; ++i) {
        csWrite((uint8_t)(1u << i));           // select only this position
        // Skip the leading dummy byte (index 1) for each read.
        uint8_t rddid = tft.readcommand8(0x04, 1);   // RDDID
        uint8_t rddst = tft.readcommand8(0x09, 1);   // RDDST
        uint8_t rddpm = tft.readcommand8(0x0A, 1);   // RDDPM (power mode)
        deselect();

        const bool allZero = (rddid == 0x00 && rddst == 0x00 && rddpm == 0x00);
        const bool allOnes = (rddid == 0xFF && rddst == 0xFF && rddpm == 0xFF);
        const bool guess   = !allZero && !allOnes;   // heuristic guess only
        Serial.printf("[detect] tube %u: RDDID=0x%02X RDDST=0x%02X RDDPM=0x%02X -> guess=%s (informational)\n",
                      i, rddid, rddst, rddpm, guess ? "present" : "empty");
    }
    Serial.printf("[detect] authoritative populated mask (NVS)=0x%02X\n", g_populated);
}

void setPopulatedMask(uint8_t mask) {
    g_populated = (uint8_t)(mask & 0x3F);
    savePopulatedToNVS(g_populated);
    Serial.printf("[config] populated mask set to 0x%02X (persisted to NVS)\n", g_populated);
}

void selectTube(uint8_t i) {
    if (!tubeIndexValid(i)) return;
    // NATIVE: index i = 595 bit i (bit0 = far right).
    csWrite((uint8_t)(1u << i)); g_selTube = i;
    // Let the panel's CS settle before the first SPI byte. A transaction the
    // panel misses leaves a stale frame on the glass (seen once as a fade frame
    // stuck mid-blend); 30 µs per select is noise next to a 13 ms frame.
    delayMicroseconds(30);
}

void deselect() {
    csWrite(0x00); g_selTube = -1;
}

bool isPopulated(uint8_t i) {
    return tubeIndexValid(i) && ((g_populated >> i) & 0x1u);
}

uint8_t populatedMask() {
    return g_populated;
}

bool alive(uint8_t i) {
    // Populated (persisted mask) AND not manually forced off (DEAD_TUBES_MASK).
    return tubeIndexValid(i) && isPopulated(i) && !tubeIsDead(i);
}

void selfTest() {
    Guard _g;
    if (!g_ready) return;
    // Distinct, high-contrast background per tube (index 0..5).
    static const uint16_t bg[TUBE_COUNT] = {
        TFT_RED, TFT_ORANGE, TFT_YELLOW, TFT_GREEN, TFT_BLUE, TFT_MAGENTA
    };
    for (uint8_t i = 0; i < TUBE_COUNT; ++i) {
        if (!alive(i)) continue;                 // only detected/live tubes
        Serial.printf("[selftest] tube %u\n", i);
        selectTube(i);
        tft.fillScreen(bg[i]);
        tft.setTextColor(TFT_BLACK, bg[i]);
        tft.setTextDatum(MC_DATUM);
        tft.drawString(String((int)i), PANEL_W / 2, PANEL_H / 2, 6);  // large font
        deselect();
    }
}

void diagAllColor(uint16_t color565) {
    Guard _g;
    if (!g_ready) return;
    csWrite(0x3F);   // ALL panels selected (74HC595)
    tft.fillScreen(color565);
    deselect();
}

void drawText(uint8_t i, const String& text) {
    Guard _g;
    if (!g_ready || !alive(i)) return;   // skip dead/invalid, never block
    selectTube(i);
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextDatum(MC_DATUM);          // middle-centre
    tft.drawString(text, PANEL_W / 2, PANEL_H / 2, 4);
    deselect();
    g_kind[i] = text.length() ? ContentKind::Text : ContentKind::Blank;
    g_text[i] = text;
}

void drawGlyph(uint8_t i, char c) {   // clock digit (plain digital: white on black)
    Guard _g;
    if (!g_ready || !alive(i)) return;
    char s[2] = { c, 0 };
    selectTube(i);
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextDatum(MC_DATUM);
    tft.drawString(s, PANEL_W / 2, PANEL_H / 2, 6);   // large 48px font
    deselect();
    g_kind[i] = ContentKind::Clock;
    g_text[i] = String(c);
}
// ---------------------------------------------------------------------------
// Nixie face. Glyphs are pre-rendered intensity plates (tools/gen_nixie_glyphs.py):
// a thin cathode-wire glyph with neon bloom; the shared BASE plate (the other
// cathodes faintly stacked behind + a warm lit-glass vignette) goes under every
// glyph at draw time. Decode/compose live in nixie_render.h (also run natively
// by test/nixie_native.cpp); here we only colorize through the LUT and push
// row-blocks to the panel. No frame buffer — ~2 KB of scratch.
// ---------------------------------------------------------------------------
namespace {

uint8_t  g_fadeSteps = 4;              // frames per glyph change (~26 ms each)
const int NIXIE_ROWS = 8;              // rows per SPI push
uint16_t g_nixieBuf[NIXIE_W * NIXIE_ROWS];
uint16_t g_nixieLut[256];
uint32_t g_lastDrawUs = 0;
int8_t   g_charPlate[128];             // ASCII -> plate index, -1 = none
int8_t   g_degreePlate = -1;           // '°' (2-byte UTF-8 in NIXIE_CHARS)

// Build the char->plate map from NIXIE_CHARS (plate index = character position,
// counting the multi-byte '°' as one).
void initNixieCharMap() {
    memset(g_charPlate, -1, sizeof g_charPlate);
    const char* s = NIXIE_CHARS; int plate = 0;
    for (size_t i = 0; s[i]; ++plate) {
        uint8_t b = (uint8_t)s[i];
        if (b == 0xC2 && (uint8_t)s[i + 1] == 0xB0) { g_degreePlate = (int8_t)plate; i += 2; }
        else { if (b < 128) g_charPlate[b] = (int8_t)plate; i += 1; }
    }
}
uint8_t plateOf(char c) {
    if (c == NIXIE_CH_DEGREE) return g_degreePlate >= 0 ? (uint8_t)g_degreePlate : NIXIE_BLANK;
    if (c >= 'a' && c <= 'z') c = (char)(c - 32);
    uint8_t u = (uint8_t)c;
    return (u < 128 && g_charPlate[u] >= 0) ? (uint8_t)g_charPlate[u] : NIXIE_BLANK;
}

// Amber neon ramp, intensity -> RGB, scaled by display brightness. sqrt scaling
// keeps the default (180) at ~84% so UP/DOWN visibly dim without going murky;
// floor at 8% so the face never fully vanishes.
struct Stop { uint8_t i, r, g, b; };
const Stop NIXIE_RAMP[] = { {0,0,0,0}, {24,38,7,0}, {80,168,44,4}, {150,255,108,18},
                            {215,255,168,72}, {255,255,228,175} };
void buildNixieLut(uint8_t bright) {
    float s = sqrtf(bright / 255.0f); if (s < 0.08f) s = 0.08f;
    for (int i = 0; i < 256; ++i) {
        int k = 0; while (k < 4 && i > NIXIE_RAMP[k + 1].i) k++;
        const Stop& a = NIXIE_RAMP[k]; const Stop& b = NIXIE_RAMP[k + 1];
        float t = (float)(i - a.i) / (float)(b.i - a.i);
        int r = (int)((a.r + (b.r - a.r) * t) * s);
        int g = (int)((a.g + (b.g - a.g) * t) * s);
        int bl = (int)((a.b + (b.b - a.b) * t) * s);
        g_nixieLut[i] = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (bl >> 3));
    }
}

// Push one full frame to the SELECTED tube: BASE + (A*(K-k) + B*k)/K, colorized
// through the LUT, in NIXIE_ROWS-row blocks.
void nixieFrame(uint8_t plateA, uint8_t plateB, uint8_t k, uint8_t K) {
    int y0 = 0;
    nixieCompose(plateA, plateB, k, K, [&](int y, const uint8_t* row) {
        uint16_t* d = g_nixieBuf + (y - y0) * NIXIE_W;
        for (int x = 0; x < NIXIE_W; ++x) d[x] = g_nixieLut[row[x]];
        if (y - y0 == NIXIE_ROWS - 1 || y == NIXIE_H - 1) {
            tft.pushImage(0, y0, NIXIE_W, y - y0 + 1, g_nixieBuf);
            y0 = y + 1;
        }
    });
}

}  // namespace

void setNixieBrightness(uint8_t v) { buildNixieLut(v); }
void setNixieFadeSteps(uint8_t k) { g_fadeSteps = k < 1 ? 1 : (k > 8 ? 8 : k); }
uint8_t nixieFadeSteps() { return g_fadeSteps; }
uint32_t lastDrawUs() { return g_lastDrawUs; }

void drawGlyphNixie(uint8_t i, char c) {
    Guard _g;
    if (!g_ready || !alive(i)) return;
    const uint32_t t0 = micros();
    selectTube(i);
    tft.setSwapBytes(true);   // LUT holds true RGB565
    nixieFrame(plateOf(c), plateOf(c), g_fadeSteps, g_fadeSteps);
    deselect();
    g_kind[i] = ContentKind::Clock;
    g_text[i] = String(c);
    g_lastDrawUs = micros() - t0;
}

void drawGlyphsNixieFade(const uint8_t* idx, const char* from, const char* to, uint8_t n) {
    Guard _g;
    if (!g_ready || n == 0) return;
    const uint32_t t0 = micros();
    tft.setSwapBytes(true);
    // Frame-major: every changing tube advances one step before any tube takes
    // the next, so a 3-digit rollover fades as one motion instead of a ripple.
    const uint8_t K = g_fadeSteps;
    for (uint8_t k = 1; k <= K; ++k) {
        for (uint8_t j = 0; j < n; ++j) {
            if (!alive(idx[j])) continue;
            selectTube(idx[j]);
            nixieFrame(plateOf(from[j]), plateOf(to[j]), k, K);
            deselect();
        }
    }
    for (uint8_t j = 0; j < n; ++j) {
        if (!alive(idx[j])) continue;
        g_kind[idx[j]] = ContentKind::Clock;
        g_text[idx[j]] = String(to[j]);
    }
    g_lastDrawUs = micros() - t0;
}

void fill(uint8_t i, uint16_t color565) {
    Guard _g;
    if (!g_ready || !alive(i)) return;
    selectTube(i);
    tft.fillScreen(color565);
    deselect();
    g_kind[i] = ContentKind::Blank;
    g_text[i] = "";
}

// Every live panel black in ONE SPI pass: the 74HC595 selects them all at once (13 ms instead of
// 13 ms x tubes). Callers that mean "and stop what is arriving" call supersede() first.
void clearAll() {
    Guard _g;
    if (!g_ready) return;
    uint8_t mask = 0;
    for (uint8_t i = 0; i < TUBE_COUNT; ++i) {
        if (!alive(i)) continue;
        mask |= (uint8_t)(1u << i);
        g_kind[i] = ContentKind::Blank;
        g_text[i] = "";
    }
    if (!mask) return;
    csWrite(mask); g_selTube = -1;
    delayMicroseconds(30);
    tft.fillScreen(TFT_BLACK);
    deselect();
}

ContentKind contentKind(uint8_t i) {
    return tubeIndexValid(i) ? g_kind[i] : ContentKind::Blank;
}
const char* contentKindStr(uint8_t i) {
    switch (contentKind(i)) {
        case ContentKind::Text:  return "text";
        case ContentKind::Image: return "image";
        case ContentKind::Clock: return "clock";
        default:                 return "blank";
    }
}
String contentText(uint8_t i) {
    return tubeIndexValid(i) ? g_text[i] : String();
}

void beginRawStream(uint8_t i) {
    if (!g_ready || !alive(i)) { g_streamTube = -1; return; }
    lock();                      // held until endRawStream()
    g_streamTube = i;
    g_streamRow  = 0;
    selectTube(i);
    tft.setSwapBytes(true);      // line[] carries true RGB565 values
    tft.fillScreen(TFT_BLACK);
}

void pushRawLine(const uint16_t* line) {
    if (g_streamTube < 0) return;
    if (g_streamRow < PANEL_H) {
        tft.pushImage(0, g_streamRow, PANEL_W, 1, const_cast<uint16_t*>(line));
        g_streamRow++;
    }
}

void endRawStream() {
    if (g_streamTube < 0) return;
    deselect();
    g_kind[g_streamTube] = ContentKind::Image;
    g_text[g_streamTube] = "";
    g_streamTube = -1;
    g_streamRow  = 0;
    unlock();
}

// ---- Region blit: the "send only what changed" primitive ----
// Rows are batched into BLIT_ROWS-row SPI pushes (one window + one transaction
// per block instead of per row) — the same trick the nixie renderer uses. Each
// push selects its tube, so writers on different tubes can interleave freely.
namespace {
const int BLIT_ROWS = 16;
void blitFlush(Tubes::BlitCtx& c) {
    if (c.pending <= 0) return;
    Tubes::lock();               // retires the previous DMA block (any writer's)
    // Superseded while we waited for the bus (a clear holds it) or since the last block: stop here.
    // Checked INSIDE the lock — a block parked on the mutex during clearAll() must not repaint after it.
    if (c.epoch != g_epoch.load()) { c.aborted = true; c.pending = 0; Tubes::unlock(); return; }
    const uint32_t t0 = micros();
    // Old-school trick: leave the tube selected between blocks of the same frame —
    // a re-select costs a shift-out + 30 µs settle 15× per frame otherwise.
    if (g_selTube != c.tube) Tubes::selectTube((uint8_t)c.tube);
    tft.setSwapBytes(true);      // true RGB565 in, like /raw565
    uint16_t* blk = c.buf[c.bank];
    if (g_dma) {
        tft.startWrite();
        tft.pushImageDMA(c.x, c.y + c.row - c.pending, c.w, c.pending, blk);
        g_dmaInFlight = true;    // the tube stays selected until the next lock() retires it
        c.bank = (uint8_t)((c.bank + 1) % BLIT_BANKS);   // next block goes to the other bank meanwhile
    } else {
        tft.pushImage(c.x, c.y + c.row - c.pending, c.w, c.pending, blk);
    }
    c.pending = 0;
    c.drawUs += micros() - t0;
    Tubes::unlock();
}
}

bool beginBlit(BlitCtx& c, uint8_t i, int x, int y, int w, int h) {
    c.tube = -1;
    if (!g_ready || !alive(i)) return false;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > PANEL_W) w = PANEL_W - x;
    if (y + h > PANEL_H) h = PANEL_H - y;
    if (w <= 0 || h <= 0 || x >= PANEL_W || y >= PANEL_H) return false;
    c.x = x; c.y = y; c.w = w; c.h = h; c.row = 0; c.tube = i; c.pending = 0; c.bank = 0;
    c.epoch = g_epoch.load(); c.aborted = false; c.drawUs = 0;
    return true;
}
int  blitWidth(const BlitCtx& c) { return c.tube < 0 ? 0 : c.w; }
void pushBlitLine(BlitCtx& c, const uint16_t* line) {
    uint16_t* dst = blitRow(c); if (!dst) return;
    memcpy(dst, line, (size_t)c.w * 2);
    blitRowDone(c);
}
uint16_t* blitRow(BlitCtx& c) {
    if (c.tube < 0 || c.aborted || c.row >= c.h) return nullptr;
    return c.buf[c.bank] + c.pending * c.w;
}
void blitRowDone(BlitCtx& c) {
    c.pending++; c.row++;
    if (c.pending == BLIT_ROWS) blitFlush(c);
}
void endBlit(BlitCtx& c) {
    if (c.tube < 0) return;
    if (!c.aborted) blitFlush(c);
    if (!c.aborted) { g_kind[c.tube] = ContentKind::Image; g_text[c.tube] = ""; g_lastDrawUs = c.drawUs; }
    c.tube = -1;
}
void abortBlit(BlitCtx& c) { c.pending = 0; c.aborted = true; c.tube = -1; }

void fillRect(uint8_t i, int x, int y, int w, int h, uint16_t color565) {
    Guard _g;
    if (!g_ready || !alive(i)) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > PANEL_W) w = PANEL_W - x;
    if (y + h > PANEL_H) h = PANEL_H - y;
    if (w <= 0 || h <= 0) return;
    selectTube(i);
    tft.fillRect(x, y, w, h, color565);
    deselect();
    g_kind[i] = ContentKind::Image; g_text[i] = "";
}

// ---------------------------------------------------------------------------
// Minimal little-endian BMP reader (uncompressed 24-bit or 8-bit palettized).
// Centers the image on the 135x240 panel and blits it via pushImage().
// ---------------------------------------------------------------------------
namespace {

inline uint16_t rd16(const uint8_t* p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }
inline uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

}  // namespace

bool drawImageBMP(uint8_t i, const uint8_t* data, size_t len) {
    Guard _g;
    if (!g_ready || !alive(i)) return false;   // skip dead/invalid
    if (!data || len < 54) return false;
    if (rd16(data + 0) != 0x4D42) return false;   // 'BM'

    const uint32_t dataOffset = rd32(data + 10);
    const uint32_t hdrSize    = rd32(data + 14);
    const int32_t  wRaw       = (int32_t)rd32(data + 18);
    const int32_t  hRaw       = (int32_t)rd32(data + 22);
    const uint16_t bpp        = rd16(data + 28);
    const uint32_t compress   = rd32(data + 30);

    if (compress != 0) return false;              // only BI_RGB supported
    if (bpp != 24 && bpp != 8) return false;

    const bool     topDown = hRaw < 0;
    const int32_t  imgW    = wRaw;
    const int32_t  imgH    = topDown ? -hRaw : hRaw;
    if (imgW <= 0 || imgH <= 0) return false;

    // 8-bit palette (BGRA entries) sits right after the info header.
    const uint8_t* palette = data + 14 + hdrSize;
    uint32_t paletteBytes  = 0;
    if (bpp == 8) {
        uint32_t nColors = rd32(data + 46);       // biClrUsed
        if (nColors == 0) nColors = 256;
        paletteBytes = nColors * 4;
        if ((size_t)(14 + hdrSize + paletteBytes) > len) return false;
    }

    const uint32_t rowSize = (((uint32_t)bpp * (uint32_t)imgW + 31u) / 32u) * 4u;

    // Clamp to panel and center.
    const int drawW = imgW < PANEL_W ? (int)imgW : PANEL_W;
    const int drawH = imgH < PANEL_H ? (int)imgH : PANEL_H;
    const int offX  = (PANEL_W - drawW) / 2;
    const int offY  = (PANEL_H - drawH) / 2;

    // Memory-safe: decode + blit ONE ROW at a time via a small stack buffer
    // (~270 B), never a full-frame heap allocation. Select the tube once.
    uint16_t line[PANEL_W];
    selectTube(i);
    tft.setSwapBytes(true);   // line[] holds true RGB565 values (e.g. 0xF800=red)
    tft.fillScreen(TFT_BLACK);
    for (int oy = 0; oy < drawH; ++oy) {
        const int32_t fileRow  = topDown ? oy : (imgH - 1 - oy);   // BMP bottom-up
        const size_t  rowStart = (size_t)dataOffset + (size_t)fileRow * rowSize;
        for (int x = 0; x < drawW; ++x) {
            uint8_t r = 0, g = 0, b = 0;
            if (bpp == 24) {
                const size_t p = rowStart + (size_t)x * 3u;
                if (p + 2 < len) { b = data[p]; g = data[p + 1]; r = data[p + 2]; }
            } else {  // 8-bit palettized
                const size_t p = rowStart + (size_t)x;
                if (p < len) {
                    const uint8_t idx = data[p];
                    const size_t pe   = (size_t)idx * 4u;
                    if (pe + 2 < paletteBytes) { b = palette[pe]; g = palette[pe + 1]; r = palette[pe + 2]; }
                }
            }
            line[x] = rgb565(r, g, b);
        }
        tft.pushImage(offX, offY + oy, drawW, 1, line);
    }
    deselect();

    g_kind[i] = ContentKind::Image;
    g_text[i] = "";
    return true;
}

}  // namespace Tubes
