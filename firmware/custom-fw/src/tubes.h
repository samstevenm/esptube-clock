#pragma once
// ============================================================================
// tubes.h — display abstraction for 6x ST7789 on a shared SPI bus (SI HAI).
//
// The ST7789 panels share MOSI/SCLK/DC/RST; chip-select is done through a
// 74HC595 shift register (TFT_eSPI is built with TFT_CS=-1 so it never touches
// CS). Every render op: select one tube (595) -> draw -> deselect all. Dead
// tubes (DEAD_TUBES_MASK) are skipped and never block.
// ============================================================================
#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>

namespace Tubes {

// What a tube is currently displaying (tracked for GET /status).
enum class ContentKind { Blank, Text, Image, Clock };

// One-time init: 595 CS lines, display-enable MOSFET, and TFT_eSPI (once).
void begin();

// The panel bus is shared by the loop task (clock, HTTP pushes) and the ESP/1
// stream task. Every public draw op takes this recursive mutex; multi-call
// sequences (raw stream) hold it from begin to end.
void lock();
void unlock();

// Chip-select via 74HC595. selectTube(i) selects only tube i; deselect() clears
// all. NATIVE index: 0 = far RIGHT (595 bit i).
void selectTube(uint8_t i);
void deselect();

// INFORMATIONAL ONLY: probe each position via ST7789 register readback and LOG
// the raw bytes. Reliable auto-detect isn't possible on this hardware, so this
// does NOT change the populated set — it's a diagnostic aid for calibration.
void detectPopulated();

// The authoritative populated set is persisted in NVS (namespace "esptube",
// key "popmask", default 0x3F) and loaded at boot. Set + persist a new mask
// (masked to 0x3F). Used by POST /config/populated.
void setPopulatedMask(uint8_t mask);

// True if position i is marked populated in the current (persisted) mask.
bool isPopulated(uint8_t i);

// Current populated bitmask (bit i set => populated).
uint8_t populatedMask();

// Liveness of a tube: populated AND not manually forced off, and in range.
// This is what all render ops and /status use.
bool alive(uint8_t i);

// Boot smoke test: fill each LIVE tube a distinct color and draw its index
// digit large + centered. WiFi/filesystem independent — stays on screen until
// the first REST/clock update overwrites it. Dead tubes are skipped.
void selfTest();

// Diagnostic: assert ALL CS low at once and fill every panel one solid color.
// Tests the shared-bus SPI write path independent of per-tube CS selection.
void diagAllColor(uint16_t color565);

// Render primitives — all no-op safely for dead / invalid tubes.
void drawText(uint8_t i, const String& text);
void drawGlyph(uint8_t i, char c);        // plain digital clock digit (white)
bool drawImageBMP(uint8_t i, const uint8_t* data, size_t len);  // 24/8-bit BMP

// ---- Nixie face ------------------------------------------------------------
// Pre-rendered cathode-wire digits (nixie_glyphs.h: 135x240 8-bit intensity
// plates, RLE in flash) colorized through a brightness-scaled amber LUT at draw
// time. No frame buffer: plates stream row-block by row-block to the panel.
// Glyph set: 0-9, A-Z (lowercase folded), - : . ! ? ° % + /, space = unlit plate.
// Unknown characters render as the unlit plate. '°' is passed as NIXIE_CH_DEGREE.
#define NIXIE_CH_DEGREE '\x01'
void setNixieBrightness(uint8_t v);       // rebuilds the LUT (display dimming)
void setNixieFadeSteps(uint8_t k);        // frames per change (1 = no fade); default 4
uint8_t nixieFadeSteps();
void drawGlyphNixie(uint8_t i, char c);   // one tube, one glyph, no fade
// Cross-fade n tubes at once (frame-major, so they change together). from[j]
// is the glyph currently shown (0 = unlit plate), to[j] the new one.
void drawGlyphsNixieFade(const uint8_t* idx, const char* from, const char* to, uint8_t n);
uint32_t lastDrawUs();                    // duration of the last nixie draw/fade
void fill(uint8_t i, uint16_t color565);
void clearAll();

// Displayed-content state for GET /status.
ContentKind contentKind(uint8_t i);
const char* contentKindStr(uint8_t i);   // "blank"|"text"|"image"|"clock"
String      contentText(uint8_t i);      // the text for kind=Text/Clock, else ""

// Memory-safe streaming of raw RGB565 rows (top-down) to one tube — no
// full-frame allocation. beginRawStream selects the tube (setSwapBytes=true, so
// each uint16_t is a TRUE RGB565 value like 0xF800); pushRawLine blits one
// PANEL_W-wide row at the next row; endRawStream deselects + marks kind=Image.
// Used by POST /tube/{}/raw565.
void beginRawStream(uint8_t i);
void pushRawLine(const uint16_t* line);
void endRawStream();

// Region blit (POST /tube/{}/blit, ESP/1 BLIT): streaming into a sub-window. Each
// writer owns a BlitCtx, so several streams (one TCP connection per tube) can be
// mid-frame at once; rows are batched into 8-row SPI pushes and every push selects
// its own tube. Coordinates are clamped; beginBlit returns false if nothing is left.
#define BLIT_BANKS 1                       // 2 when the DMA flush path is enabled (g_dma); 7 contexts × banks × 4.3 KB of DRAM
struct BlitCtx {
    int tube = -1, x = 0, y = 0, w = 0, h = 0, row = 0;
    int pending = 0;                       // rows buffered, not yet pushed
    uint8_t epoch = 0;                     // content epoch this blit started under (ESP/1.1)
    bool aborted = false;                  // superseded mid-frame: no more SPI, the rest of the payload is drained
    uint32_t drawUs = 0;                   // SPI time spent on this frame
    uint8_t bank = 0;                      // with DMA: decode into one bank while the other transfers
    uint16_t buf[BLIT_BANKS][135 * 16] __attribute__((aligned(4)));  // PANEL_W * BLIT_ROWS, DMA-capable
};
bool beginBlit(BlitCtx& c, uint8_t i, int x, int y, int w, int h);
int  blitWidth(const BlitCtx& c);        // clamped width in pixels (row length expected)
void pushBlitLine(BlitCtx& c, const uint16_t* line);
// Zero-copy variant: decode straight into the next row of the block buffer, then commit.
uint16_t* blitRow(BlitCtx& c);           // nullptr when the window is full
void      blitRowDone(BlitCtx& c);
void endBlit(BlitCtx& c);
void abortBlit(BlitCtx& c);              // drop buffered rows and close WITHOUT drawing them (superseded / upload aborted)

// ---- Content epoch (ESP/1.1): the NODE owns it, 1..255, never 0. Anything that makes what is
// in flight stale — a clear, a mode change, a CANCEL — calls supersede() BEFORE it takes the
// panel lock; every blit checks the epoch inside its locked 16-row flush and stops drawing when
// it no longer matches. That is what makes "clear" actually win against a frame already arriving.
uint8_t epoch();
uint8_t supersede();                     // bump; returns the new epoch
bool    advanceEpochTo(uint8_t gen);     // CAS epoch -> gen iff gen == succ(epoch); true if epoch == gen afterwards
// Solid rectangle (clamped), for the stream FILL op.
void fillRect(uint8_t i, int x, int y, int w, int h, uint16_t color565);

}  // namespace Tubes
