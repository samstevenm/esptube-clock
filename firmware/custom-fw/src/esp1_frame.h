#pragma once
// ============================================================================
// esp1_frame.h — hardware-independent ESP/1.1 helpers, shared by the firmware
// (esp1.cpp, shell.cpp) and the native test (test/esp1_frame_native.cpp), so the
// exact code that frames bytes on the wire is what the test fuzzes.
//
//   epoch      node-owned content generation (1..255); see genVerdict()
//   blitLenCheck   exact BLIT payload length per format (a corrupt len must never wedge a parser)
//   crc16      CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF; "123456789" -> 0x29B1)
//   cobs*      Consistent Overhead Byte Stuffing: no 0x00 inside a frame, so 0x00 delimits
//   UartFramer the UART byte-stream splitter: text lines | legacy raw frames | COBS datagrams
// ============================================================================
#include <stdint.h>
#include <stddef.h>
#include <string.h>

namespace Esp1Frame {

// ---- header flags (byte 15) ----
const uint8_t FLAG_NOACK = 0x01;   // don't ack a frame that drew fine (errors are always acked)
const uint8_t FLAG_CLEAR = 0x04;   // on CANCEL: also master-clear the glass
const uint8_t FLAG_V11   = 0x80;   // "I speak 1.1": any non-zero gen/flags puts the node's epoch in ack byte 3 — a client that
                                   // does not know the epoch yet sends PING with this bit to learn it
// ---- ack status ----
const uint8_t ST_OK = 0, ST_BAD_TUBE = 1, ST_BAD_FMT = 2, ST_BAD_LEN = 3, ST_SUPERSEDED = 4, ST_BAD_CRC = 5;

// ---- epoch ------------------------------------------------------------------
// The NODE owns the epoch (a client-owned counter starves the second writer). Header byte 14:
//   0            legacy: adopt whatever the epoch is now
//   == epoch     draw
//   == succ      the sender already knows about a supersede it (or its CANCEL on another
//                socket) caused: advance, then draw
//   anything else  stale content: skip it, ack status 4, ack byte 3 carries the true epoch
inline uint8_t epochSucc(uint8_t e) { return e >= 255 ? (uint8_t)1 : (uint8_t)(e + 1); }
enum GenVerdict : uint8_t { GEN_DRAW = 0, GEN_ADVANCE = 1, GEN_STALE = 2 };
inline GenVerdict genVerdict(uint8_t gen, uint8_t epoch) {
    if (gen == 0 || gen == epoch) return GEN_DRAW;
    return gen == epochSucc(epoch) ? GEN_ADVANCE : GEN_STALE;
}

// ---- BLIT payload length ------------------------------------------------------
// 0 = exact, 1 = wrong but bounded (skip `len` bytes, ack status 3), 2 = insane (don't trust
// it: ack status 3 and hunt for the next magic instead of swallowing up to 4 GB).
const uint32_t BLIT_MAX_LEN = 135u * 240u * 3u;   // worst case: RLE565, every pixel its own run
inline int blitLenCheck(uint8_t fmt, uint16_t w, uint16_t h, uint32_t len) {
    if (len > BLIT_MAX_LEN) return 2;
    const uint32_t rb = ((uint32_t)w + 1) / 2;
    switch (fmt) {
        case 0: return len == (uint32_t)w * h * 2 ? 0 : 1;
        case 1: return (len >= 3 && len % 3 == 0 && len <= (uint32_t)w * h * 3) ? 0 : 1;
        case 2: return len == 32 + rb * h ? 0 : 1;
        case 3: return len == 32 + rb * (((uint32_t)h + 1) / 2) ? 0 : 1;
    }
    return 1;
}

// ---- CRC-16/CCITT-FALSE (nibble table: 32 bytes of flash, no RAM) ----------------
inline uint16_t crc16(const uint8_t* p, size_t n, uint16_t crc = 0xFFFF) {
    static const uint16_t T[16] = { 0x0000, 0x1021, 0x2042, 0x3063, 0x4084, 0x50A5, 0x60C6, 0x70E7,
                                    0x8108, 0x9129, 0xA14A, 0xB16B, 0xC18C, 0xD1AD, 0xE1CE, 0xF1EF };
    while (n--) {
        const uint8_t b = *p++;
        crc = (uint16_t)((crc << 4) ^ T[((crc >> 12) ^ (b >> 4)) & 0x0F]);
        crc = (uint16_t)((crc << 4) ^ T[((crc >> 12) ^ b) & 0x0F]);
    }
    return crc;
}

// ---- COBS ---------------------------------------------------------------------
// encode: out needs n + n/254 + 1 bytes; returns the encoded length (never contains 0x00).
inline size_t cobsEncode(const uint8_t* in, size_t n, uint8_t* out) {
    size_t o = 1, codeAt = 0; uint8_t code = 1;
    for (size_t i = 0; i < n; ++i) {
        if (in[i] == 0) { out[codeAt] = code; codeAt = o++; code = 1; }
        else { out[o++] = in[i]; if (++code == 0xFF) { out[codeAt] = code; codeAt = o++; code = 1; } }
    }
    out[codeAt] = code;
    return o;
}
// decode (in place is fine: out may equal in); returns the decoded length, or -1 if malformed.
inline long cobsDecode(const uint8_t* in, size_t n, uint8_t* out) {
    size_t i = 0, o = 0;
    while (i < n) {
        const uint8_t code = in[i++];
        if (code == 0) return -1;
        for (uint8_t k = 1; k < code; ++k) { if (i >= n) return -1; out[o++] = in[i++]; }
        if (code != 0xFF && i < n) out[o++] = 0;
    }
    return (long)o;
}

// ---- UART framer ----------------------------------------------------------------
// One byte stream carries three things. The sender always writes a datagram as 00 <cobs> 00.
//   TEXT    00 -> COBS | 0xE5 at the start of a line -> LEGACY | '\n' -> a shell line (if printable)
//   LEGACY  bytes go to the streaming ESP/1 parser until it finishes one frame -> TEXT
//   COBS    non-zero -> append | 00 with nothing buffered -> stay (resync) | 00 -> decode:
//           needs >= 22 bytes, magic E5 7B, CRC16 over everything before it, 20 + len == n - 2
// Either delimiter lost heals within one datagram (traces in the native test).
struct UartFramer {
    enum Mode : uint8_t { TEXT = 0, LEGACY = 1, COBS = 2 };
    static const size_t DG_CAP = 1600, LINE_CAP = 200;
    // callbacks (ctx is passed through)
    void   (*onLine)(void* ctx, const char* line, size_t n) = nullptr;
    void   (*onDatagram)(void* ctx, const uint8_t* frame, size_t n) = nullptr;      // header + payload, CRC verified and stripped
    size_t (*onLegacy)(void* ctx, const uint8_t* p, size_t n, bool* frameDone) = nullptr;
    void   (*onBad)(void* ctx, uint8_t status, uint8_t seq) = nullptr;               // a datagram that failed (seq = its byte 13 if decodable)
    void   (*onBadLine)(void* ctx, size_t n) = nullptr;                              // a SHORT line with control bytes in it (a mistyped / garbled command): say so, don't go silent
    void*  ctx = nullptr;
    // state
    Mode     mode = TEXT;
    char     line[LINE_CAP]; uint16_t ll = 0; bool lineJunk = false; uint16_t lineBytes = 0;
    uint8_t  dg[DG_CAP];     uint16_t dl = 0; bool overflow = false;
    uint32_t crcBad = 0, resync = 0, datagrams = 0, junkLines = 0;

    void reset() { mode = TEXT; ll = 0; lineJunk = false; dl = 0; overflow = false; }
    bool midFrame() const { return mode == LEGACY || (mode == COBS && dl > 0); }
    // the ring went quiet for too long mid-frame: drop the partial one so the shell comes back
    void idleAbort() { if (mode != TEXT) { resync++; mode = TEXT; dl = 0; overflow = false; ll = 0; lineJunk = false; } }

    void closeDatagram() {
        const bool ovf = overflow; const uint16_t n = dl; dl = 0; overflow = false;
        uint8_t seq = 0; uint8_t bad = 0;
        long m = ovf ? -1 : cobsDecode(dg, n, dg);
        if (m >= 14) seq = dg[13];
        if (m < 22 || dg[0] != 0xE5 || dg[1] != 0x7B) bad = ST_BAD_LEN;
        else {
            const uint32_t len = (uint32_t)dg[16] | ((uint32_t)dg[17] << 8) | ((uint32_t)dg[18] << 16) | ((uint32_t)dg[19] << 24);
            const uint16_t want = (uint16_t)((dg[m - 2] << 8) | dg[m - 1]);
            if (20 + len != (uint32_t)m - 2) bad = ST_BAD_LEN;
            else if (crc16(dg, (size_t)m - 2) != want) bad = ST_BAD_CRC;
        }
        if (bad) { crcBad++; if (onBad) onBad(ctx, bad, seq); }
        else { datagrams++; if (onDatagram) onDatagram(ctx, dg, (size_t)m - 2); }
    }

    void feed(const uint8_t* p, size_t n) {
        size_t i = 0;
        while (i < n) {
            if (mode == LEGACY) {
                bool done = false;
                const size_t used = onLegacy ? onLegacy(ctx, p + i, n - i, &done) : (n - i);
                i += used;
                if (done || !onLegacy) { mode = TEXT; lineJunk = (used <= 1); }   // E5 that was no frame: the rest of that line is debris
                else if (!used) i++;                                              // never spin
                continue;
            }
            const uint8_t b = p[i++];
            if (mode == COBS) {
                if (b == 0) { if (dl == 0 && !overflow) continue;      // opener after opener / lost closer: stay, resync
                              closeDatagram(); mode = TEXT; ll = 0; lineJunk = false; lineBytes = 0; continue; }
                if (dl < DG_CAP) dg[dl++] = b; else overflow = true;
                // A frame starts E5 7B, so its COBS form is <code> E5 7B … — ALWAYS. Anything else after a 0x00 is
                // not a datagram: it is shell text that followed a STRAY zero (a reset glitch when a terminal opens
                // the port). Replay it as text at once; a stray zero must never eat the next command.
                if ((dl == 2 && dg[1] != 0xE5) || (dl == 3 && dg[2] != 0x7B)) {
                    uint8_t tmp[3]; const uint16_t n = dl; memcpy(tmp, dg, n); dl = 0; overflow = false; mode = TEXT; ll = 0; lineJunk = false; lineBytes = 0; resync++;
                    feed(tmp, n);
                }
                continue;
            }
            // TEXT
            if (b == 0) { if (ll) { junkLines++; } ll = 0; lineJunk = false; lineBytes = 0; dl = 0; overflow = false; mode = COBS; continue; }
            if (b == 0xE5 && ll == 0 && !lineJunk) { mode = LEGACY; i--; continue; }   // legacy raw frame: the parser wants the magic too
            if (b == '\r') continue;
            if (b == '\n') {
                if (ll && !lineJunk) { line[ll] = 0; if (onLine) onLine(ctx, line, ll); }
                else if (lineJunk) { junkLines++; if (lineBytes < 48 && onBadLine) onBadLine(ctx, lineBytes); }   // short = a garbled command (say so); long = binary debris (stay quiet)
                ll = 0; lineJunk = false; lineBytes = 0; continue;
            }
            lineBytes++;
            // Control bytes mark debris (a datagram whose opener was lost). Bytes >= 0x80 are UTF-8 and belong in a
            // command: `nixie 21°`, `text 3 héllo` — the shell has always taken UTF-8.
            if ((b < 0x20 && b != '\t') || b == 0x7F) { lineJunk = true; continue; }
            if (ll < LINE_CAP - 1) line[ll++] = (char)b; else lineJunk = true;
        }
    }
};

}  // namespace Esp1Frame
