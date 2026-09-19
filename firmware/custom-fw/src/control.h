#pragma once
// ============================================================================
// control.h — TWO INDEPENDENT LAYERS + NTP clock + brightness + timezone.
//
//   Display layer (panels): CLOCK (NTP time), OFF (blank), MANUAL (a REST
//     content push took over; clock stops overwriting).
//   LED-effect layer (WS2812, runs in tick() regardless of the display):
//     OFF, SOLID (per-tube /rgb or a solid color), RAINBOW/BREATHE/COMET
//     (live ~40 ms animations that override per-tube /rgb until set back to
//     off/solid). Effects run SIMULTANEOUSLY with whatever the panels show.
//
// Physical buttons: MODE cycles the LED effect (off→rainbow→breathe→comet→
//   solid→off); UP/DOWN = brightness; POWER = display on/off. Brightness +
//   timezone persist in NVS.
// ============================================================================
#include <Arduino.h>

namespace Control {

enum class Mode   { Clock, Off, Manual, Nixie };             // display layer (Nixie = device-rendered message)
enum class Effect { Off, Solid, Rainbow, Breathe, Comet };   // LED layer
enum class Button { Up, Down, Mode, Power };

void begin();   // load NVS (brightness, tz), start NTP, set initial state
void tick();    // call each loop(): step the LED effect AND render the clock

// Single action handler shared by physical buttons and REST /button:
//   MODE  = next LED effect     UP/DOWN = brightness ±32
//   POWER = display on/off toggle (short or long)
// longPress is set only by physical buttons (REST calls are short presses).
void onButton(Button b, bool longPress = false);

// ---- Display layer ----
void setMode(Mode m);
Mode mode();
const char* modeName();                  // "clock"|"off"|"manual"
bool setModeByName(const char* name);    // clock|off|manual; false if bad

// ---- LED-effect layer (effect + solid colour persist in NVS) ----
void setEffect(Effect e);                // reset animation phase
void setSolidColor(uint8_t r, uint8_t g, uint8_t b);   // and switch to SOLID
void cycleEffect();                      // MODE button / POST /button/mode
Effect effect();
const char* effectName();                // "off"|"solid"|"rainbow"|"breathe"|"comet"
bool setEffectByName(const char* name);
void solidColor(uint8_t& r, uint8_t& g, uint8_t& b);

// ---- Device-native presets (firmware-rendered faces; short MODE / POST /preset) ----
//   0 = Wide Nixie clock (default) · 1 = Big digital · 2 = LED show (clock+comet) · 3 = Off
//   4 = Date (today as nixie text, e.g. "SEP17"; refreshes when the day changes)
void applyPreset(uint8_t p, bool persist = true);
void nextPreset();
bool setPresetByIndex(int i);         // false if out of range
uint8_t preset();                     // current index
uint8_t presetCount();
const char* presetName(uint8_t p);    // "nixie"|"digital"|"ledshow"|"off"|"date"

// REST content push auto-switches to MANUAL so the clock stops overwriting it.
void notifyManualContent();
// MASTER CLEAR — the one definition (REST /tubes/clear, shell `clear`, ESP/1 CANCEL+clear): supersede
// whatever is arriving, stop the sweep and the LED effect, go MANUAL, every panel black, every glow off.
void masterClear();                  // from loop() context (touches NVS via the LED effect)
void masterClearFromTask(bool bumpEpoch = true);   // from any task (bumpEpoch=false when the caller already superseded): the glass goes dark NOW, the NVS-touching rest on the next tick()
uint16_t idleMinutes();              // MANUAL -> clock after this many minutes without content (0 = never)
void     setIdleMinutes(uint16_t m); // persisted

// Brightness 0..255 (scales WS2812; TFT stays full-on on this board).
void setBrightness(uint8_t v);
uint8_t brightness();

// POSIX timezone string (persisted; re-applies NTP config on change).
void setTz(const String& tz);
String tz();
bool timeValid();   // true once NTP has produced a plausible year (>2020)

// Force a full clock re-layout on the next tick (e.g. after the populated
// mask changed).
void forceClockRedraw();

// ---- Device-native nixie messages (Mode::Nixie) — zero pixels pushed ----
// text: UTF-8 (A-Z 0-9 and - : . ! ? ° % + /; others blank). effect strings:
//   "static"                 — the message, left-aligned, held.
//   "flash"                  — on/off every ms.
//   "scroll"                 — SMOOTH horizontal marquee (sub-glyph pixel motion).
//   "scrollstep"             — legacy one-glyph-per-step scroll (cheapest).
//   "vscroll"                — SMOOTH vertical marquee: hyphenated lines slide DOWN.
//   "vpage" / "vflip"        — vertical marquee by page-flip (cross-fade per line).
// `ms` is the motion rate (glyph-width period for scroll, line dwell for vpage).
// countdown: HHMMSS / MMSS positional; flashes 0000 at the end, returns to clock.
// nixieStop() returns to the clock immediately.
enum class NixieKind { Static, Flash, Scroll, Marquee, VScroll, VPage, Countdown };
bool nixieText(const char* utf8, const char* effect, uint32_t ms);
void nixieCountdown(uint32_t seconds);
void nixieStop();
const char* nixieKindName();             // "static"|"flash"|"scroll"|"countdown"
const char* nixieMessage();              // the sanitized message ('\x01' = °)
uint32_t    nixieMs();
int32_t     nixieRemaining();            // countdown seconds left (0 if n/a)

// ---- Test: sweep the clock through [fromSec, toSec] using the REAL render path ----
// stepMs = pacing (0 = as fast as the fades allow); fade=false renders each
// step in one frame. Progress is reported in /status.sweep. Stops itself at toSec.
void clockSweepStart(uint32_t fromSec, uint32_t toSec, uint16_t stepMs, bool fade);
void clockSweepStop();
bool     clockSweepActive();
uint32_t clockSweepSec();                // simulated seconds-of-day now showing
uint32_t clockSweepDone();               // steps rendered so far
uint32_t clockSweepTotal();              // steps in the range
uint32_t clockSweepMaxDrawUs();          // slowest step so far

}  // namespace Control
