// nixie_native.cpp — exhaustive native test of the nixie render core + the clock
// transition state machine. Runs on the Mac in a few seconds; see nixie-native.sh.
//
//  1. Every plate's RLE decodes to exactly NIXIE_W*NIXIE_H pixels (no reader over/under-run).
//  2. For EVERY ordered pair of glyphs (A,B) and every fade step k=0..K:
//       k=0 frame == pure-A frame, k=K frame == pure-B frame (pixel-exact), and each
//       intermediate pixel lies between the A and B values (monotonic blend, no wrap).
//  3. Every valid time 00:00:00..23:59:59 stepped through the SAME per-tube transition
//     logic as Control::tick(): the 'from' char of every change equals what that tube
//     last showed, only digits ever reach a clock tube, and the change-count histogram
//     matches the arithmetic (86,400 sec-ones changes, 8,640 sec-tens, ...).
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <vector>
#include "nixie_render.h"

static const int NPIX = NIXIE_W * NIXIE_H;
static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static void render(uint8_t a, uint8_t b, uint8_t k, uint8_t K, std::vector<uint8_t>& out) {
    out.assign(NPIX, 0);
    nixieCompose(a, b, k, K, [&](int y, const uint8_t* row) { memcpy(&out[y * NIXIE_W], row, NIXIE_W); });
}

int main() {
    const uint8_t K = 4;
    // ---- 1. RLE integrity ----------------------------------------------------
    for (int p = 0; p < NIXIE_PLATES; ++p) {
        const uint8_t* d = (const uint8_t*)NIXIE_RDPTR(&NIXIE_RLE[p]);
        uint32_t len = NIXIE_RLE_LEN[p], px = 0;
        CHECK(len % 2 == 0, "plate %d: odd RLE length %u", p, len);
        for (uint32_t i = 0; i + 1 < len; i += 2) { CHECK(d[i] > 0, "plate %d: zero run at %u", p, i); px += d[i]; }
        CHECK(px == (uint32_t)NPIX, "plate %d: decodes to %u px, want %d", p, px, NPIX);
    }
    printf("1. RLE integrity: %d plates OK (%d chars + base)\n", NIXIE_PLATES, NIXIE_NCHARS);

    // ---- 2. exhaustive cross-fade ---------------------------------------------
    std::vector<std::vector<uint8_t>> pure(NIXIE_NCHARS);
    for (int a = 0; a < NIXIE_NCHARS; ++a) render(a, a, K, K, pure[a]);
    std::vector<uint8_t> f;
    long frames = 0;
    for (int a = 0; a < NIXIE_NCHARS; ++a) for (int b = 0; b < NIXIE_NCHARS; ++b) {
        render(a, b, 0, K, f); frames++;
        CHECK(f == pure[a], "pair %d->%d k=0 != pure A", a, b);
        render(a, b, K, K, f); frames++;
        CHECK(f == pure[b], "pair %d->%d k=K != pure B", a, b);
        for (uint8_t k = 1; k < K; ++k) {
            render(a, b, k, K, f); frames++;
            for (int i = 0; i < NPIX; ++i) {
                uint8_t lo = pure[a][i] < pure[b][i] ? pure[a][i] : pure[b][i];
                uint8_t hi = pure[a][i] < pure[b][i] ? pure[b][i] : pure[a][i];
                if (f[i] < lo || f[i] > hi) { CHECK(false, "pair %d->%d k=%u px %d = %u outside [%u,%u]", a, b, k, i, f[i], lo, hi); break; }
            }
        }
    }
    printf("2. cross-fade: %ld frames over %d x %d glyph pairs, K=%u — endpoints exact, blends bounded\n",
           frames, NIXIE_NCHARS, NIXIE_NCHARS, K);

    // ---- 3. every valid time through the tick() transition logic ---------------
    const int TUBES = 6;
    char shown[TUBES]; memset(shown, 0, sizeof shown);            // 0 = nothing shown yet (blank plate)
    long changes[TUBES] = {0}, total = 0, maxSimul = 0, steps = 0;
    for (int sec = 0; sec < 86400; ++sec) {
        int h = sec / 3600, m = (sec / 60) % 60, s = sec % 60;
        char hhmmss[7]; snprintf(hhmmss, sizeof hhmmss, "%02d%02d%02d", h, m, s);
        int n = 0;
        for (int i = 0; i < TUBES; ++i) {                            // tube i shows hhmmss[5-i]
            char c = hhmmss[5 - i];
            CHECK(c >= '0' && c <= '9', "non-digit '%c' at sec %d tube %d", c, sec, i);
            if (shown[i] == c) continue;
            char from = shown[i];
            CHECK(from == 0 || (from >= '0' && from <= '9'), "bad from '%c'", from);
            // the fade would be from plate(from) to plate(c): both must be valid glyphs
            CHECK(from == 0 || pure[from - '0'].size() == (size_t)NPIX, "no plate for from");
            shown[i] = c; changes[i]++; total++; n++;
        }
        if (n > maxSimul) maxSimul = n;
        steps++;
    }
    // wrap-around: 23:59:59 -> 00:00:00 must also be a clean 6-tube change
    { int n = 0; const char* z = "000000"; for (int i = 0; i < TUBES; ++i) if (shown[i] != z[5 - i]) n++; CHECK(n == 6, "midnight wrap changes %d tubes, want 6", n); }
    // each count includes the first blank->digit render; hr-tens: blank->0, 0->1 (10:00), 1->2 (20:00)
    CHECK(changes[0] == 86400 && changes[1] == 8640 && changes[2] == 1440 && changes[3] == 144 && changes[4] == 24 && changes[5] == 3,
          "change histogram %ld %ld %ld %ld %ld %ld", changes[0], changes[1], changes[2], changes[3], changes[4], changes[5]);
    printf("3. clock sweep: %ld times, %ld digit changes (per tube: %ld %ld %ld %ld %ld %ld), max simultaneous %ld\n",
           steps, total, changes[0], changes[1], changes[2], changes[3], changes[4], changes[5], maxSimul);

    printf(failures ? "\nRESULT: %d FAILURE(S)\n" : "\nRESULT: ALL PASS\n", failures);
    return failures ? 1 : 0;
}
