// Native fuzz/unit test for firmware/custom-fw/src/esp1_frame.h (no hardware).
#include "esp1_frame.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <string>
using namespace Esp1Frame;
static int fails = 0, checks = 0;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("  FAIL %s:%d  ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

struct Sink { std::vector<std::string> lines; std::vector<std::vector<uint8_t>> dgs; std::vector<int> bad; size_t legacyBytes = 0; int legacyFrames = 0; };
static void onLine(void* c, const char* l, size_t n) { ((Sink*)c)->lines.emplace_back(l, n); }
static void onDg(void* c, const uint8_t* d, size_t n) { ((Sink*)c)->dgs.emplace_back(d, d + n); }
static void onBad(void* c, uint8_t st, uint8_t) { ((Sink*)c)->bad.push_back(st); }
// fake legacy parser: a frame is 20 header bytes + len payload bytes
struct Legacy { uint8_t hdr[20]; size_t hl = 0; uint32_t remain = 0; bool inPayload = false; };
static Legacy g_leg; static Sink* g_sink;
static size_t onLegacy(void*, const uint8_t* p, size_t n, bool* done) {
    size_t i = 0;
    while (i < n) {
        if (!g_leg.inPayload) { g_leg.hdr[g_leg.hl++] = p[i++]; if (g_leg.hl == 20) { g_leg.remain = g_leg.hdr[16] | (g_leg.hdr[17] << 8); g_leg.inPayload = true; if (!g_leg.remain) { g_leg = Legacy(); g_sink->legacyFrames++; *done = true; return i; } } }
        else { size_t t = n - i < g_leg.remain ? n - i : g_leg.remain; i += t; g_leg.remain -= t; g_sink->legacyBytes += t; if (!g_leg.remain) { g_leg = Legacy(); g_sink->legacyFrames++; *done = true; return i; } }
    }
    return i;
}
static std::vector<uint8_t> frame(uint8_t op, uint8_t seq, const std::vector<uint8_t>& pay) {
    std::vector<uint8_t> f(20, 0); f[0] = 0xE5; f[1] = 0x7B; f[2] = op; f[13] = seq;
    f[16] = pay.size() & 255; f[17] = (pay.size() >> 8) & 255; f.insert(f.end(), pay.begin(), pay.end()); return f;
}
static std::vector<uint8_t> datagram(const std::vector<uint8_t>& f, bool opener = true, bool closer = true) {
    std::vector<uint8_t> raw = f; uint16_t c = crc16(raw.data(), raw.size()); raw.push_back(c >> 8); raw.push_back(c & 255);
    std::vector<uint8_t> enc(raw.size() + raw.size() / 254 + 2); size_t n = cobsEncode(raw.data(), raw.size(), enc.data()); enc.resize(n);
    std::vector<uint8_t> out; if (opener) out.push_back(0); out.insert(out.end(), enc.begin(), enc.end()); if (closer) out.push_back(0); return out;
}
static UartFramer* mk(Sink& s) { auto* f = new UartFramer(); f->ctx = &s; f->onLine = onLine; f->onDatagram = onDg; f->onBad = onBad; f->onLegacy = onLegacy; g_sink = &s; g_leg = Legacy(); return f; }
static void cat(std::vector<uint8_t>& a, const std::vector<uint8_t>& b) { a.insert(a.end(), b.begin(), b.end()); }
static void cat(std::vector<uint8_t>& a, const char* s) { while (*s) a.push_back((uint8_t)*s++); }

int main() {
    printf("esp1_frame native test\n");
    // --- crc + epoch + lengths
    CHECK(crc16((const uint8_t*)"123456789", 9) == 0x29B1, "crc16 check value");
    CHECK(epochSucc(1) == 2 && epochSucc(254) == 255 && epochSucc(255) == 1, "epochSucc wraps 255->1, never 0");
    CHECK(genVerdict(0, 7) == GEN_DRAW && genVerdict(7, 7) == GEN_DRAW && genVerdict(8, 7) == GEN_ADVANCE && genVerdict(6, 7) == GEN_STALE && genVerdict(1, 255) == GEN_ADVANCE && genVerdict(200, 5) == GEN_STALE, "genVerdict");
    CHECK(blitLenCheck(0, 135, 240, 64800) == 0 && blitLenCheck(0, 135, 240, 64799) == 1, "raw len");
    CHECK(blitLenCheck(2, 135, 240, 32 + 68 * 240) == 0 && blitLenCheck(3, 135, 240, 32 + 68 * 120) == 0 && blitLenCheck(3, 135, 33, 32 + 68 * 17) == 0, "pal4 / pal4x2 len (odd h rounds up)");
    CHECK(blitLenCheck(1, 135, 240, 3) == 0 && blitLenCheck(1, 135, 240, 4) == 1 && blitLenCheck(1, 10, 10, 303) == 1, "rle len");
    CHECK(blitLenCheck(0, 135, 240, 0xFFFFFFFFu) == 2 && blitLenCheck(1, 135, 240, BLIT_MAX_LEN + 3) == 2, "insane len");
    // --- cobs round trip, fuzz
    srand(12345);
    for (int t = 0; t < 4000; ++t) {
        size_t n = (size_t)(rand() % 1500); std::vector<uint8_t> a(n), e(n + n / 254 + 2), d(n + 8);
        for (auto& b : a) b = (uint8_t)((rand() % 4) ? rand() : 0);           // plenty of zeros
        if (t % 7 == 0) for (auto& b : a) b = 0x11;                           // long zero-free runs (0xFF code path)
        size_t m = cobsEncode(a.data(), n, e.data()); bool z = false; for (size_t i = 0; i < m; ++i) if (!e[i]) z = true;
        long k = cobsDecode(e.data(), m, d.data());
        CHECK(!z && k == (long)n && !memcmp(a.data(), d.data(), n), "cobs round trip n=%zu", n);
        long k2 = cobsDecode(e.data(), m, e.data());                          // in place
        CHECK(k2 == (long)n && !memcmp(a.data(), e.data(), n), "cobs in-place n=%zu", n);
    }
    // --- framer: text, datagram, legacy, mixed
    { Sink s; auto* f = mk(s); std::vector<uint8_t> in; cat(in, "status\r\nhelp\n"); auto A = frame(3, 9, {}); cat(in, datagram(A)); cat(in, "wifi json\n");
      auto L = frame(1, 4, std::vector<uint8_t>(300, 0xAB)); cat(in, L); cat(in, "after\n");
      for (size_t i = 0; i < in.size(); i += 7) f->feed(in.data() + i, in.size() - i < 7 ? in.size() - i : 7);
      CHECK(s.lines.size() == 4 && s.lines[0] == "status" && s.lines[2] == "wifi json" && s.lines[3] == "after", "text lines around binary (%zu)", s.lines.size());
      CHECK(s.dgs.size() == 1 && s.dgs[0] == A, "one datagram"); CHECK(s.legacyFrames == 1 && s.legacyBytes == 300, "legacy frame passthrough"); CHECK(s.bad.empty(), "no bad"); delete f; }
    // --- trace 1: opener of A lost -> A becomes a junk text line (dropped), B decodes
    { Sink s; auto* f = mk(s); auto A = frame(1, 1, std::vector<uint8_t>(900, 0x5A)), B = frame(1, 2, std::vector<uint8_t>(40, 0x33));
      std::vector<uint8_t> in; cat(in, datagram(A, false, true)); cat(in, datagram(B)); cat(in, "help\n"); f->feed(in.data(), in.size());
      CHECK(s.dgs.size() == 1 && s.dgs[0] == B, "lost opener: B decodes (%zu dgs)", s.dgs.size()); CHECK(s.lines.size() == 1 && s.lines[0] == "help", "lost opener: shell alive, no junk exec (%zu)", s.lines.size()); delete f; }
    // --- trace 2: closer of A lost -> B's opener closes A (accepted); B is a junk line; C decodes
    { Sink s; auto* f = mk(s); auto A = frame(1, 1, std::vector<uint8_t>(64, 0x5A)), B = frame(1, 2, std::vector<uint8_t>(64, 0x33)), C = frame(3, 3, {});
      std::vector<uint8_t> in; cat(in, datagram(A, true, false)); cat(in, datagram(B)); cat(in, datagram(C)); cat(in, "help\n"); f->feed(in.data(), in.size());
      CHECK(s.dgs.size() == 2 && s.dgs[0] == A && s.dgs[1] == C, "lost closer: A and C decode (%zu dgs)", s.dgs.size()); CHECK(s.lines.size() == 1, "lost closer: shell alive"); delete f; }
    // --- corruption: flipped byte -> status 5, truncated -> status 3, oversize -> dropped; stream keeps going
    { Sink s; auto* f = mk(s); auto A = frame(1, 1, std::vector<uint8_t>(500, 0x5A)); auto d = datagram(A); d[40] ^= 0x10; auto G = frame(3, 7, {});
      std::vector<uint8_t> in; cat(in, d); auto t = datagram(A); t.erase(t.begin() + 30, t.begin() + 200); cat(in, t);
      auto big = datagram(frame(1, 5, std::vector<uint8_t>(1700, 0x77))); cat(in, big); cat(in, datagram(G)); f->feed(in.data(), in.size());
      CHECK(s.bad.size() == 3 && s.bad[0] == ST_BAD_CRC && s.bad[1] == ST_BAD_LEN && s.bad[2] == ST_BAD_LEN, "bad statuses (%zu)", s.bad.size());
      CHECK(s.dgs.size() == 1 && s.dgs[0] == G, "good datagram after three bad ones"); delete f; }
    // --- a STRAY zero (the reset glitch when a terminal opens the port) must not eat the next command — nor two, nor one mid-line
    { Sink s; auto* f = mk(s); std::vector<uint8_t> in; in.push_back(0); cat(in, "fps\n"); in.push_back(0); in.push_back(0); cat(in, "status\n"); cat(in, "he"); in.push_back(0); cat(in, "lp\n"); cat(in, datagram(frame(3, 9, {}))); cat(in, "uart\n");
      for (size_t i = 0; i < in.size(); ++i) f->feed(in.data() + i, 1);
      CHECK(s.lines.size() >= 3 && s.lines[0] == "fps" && s.lines[1] == "status" && s.lines.back() == "uart", "stray zeros: commands survive (%zu lines: %s | %s)", s.lines.size(), s.lines.size() ? s.lines[0].c_str() : "", s.lines.size() > 1 ? s.lines[1].c_str() : "");
      CHECK(s.dgs.size() == 1, "a real datagram after stray zeros still decodes (%zu)", s.dgs.size()); delete f; }
    // --- UTF-8 belongs in commands (nixie 21°); control bytes do not
    { Sink s; auto* f = mk(s); std::vector<uint8_t> in; cat(in, "nixie 21\xC2\xB0\n"); cat(in, "text 3 h\xC3\xA9llo\n"); cat(in, "bad\x01line\n"); cat(in, "ok\n"); f->feed(in.data(), in.size());
      CHECK(s.lines.size() == 3 && s.lines[0] == "nixie 21\xC2\xB0" && s.lines[2] == "ok", "utf-8 lines pass, a control byte drops only that line (%zu)", s.lines.size()); delete f; }
    // --- idle abort mid-datagram brings the shell back
    { Sink s; auto* f = mk(s); auto d = datagram(frame(1, 1, std::vector<uint8_t>(500, 0x5A))); f->feed(d.data(), 200); CHECK(f->midFrame(), "mid-frame"); f->idleAbort();
      const char* l = "status\n"; f->feed((const uint8_t*)l, 7); CHECK(s.lines.size() == 1 && s.lines[0] == "status", "shell back after idle abort"); delete f; }
    // --- fuzz: random garbage between valid datagrams never loses a later valid one
    { int lost = 0;
      for (int t = 0; t < 600; ++t) { Sink s; auto* f = mk(s); std::vector<uint8_t> in; int good = 0;
        for (int k = 0; k < 6; ++k) { int g = rand() % 40; for (int j = 0; j < g; ++j) { uint8_t b = (uint8_t)rand(); if (b == 0xE5) b = 0xE4; in.push_back(b); }   // (a stray E5 at line start would legitimately start a legacy frame)
          in.push_back(0); auto F = frame(3, (uint8_t)k, std::vector<uint8_t>((size_t)(rand() % 60), 0x21)); cat(in, datagram(F)); good++; }
        f->feed(in.data(), in.size()); if ((int)s.dgs.size() != good) lost++; delete f; }
      CHECK(lost == 0, "fuzz: %d of 600 runs lost a valid datagram", lost); }
    printf("%d checks, %d failed\n%s\n", checks, fails, fails ? "FAILED" : "ALL PASS");
    return fails ? 1 : 0;
}
