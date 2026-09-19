#pragma once
// ============================================================================
// leds.h — WS2812 underglow wrapper (6 LEDs, one per tube, GRB order).
// Colors are staged as RAW (unscaled) values in a shadow buffer; show() applies
// the global brightness (0..255) when latching to the strip, so brightness
// changes re-scale cleanly from the originals rather than compounding.
// ============================================================================
#include <stdint.h>

namespace Leds {

void begin();
void setTube(uint8_t i, uint8_t r, uint8_t g, uint8_t b);  // stage raw color
void all(uint8_t r, uint8_t g, uint8_t b);                 // stage all raw
void off();                                                // stage all off + show
void show();                                               // latch (brightness applied)

void setBrightness(uint8_t b);   // 0..255; takes effect on next show()
uint8_t brightness();

// Read back the raw (unscaled) staged color for tube i (for GET /status).
void getTube(uint8_t i, uint8_t& r, uint8_t& g, uint8_t& b);

}  // namespace Leds
