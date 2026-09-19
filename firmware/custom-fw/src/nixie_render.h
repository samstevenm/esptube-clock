#pragma once
// ============================================================================
// nixie_render.h — hardware-independent nixie plate decoding + composition.
//
// Shared by the firmware (tubes.cpp) and the native test (test/nixie_native.cpp)
// so the exact code that produces pixels on the glass is what the test checks.
// Plates come from nixie_glyphs.h (generated). A frame is:
//     intensity = BASE + (A*(K-k) + B*k)/K      (clamped to 255)
// i.e. the shared ghost/glass plate under a cross-fade of two glyph plates.
// k==0 -> pure A, k==K -> pure B (single reader, no blend rounding).
// ============================================================================
#include <stdint.h>
#include "nixie_glyphs.h"

#ifdef ARDUINO
  #define NIXIE_RD(p)    pgm_read_byte(p)
  #define NIXIE_RDPTR(p) pgm_read_ptr(p)
#else
  #define NIXIE_RD(p)    (*(const uint8_t*)(p))
  #define NIXIE_RDPTR(p) (*(const void* const*)(p))
#endif

// Streams one RLE plate pixel by pixel.
struct NixieRle {
    const uint8_t* p = nullptr; uint16_t remain = 0; uint8_t val = 0;
    void begin(uint8_t plate) {
        p = (const uint8_t*)NIXIE_RDPTR(&NIXIE_RLE[plate]); remain = 0; val = 0;
    }
    inline uint8_t next() {
        if (remain == 0) { remain = NIXIE_RD(p); val = NIXIE_RD(p + 1); p += 2; }
        remain--; return val;
    }
};

// Compose a full frame; calls sink(y, row) for each of NIXIE_H rows with a
// NIXIE_W-byte intensity row. Sink is any callable (lambda / functor).
template <typename Sink>
inline void nixieCompose(uint8_t plateA, uint8_t plateB, uint8_t k, uint8_t K, Sink&& sink) {
    NixieRle base, ra, rb;
    base.begin(NIXIE_BASE);
    const bool onlyB = (k >= K), onlyA = (k == 0);
    if (!onlyB) ra.begin(plateA);
    if (!onlyA) rb.begin(plateB);
    const uint16_t wa = K - k, wb = k;
    uint8_t row[NIXIE_W];
    for (int y = 0; y < NIXIE_H; ++y) {
        for (int x = 0; x < NIXIE_W; ++x) {
            uint16_t v = base.next();
            if (onlyB)      v += rb.next();
            else if (onlyA) v += ra.next();
            else            v += (uint16_t)(((uint32_t)ra.next() * wa + (uint32_t)rb.next() * wb) / K);
            row[x] = (v > 255) ? 255 : (uint8_t)v;
        }
        sink(y, row);
    }
}
