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

// ---- Smooth motion composites (baked plates, on-device — no pixels pushed) ----
// The BASE (ghost cathodes + warm glass) is fixed to the tube; only the lit glyph
// layer moves, exactly like a real tube. Both share the row-sequential RLE readers,
// so they stay ~2 KB of scratch and single-pass.

// Horizontal window: this tube shows a 135-px slice of a virtual glyph strip. The
// right part of plateL (from column `fx`) fills the left of the panel; plateR's left
// fills the rest → a glyph can straddle two tubes as the text scrolls left.
template <typename Sink>
inline void nixieComposeHShift(uint8_t plateL, uint8_t plateR, uint16_t fx, Sink&& sink) {
    if (fx >= NIXIE_W) fx = NIXIE_W - 1;
    NixieRle base, rl, rr;
    base.begin(NIXIE_BASE); rl.begin(plateL); rr.begin(plateR);
    const uint16_t split = NIXIE_W - fx;              // out x < split -> plateL[fx+x]; else plateR[x-split]
    uint8_t bl[NIXIE_W], gl[NIXIE_W], gr[NIXIE_W], row[NIXIE_W];
    for (int y = 0; y < NIXIE_H; ++y) {
        for (int x = 0; x < NIXIE_W; ++x) { bl[x] = base.next(); gl[x] = rl.next(); gr[x] = rr.next(); }
        for (int x = 0; x < NIXIE_W; ++x) {
            uint16_t g = (x < split) ? gl[fx + x] : gr[x - split];
            uint16_t v = (uint16_t)bl[x] + g;
            row[x] = v > 255 ? 255 : (uint8_t)v;
        }
        sink(y, row);
    }
}

// Vertical shift: the current glyph slides DOWN by `dy` (0..NIXIE_H) and the next
// glyph enters from the top — a downward marquee, one line per glyph position.
template <typename Sink>
inline void nixieComposeVShift(uint8_t plateCur, uint8_t plateNext, uint16_t dy, Sink&& sink) {
    if (dy > NIXIE_H) dy = NIXIE_H;
    NixieRle base, rc, rn;
    base.begin(NIXIE_BASE); rc.begin(plateCur); rn.begin(plateNext);
    for (uint32_t skip = (uint32_t)(NIXIE_H - dy) * NIXIE_W; skip; --skip) rn.next();  // to plateNext row (H-dy)
    uint8_t bl[NIXIE_W], g[NIXIE_W], row[NIXIE_W];
    for (int y = 0; y < NIXIE_H; ++y) {
        for (int x = 0; x < NIXIE_W; ++x) bl[x] = base.next();
        if (y < dy) for (int x = 0; x < NIXIE_W; ++x) g[x] = rn.next();   // next glyph's bottom, entering from top
        else        for (int x = 0; x < NIXIE_W; ++x) g[x] = rc.next();   // current glyph, slid down
        for (int x = 0; x < NIXIE_W; ++x) { uint16_t v = (uint16_t)bl[x] + g[x]; row[x] = v > 255 ? 255 : (uint8_t)v; }
        sink(y, row);
    }
}

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
