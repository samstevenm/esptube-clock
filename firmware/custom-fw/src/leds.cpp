// ============================================================================
// leds.cpp — Adafruit NeoPixel implementation of the WS2812 underglow.
// ============================================================================
#include "leds.h"
#include "config.h"
#include <Adafruit_NeoPixel.h>

namespace {
Adafruit_NeoPixel strip(LED_COUNT, PIN_LED_DATA, NEO_GRB + NEO_KHZ800);

// Shadow buffer of RAW (unscaled) colors; brightness is applied at show().
uint8_t rawR[LED_COUNT], rawG[LED_COUNT], rawB[LED_COUNT];
uint8_t g_bright = 180;

inline uint8_t scale(uint8_t v) { return (uint8_t)((uint16_t)v * g_bright / 255); }
}

namespace Leds {

void begin() {
    strip.begin();
    for (uint8_t i = 0; i < LED_COUNT; ++i) { rawR[i] = rawG[i] = rawB[i] = 0; }
    strip.clear();
    strip.show();
}

void setTube(uint8_t i, uint8_t r, uint8_t g, uint8_t b) {
    if (i >= LED_COUNT) return;
    rawR[i] = r; rawG[i] = g; rawB[i] = b;
}

void all(uint8_t r, uint8_t g, uint8_t b) {
    for (uint8_t i = 0; i < LED_COUNT; ++i) { rawR[i] = r; rawG[i] = g; rawB[i] = b; }
}

void off() {
    for (uint8_t i = 0; i < LED_COUNT; ++i) { rawR[i] = rawG[i] = rawB[i] = 0; }
    show();
}

void show() {
    // Shadow buffer is indexed by DISPLAY index i (0 = far right). The WS2812
    // chain runs the OPPOSITE direction from the shift-register display order,
    // so display index i maps to strip pixel (LED_COUNT-1-i). This is the single
    // place the reversal happens, so LED i and display i are the same tube.
    for (uint8_t i = 0; i < LED_COUNT; ++i) {
        strip.setPixelColor(LED_COUNT - 1 - i,
                            strip.Color(scale(rawR[i]), scale(rawG[i]), scale(rawB[i])));
    }
    strip.show();
}

void setBrightness(uint8_t b) { g_bright = b; }
uint8_t brightness() { return g_bright; }

void getTube(uint8_t i, uint8_t& r, uint8_t& g, uint8_t& b) {
    if (i >= LED_COUNT) { r = g = b = 0; return; }
    r = rawR[i]; g = rawG[i]; b = rawB[i];
}

}  // namespace Leds
