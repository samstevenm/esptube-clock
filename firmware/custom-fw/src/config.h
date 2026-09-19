#pragma once
// ============================================================================
// config.h — hardware pin map & board constants for esptube-fw
// Target: ESP32-D0WD (WROOM-32D), 16 MB flash, no PSRAM.
// Board identified as a SI HAI IPS tube clock: 6x ST7789 135x240 IPS panels on
// a SHARED SPI bus, chip-select via a 74HC595 SHIFT REGISTER (not direct GPIO).
// Pins pulled verbatim from EleksTubeHAX include/GLOBAL_DEFINES.h SI_HAI block.
// ============================================================================
#include <stdint.h>

// ---- Identity ----
#define HOSTNAME "esptube"

// ---- Tube / panel geometry ----
#define TUBE_COUNT 6
#define PANEL_W    135
#define PANEL_H    240

// ---- Shared SPI bus (also mirrored into TFT_eSPI via -D build_flags) ----
#define PIN_SPI_MOSI 19
#define PIN_SPI_SCLK 18
#define PIN_TFT_DC   16
#define PIN_TFT_RST  23
#define PIN_SPI_MISO -1   // no separate MISO (TFT_SDA_READ)

// ---- Chip-select 74HC595 shift register ----
// One 595 selects which display(s) receive SPI. Active LOW at the panel; the
// shift path uses two unused high bits (Q7/Q6), so we shift (~map) << 2.
#define CSSR_DATA_PIN  4
#define CSSR_CLOCK_PIN 22
#define CSSR_LATCH_PIN 21

// ---- Display power / backlight-enable MOSFET (drive HIGH to enable) ----
#define PIN_TFT_ENABLE 2

// ---- WS2812 underglow: 6 LEDs (one per tube), color order GRB ----
// DS1302 real-time clock (3-wire, bit-banged) — keeps time with no WiFi (EleksTubeHAX SI HAI map)
#define PIN_RTC_SCLK 33
#define PIN_RTC_IO   25
#define PIN_RTC_CE   26

#define PIN_LED_DATA 32
#define LED_COUNT    6

// ---- Physical buttons (ESP32 INPUT-ONLY pins 34-39, NO internal pull-ups;
//      external pull-ups on the board, active-LOW). Read + debounced in
//      buttons.cpp; each maps to a Control::Button action. ----
#define BUTTON_UP_PIN    35   // brightness up  (also REST /button/left)
#define BUTTON_MODE_PIN  34   // cycle mode
#define BUTTON_DOWN_PIN  39   // brightness down (also REST /button/right)
#define BUTTON_POWER_PIN 36   // power toggle (off <-> last mode)

// ---- Tube index convention ------------------------------------------------
// NATIVE index: 0 = far RIGHT ... 5 = far LEFT (the 74HC595 bit i = the i-th
// tube from the right; selectTube(i) = csWrite(1<<i)). Used EVERYWHERE (REST,
// populated mask, clock, status, UI). The WS2812 glow chain runs the OPPOSITE
// direction, so LED index i maps to strip pixel (LED_COUNT-1-i) — the reversal
// lives in leds.cpp so LED i and display i are the same physical tube.

// ---- Default populated mask -----------------------------------------------
// Populated tubes are a PERSISTED setting (NVS "esptube"/"popmask"), authored
// via the web UI / POST /config/populated. Bit i = tube i (0 = far right).
// Fresh-NVS default 0x3E: far-right slot (index 0) empty; tubes 1-5 populated.
#define DEFAULT_POPULATED_MASK (0x3Eu)

// ---- Dead-tube set (manual compile-time force-off override) ---------------
// OR-ed into deadness on top of the runtime populated mask (a tube is dead if
// it's absent from the populated mask OR its bit is set here). Default 0.
#define DEAD_TUBES_MASK (0u)

static inline bool tubeIsDead(uint8_t i) {
    return (DEAD_TUBES_MASK >> i) & 0x1u;
}
static inline bool tubeIndexValid(int i) {
    return i >= 0 && i < TUBE_COUNT;
}
