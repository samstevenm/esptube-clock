// ============================================================================
// esp1.cpp — ESP/1 frame parser + TCP server. See esp1.h / docs/ESP1-PROTOCOL.md.
// ============================================================================
#include "esp1.h"
#include "config.h"
#include "tubes.h"
#include "leds.h"
#include "control.h"
#include <WiFi.h>
#include <WiFiUdp.h>
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <mbedtls/sha1.h>
#include <mbedtls/base64.h>
#include "shell.h"
#include "esp1_frame.h"

namespace {

WiFiServer g_srv(Esp1::PORT);
WiFiServer g_wss(Esp1::WS_PORT);
WiFiClient g_cli[Esp1::MAX_TCP];
Esp1::Parser g_tcp[Esp1::MAX_TCP];
uint32_t   g_lastActive[Esp1::MAX_TCP];      // millis of the last byte per slot (LRU eviction)

// ---- Minimal WebSocket (RFC 6455) on the same slots: handshake, then client→server
// frames are unmasked and fed to the ESP/1 parser (binary) or run as shell commands
// (text). Server→client frames carry acks (binary) and command replies (text).
struct WsState {
    bool ws = false;      // this slot speaks WebSocket
    uint8_t st = 0;       // 0 handshake, 1 frame header, 2 extended length, 3 mask key, 4 payload
    String hs;            // handshake request (until \r\n\r\n)
    uint8_t hdr[2], hlen = 0, extNeed = 0, ext[8], extLen = 0, mask[4], mlen = 0, mi = 0;
    uint8_t opcode = 0; bool masked = false;
    uint32_t plen = 0, pleft = 0;
    String txt;           // text frame (a shell command line)
};
WsState g_ws[Esp1::MAX_TCP];

void wsSendFrame(WiFiClient& c, uint8_t opcode, const uint8_t* d, size_t n) {
    uint8_t h[10]; size_t hl = 0;
    h[hl++] = (uint8_t)(0x80 | opcode);
    if (n < 126) h[hl++] = (uint8_t)n;
    else if (n < 65536) { h[hl++] = 126; h[hl++] = (uint8_t)(n >> 8); h[hl++] = (uint8_t)n; }
    else { h[hl++] = 127; for (int i = 7; i >= 0; --i) h[hl++] = (uint8_t)(((uint64_t)n >> (8 * i)) & 0xFF); }
    c.write(h, hl); if (n) c.write(d, n);
}
struct StringPrint : public Print { String s; size_t write(uint8_t b) override { s += (char)b; return 1; } size_t write(const uint8_t* b, size_t n) override { for (size_t i = 0; i < n; ++i) s += (char)b[i]; return n; } };

bool wsHandshake(WiFiClient& c, const String& req) {
    int k = req.indexOf("Sec-WebSocket-Key:"); if (k < 0) return false;
    int e = req.indexOf("\r\n", k); String key = req.substring(k + 18, e); key.trim();
    key += "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    uint8_t sha[20]; mbedtls_sha1((const unsigned char*)key.c_str(), key.length(), sha);
    unsigned char b64[32]; size_t olen = 0; mbedtls_base64_encode(b64, sizeof b64, &olen, sha, 20);
    String resp = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ";
    resp += String((const char*)b64, olen); resp += "\r\n\r\n";
    c.print(resp); return true;
}
WiFiUDP    g_udp;
IPAddress  g_udpRemote; uint16_t g_udpPort = 0;
Esp1::Parser g_udpP;
Esp1::Stats  g_stats;
uint32_t g_winMs = 0, g_winFrames = 0, g_winBytes = 0;
String   g_token;

void tcpSink(void* ctx, const uint8_t* d, size_t n) {
    WiFiClient* c = (WiFiClient*)ctx; if (!c || !*c) return;
    const int slot = (int)(c - g_cli);
    if (slot >= 0 && slot < Esp1::MAX_TCP && g_ws[slot].ws) wsSendFrame(*c, 0x2, d, n); else c->write(d, n);
}
void udpSink(void*, const uint8_t* d, size_t n) {
    if (!g_udpPort) return;
    g_udp.beginPacket(g_udpRemote, g_udpPort); g_udp.write(d, n); g_udp.endPacket();
}

inline uint16_t be565(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }

}  // namespace

namespace Esp1 {

// ---- Parser ------------------------------------------------------------------
void Parser::reset() {
    st = 0; hlen = 0; remain = 0; rowFill = 0; pixFill = 0; palFill = 0; rleFill = 0; pixLeft = 0; ledFill = 0;
    authLen = 0; authed = g_token.length() == 0;   // no token configured = open
    authFails = 0; wantClose = false;
    if (!authed) st = 3;
}

// Legacy clients (gen 0, flags 0) get byte 3 = 0 exactly as before; a 1.1 client gets the node's epoch.
void Parser::ack() {
    uint8_t a[4] = { 0xA5, seq, status, (uint8_t)(v11 ? Tubes::epoch() : 0) };
    if (sink) sink(sinkCtx, a, 4);
}
void Parser::fail(uint8_t s) { status = s; ack(); abortFrame(); }
void Parser::abortFrame() {
    if (st == 1 && op == 1) Tubes::abortBlit(blit);
    if (st != 3) st = 0;
    hlen = 0; remain = 0;
}

void Parser::pushRow(const uint16_t* px) { Tubes::pushBlitLine(blit, px); }

void Parser::beginFrame() {
    op = hdr[2]; tube = hdr[3];
    x = hdr[4] | (hdr[5] << 8); y = hdr[6] | (hdr[7] << 8);
    w = hdr[8] | (hdr[9] << 8); h = hdr[10] | (hdr[11] << 8);
    fmt = hdr[12]; seq = hdr[13]; gen = hdr[14]; flags = hdr[15];
    v11 = (gen | flags) != 0; advanced = false;
    len = (uint32_t)hdr[16] | ((uint32_t)hdr[17] << 8) | ((uint32_t)hdr[18] << 16) | ((uint32_t)hdr[19] << 24);
    remain = len; status = 0; rowFill = 0; pixFill = 0; palFill = 0; rleFill = 0; ledFill = 0;
    st = 1;                      // payload state; a zero-length frame ends immediately below

    // A length no frame can have is a corrupt header: never swallow up to 4 GB waiting for it.
    // Ack status 3 and go straight back to hunting for the next magic.
    if (len > Esp1Frame::BLIT_MAX_LEN) { status = Esp1Frame::ST_BAD_LEN; countFrame(20, false); ack(); st = 0; hlen = 0; remain = 0; return; }

    // ESP/1.1 epoch rule for everything that paints the glass (see esp1_frame.h).
    if (op == 1 || op == 2 || op == 5) {
        Esp1Frame::GenVerdict v = Esp1Frame::genVerdict(gen, Tubes::epoch());
        if (v == Esp1Frame::GEN_ADVANCE) { if (Tubes::advanceEpochTo(gen)) advanced = true; else v = Esp1Frame::GEN_STALE; }
        if (v == Esp1Frame::GEN_STALE) { status = Esp1Frame::ST_SUPERSEDED; st = 2; }
    }

    if (st == 1) switch (op) {
        case 1: {   // BLIT
            if (fmt > 3 || w == 0 || h == 0 || w > PANEL_W || h > PANEL_H) { status = 2; st = 2; break; }
            if (Esp1Frame::blitLenCheck(fmt, w, h, len) != 0) { status = Esp1Frame::ST_BAD_LEN; st = 2; break; }   // wrong but bounded: skip it
            if (!tubeIndexValid(tube) || !Tubes::beginBlit(blit, tube, x, y, w, h)) { status = 1; st = 2; break; }
            Control::notifyManualContent();
            rowBytes = (fmt >= 2) ? (uint16_t)((w + 1) / 2) : (uint16_t)(w * 2);   // PAL4 / PAL4x2: nibbles
            pixLeft = (uint32_t)w * h;
            break; }
        case 2:     // FILL: payload 2 bytes; window as given (tube 0xFF = all)
            if (len != 2) { status = 3; st = 2; }
            break;
        case 4:     // LED: 18 bytes
            if (len != 18) { status = 3; st = 2; }
            break;
        case 3: case 5: case 6:   // PING / CLEAR / CANCEL: no payload
            if (len != 0) { status = 3; st = 2; }
            break;
        case 7:     // ECHO: any payload up to 4 KB; the reply reports the CRC16 the node computed over what it received
            if (len > 4096) { status = 3; st = 2; } else echoCrc = 0xFFFF;
            break;
        default: status = 2; st = 2;
    }
    if (remain == 0) endFrame();
}

void Parser::endFrame() {
    const bool skipped = (st == 2);
    if (!skipped) {
        switch (op) {
            case 1: Tubes::endBlit(blit);
                    if (blit.aborted) status = Esp1Frame::ST_SUPERSEDED;      // a clear / CANCEL / mode change won mid-frame
                    else if (pixLeft != 0) status = 3;
                    break;
            case 5: if (!advanced) Tubes::supersede();                            // CLEAR: whatever else is arriving is stale now
                    Control::notifyManualContent(); Tubes::clearAll(); break;
            case 6: {                                                              // CANCEL: stop drawing what is in flight, everywhere
                const Esp1Frame::GenVerdict v = Esp1Frame::genVerdict(gen, Tubes::epoch());
                if (v == Esp1Frame::GEN_STALE) { status = Esp1Frame::ST_SUPERSEDED; break; }   // a late duplicate must not cancel newer content
                if (v == Esp1Frame::GEN_ADVANCE) Tubes::advanceEpochTo(gen); else Tubes::supersede();
                if (flags & Esp1Frame::FLAG_CLEAR) Control::masterClearFromTask(false);   // the epoch was just bumped above: exactly one step per CANCEL
                break; }
            default: break;   // FILL/LED are applied in payload(); PING has nothing to do
        }
    } else if (op == 1) {
        Tubes::abortBlit(blit);
    }
    if (status == Esp1Frame::ST_SUPERSEDED) g_stats.superseded++;
    countFrame(20 + len, status == 0);
    if (op == 7 && status == 0) {                                     // ECHO reply: A7 seq crcHi crcLo lenLo lenHi epoch 00
        const uint8_t r[8] = { 0xA7, seq, (uint8_t)(echoCrc >> 8), (uint8_t)echoCrc, (uint8_t)len, (uint8_t)(len >> 8), Tubes::epoch(), 0 };
        if (sink) sink(sinkCtx, r, 8);
    } else if (!(flags & Esp1Frame::FLAG_NOACK) || status != 0) ack();   // NOACK only silences success
    st = 0; hlen = 0;
}

// Consume payload bytes for the current frame; returns how many were used.
size_t Parser::payload(const uint8_t* p, size_t n) {
    size_t take = (n < remain) ? n : (size_t)remain;
    if (st == 2) { remain -= take; if (remain == 0) endFrame(); return take; }   // skipping a bad frame

    if (op == 1) {
        size_t i = 0;
        if (fmt != 1) {
            // wire rows: RAW = w*2 bytes; PAL4 / PAL4x2 = palette(32) then ceil(w/2) bytes per row.
            // Decoded straight into the SPI block buffer (no staging copy); PAL4x2 draws each
            // payload row twice (old-school line doubling: half the bytes, same picture height).
            while (i < take) {
                if (fmt >= 2 && palFill < 32) { pal[palFill / 2] = (palFill & 1) ? (uint16_t)(pal[palFill / 2] | p[i]) : (uint16_t)(p[i] << 8); palFill++; i++; continue; }
                size_t want = rowBytes - rowFill, avail = take - i, c = avail < want ? avail : want;
                memcpy(row + rowFill, p + i, c); rowFill += c; i += c;
                if (rowFill == rowBytes) {
                    rowFill = 0;
                    const int copies = (fmt == 3) ? 2 : 1;
                    for (int r = 0; r < copies && pixLeft; ++r) {
                        uint16_t* d = Tubes::blitRow(blit); if (!d) { pixLeft = 0; break; }
                        if (fmt == 0) { for (uint16_t k = 0; k < w; ++k) d[k] = be565(row + 2 * k); }
                        else          { for (uint16_t k = 0; k < w; ++k) d[k] = pal[(k & 1) ? (row[k / 2] & 0x0F) : (row[k / 2] >> 4)]; }
                        Tubes::blitRowDone(blit);
                        if (pixLeft >= w) pixLeft -= w; else pixLeft = 0;
                    }
                }
            }
        } else {   // RLE565: {count, hi, lo}; runs may cross rows
            while (i < take) {
                rle[rleFill++] = p[i++];
                if (rleFill == 3) {
                    rleFill = 0;
                    uint16_t cnt = rle[0] ? rle[0] : 1, c = (uint16_t)((rle[1] << 8) | rle[2]);
                    while (cnt-- && pixLeft) {
                        pix[pixFill++] = c; pixLeft--;
                        if (pixFill == w) { pushRow(pix); pixFill = 0; }
                    }
                }
            }
        }
    } else if (op == 2) {
        for (size_t i = 0; i < take && ledFill < 2; ++i) ledBuf[ledFill++] = p[i];
        if (ledFill == 2) {
            const uint16_t c = be565(ledBuf);
            Control::notifyManualContent();
            if (tube == 0xFF) { for (uint8_t t = 0; t < TUBE_COUNT; ++t) Tubes::fillRect(t, x, y, w, h, c); }
            else if (tubeIndexValid(tube)) Tubes::fillRect(tube, x, y, w, h, c);
            else status = 1;
        }
    } else if (op == 7) {
        echoCrc = Esp1Frame::crc16(p, take, echoCrc);
    } else if (op == 4) {
        for (size_t i = 0; i < take && ledFill < 18; ++i) ledBuf[ledFill++] = p[i];
        if (ledFill == 18) { for (uint8_t t = 0; t < TUBE_COUNT; ++t) Leds::setTube(t, ledBuf[t * 3], ledBuf[t * 3 + 1], ledBuf[t * 3 + 2]); Leds::show(); }
    }
    remain -= take;
    if (remain == 0) endFrame();
    return take;
}

void Parser::feed(const uint8_t* p, size_t n) {
    size_t i = 0;
    while (i < n) {
        if (st == 3) {                                   // auth line: "HELLO <token>\n"
            char c = (char)p[i++];
            if (c == '\n') {
                auth[authLen] = 0;
                const bool ok = (authLen > 6 && !strncmp(auth, "HELLO ", 6) && g_token == String(auth + 6));
                const char* r = ok ? "OK\n" : "NO\n";
                if (sink) sink(sinkCtx, (const uint8_t*)r, 3);
                if (ok) { authed = true; st = 0; }
                else { authLen = 0; if (++authFails >= 5) wantClose = true; }   // no free retries forever on one socket
            } else if (authLen < sizeof(auth) - 1) auth[authLen++] = c;
            continue;
        }
        if (st == 0) {                                   // header
            if (hlen == 0 && p[i] != 0xE5) { i++; continue; }          // resync on magic
            if (hlen == 1 && p[i] != 0x7B) { hlen = 0; continue; }
            hdr[hlen++] = p[i++];
            if (hlen == 20) beginFrame();
            continue;
        }
        i += payload(p + i, n - i);                      // st 1 or 2
    }
}

// At most one frame: stops as soon as the parser is idle again, so a byte stream shared with
// something else (the UART shell) gets its bytes back. E5 that is not followed by 7B = not a frame.
size_t Parser::feedFrame(const uint8_t* p, size_t n, bool& done) {
    size_t i = 0; done = false;
    while (i < n) {
        if (st == 0) {
            if (hlen == 0 && p[i] != 0xE5) { done = true; return i; }
            if (hlen == 1 && p[i] != 0x7B) { hlen = 0; done = true; return i; }
            hdr[hlen++] = p[i++];
            if (hlen == 20) { beginFrame(); if (st == 0 && hlen == 0) { done = true; return i; } }
            continue;
        }
        if (st == 3) { st = 0; continue; }               // (no auth on the transports that use this)
        i += payload(p + i, n - i);
        if (st == 0 && hlen == 0) { done = true; return i; }
    }
    return i;
}

// ---- TCP server + counters ----------------------------------------------------
bool service();
void begin() {
    Preferences pr; if (pr.begin("esptube", true)) { g_token = pr.getString("token", ""); pr.end(); }
    g_srv.begin(); g_srv.setNoDelay(true);
    g_wss.begin(); g_wss.setNoDelay(true);
    for (uint8_t i = 0; i < MAX_TCP; ++i) { g_tcp[i].sink = tcpSink; g_tcp[i].sinkCtx = &g_cli[i]; g_tcp[i].reset(); }
    g_udp.begin(UDP_PORT);
    g_udpP.sink = udpSink; g_udpP.reset(); g_udpP.authed = true; g_udpP.st = 0;   // no per-datagram auth
    Serial.printf("[stream] ESP/1 on tcp:%u udp:%u ws:%u (%s)\n", PORT, UDP_PORT, WS_PORT, g_token.length() ? "token" : "open");
    // Own task: the main loop only reached ~30 iterations/s under load, and lwIP
    // opens the receive window only when the app reads — so draining here, with
    // a 1 ms sleep when idle, is what lets the sockets actually flow.
    // Always yield a tick per pass: a busy pass that never sleeps starves core 0's
    // idle task and trips the task watchdog (seen as reset_reason task_wdt).
    xTaskCreatePinnedToCore([](void*) { for (;;) { service(); vTaskDelay(1); } }, "esp1", 8192, nullptr, 1, nullptr, 1);   // core 1 at loop() priority: time-sliced with it; core 0 stays WiFi/lwIP's
}

// Feed raw socket bytes of a WebSocket slot: handshake, deframe, unmask, dispatch.
static void wsFeed(uint8_t slot, const uint8_t* p, size_t n) {
    WsState& w = g_ws[slot]; WiFiClient& c = g_cli[slot];
    size_t i = 0;
    while (i < n) {
        if (w.st == 0) {                                        // HTTP upgrade request
            w.hs += (char)p[i++];
            if (w.hs.endsWith("\r\n\r\n")) {
                if (!wsHandshake(c, w.hs)) { c.stop(); return; }
                w.hs = ""; w.st = 1; w.hlen = 0;
            } else if (w.hs.length() > 2048) { c.stop(); return; }
            continue;
        }
        if (w.st == 1) {                                        // 2-byte frame header
            w.hdr[w.hlen++] = p[i++];
            if (w.hlen < 2) continue;
            w.opcode = w.hdr[0] & 0x0F; w.masked = (w.hdr[1] & 0x80) != 0;
            const uint8_t l7 = w.hdr[1] & 0x7F;
            w.extNeed = (l7 == 126) ? 2 : (l7 == 127) ? 8 : 0; w.extLen = 0; w.plen = l7; w.mlen = 0; w.mi = 0;
            w.st = w.extNeed ? 2 : (w.masked ? 3 : 4); w.hlen = 0;
            if (w.st == 4) { w.pleft = w.plen; if (w.plen == 0) goto frame_end; }
            continue;
        }
        if (w.st == 2) {                                        // extended length
            w.ext[w.extLen++] = p[i++];
            if (w.extLen < w.extNeed) continue;
            w.plen = 0; for (uint8_t k = 0; k < w.extNeed; ++k) w.plen = (w.plen << 8) | w.ext[k];
            w.st = w.masked ? 3 : 4; if (w.st == 4) { w.pleft = w.plen; if (w.plen == 0) goto frame_end; }
            continue;
        }
        if (w.st == 3) {                                        // masking key
            w.mask[w.mlen++] = p[i++];
            if (w.mlen < 4) continue;
            w.st = 4; w.pleft = w.plen; if (w.plen == 0) goto frame_end;
            continue;
        }
        {                                                       // payload
            size_t take = (n - i < w.pleft) ? (n - i) : (size_t)w.pleft;
            static uint8_t un[1472]; if (take > sizeof un) take = sizeof un;
            for (size_t k = 0; k < take; ++k) un[k] = w.masked ? (uint8_t)(p[i + k] ^ w.mask[(w.mi + k) & 3]) : p[i + k];
            w.mi = (uint8_t)((w.mi + take) & 3); i += take; w.pleft -= take;
            if (w.opcode == 0x2 || w.opcode == 0x0) g_tcp[slot].feed(un, take);          // ESP/1 bytes
            else if (w.opcode == 0x1) { for (size_t k = 0; k < take && w.txt.length() < 200; ++k) w.txt += (char)un[k]; }
            if (w.pleft) continue;
        }
frame_end:
        if (w.opcode == 0x8) { wsSendFrame(c, 0x8, nullptr, 0); c.stop(); return; }
        if (w.opcode == 0x9) wsSendFrame(c, 0xA, nullptr, 0);
        if (w.opcode == 0x1) {   // a shell line: executed by loop() (it owns Control/NVS state); the reply comes back via Shell::takeReply()
            if (!Shell::post(w.txt.c_str(), w.txt.length(), (int8_t)slot)) { const char* e = "ERR busy (command queue full)\n"; wsSendFrame(c, 0x1, (const uint8_t*)e, strlen(e)); }
            w.txt = ""; }
        w.st = 1; w.hlen = 0;
    }
}

// One service pass over UDP + every TCP/WebSocket client. Returns true if any bytes were consumed.
bool service() {
    bool any = false;
    // WiFiManager (re)initialises the radio after our early setSleep(false); re-assert
    // once the link is up. Modem sleep = ~100 ms per TCP ack = a few frames/s at best.
    static bool noSleepApplied = false;
    if (!noSleepApplied && WiFi.status() == WL_CONNECTED) { WiFi.setSleep(false); noSleepApplied = true; }

    // Up to MAX_TCP clients — the bridge opens one connection per tube so each
    // gets its own lwIP receive window. A newcomer takes a free slot, else the
    // oldest (round-robin) — a controller that died without a FIN never blocks.
    for (int which = 0; which < 2; ++which) {
        WiFiServer& srv = which ? g_wss : g_srv;
        while (srv.hasClient()) {
            if (ESP.getFreeHeap() < 40000) {                         // a starving heap must not take on another stream
                WiFiClient c = srv.accept(); c.stop();
                Serial.printf("[esp1] low heap (%u) — refused a stream client\n", (unsigned)ESP.getFreeHeap()); continue;
            }
            // a free slot first; otherwise evict the LEAST RECENTLY ACTIVE one — a controller that
            // just reconnected all five tubes must not bump its own fresh sockets (that was a churn loop)
            uint8_t slot = MAX_TCP;
            for (uint8_t i = 0; i < MAX_TCP; ++i) if (!g_cli[i] || !g_cli[i].connected()) { slot = i; break; }
            if (slot == MAX_TCP) {
                // No free slot. With a token set, an UNauthenticated newcomer must not be able to bump an
                // authed, recently-active session (that would be an auth-bypass DoS): skip protected slots
                // when choosing the LRU victim, and refuse the newcomer if every slot is authed and fresh.
                uint32_t oldest = 0xFFFFFFFFu;
                for (uint8_t i = 0; i < MAX_TCP; ++i) {
                    if (g_token.length() && g_tcp[i].authed && (uint32_t)(millis() - g_lastActive[i]) < 10000) continue;
                    if (g_lastActive[i] < oldest) { oldest = g_lastActive[i]; slot = i; }
                }
                if (slot == MAX_TCP) { WiFiClient c = srv.accept(); c.stop(); Serial.println("[esp1] all slots authed & active — refused a newcomer"); continue; }
            }
            if (g_cli[slot]) g_cli[slot].stop();
            g_lastActive[slot] = millis();
            g_cli[slot] = srv.accept(); g_cli[slot].setNoDelay(true); g_tcp[slot].reset();
            g_ws[slot] = WsState(); g_ws[slot].ws = (which == 1);
            // WS clients authenticate like TCP ones: with a token set, the first binary
            // frame must be "HELLO <token>\n" (the parser starts in its auth state).
            Serial.printf("[stream] %s client %s -> slot %u\n", which ? "ws" : "tcp", g_cli[slot].remoteIP().toString().c_str(), slot);
        }
    }
    Shell::rxPump();                                         // the UART is an ESP/1 transport too: drained here, not in loop()
    { int8_t slot; String reply;                           // WebSocket shell replies are sent from THIS task (one writer per socket)
      while (Shell::takeReply(slot, reply)) if (slot >= 0 && slot < MAX_TCP && g_cli[slot] && g_cli[slot].connected() && g_ws[slot].ws) wsSendFrame(g_cli[slot], 0x1, (const uint8_t*)reply.c_str(), reply.length()); }
    static uint8_t buf[1472];
    // UDP first: datagrams are the same byte stream; drain up to ~64 per loop.
    // With a token configured the UDP path is disabled (no per-datagram auth).
    if (!g_token.length()) {
        for (int k = 0; k < 64; ++k) {
            int n = g_udp.parsePacket(); if (n <= 0) break;
            int r = g_udp.read(buf, sizeof buf); if (r <= 0) break;
            g_udpRemote = g_udp.remoteIP(); g_udpPort = g_udp.remotePort();
            g_udpP.abortFrame();                          // one datagram = one self-contained frame: a lost one must not desync the next
            g_udpP.feed(buf, (size_t)r); any = true;
        }
    }
    for (uint8_t i = 0; i < MAX_TCP; ++i) {
        if (!g_cli[i]) continue;
        if (!g_cli[i].connected()) { g_cli[i].stop(); continue; }
        int budget = 8;                                  // ≤ ~12 KB per client per loop()
        while (budget-- && g_cli[i].available()) {
            int n = g_cli[i].read(buf, sizeof buf);
            if (n <= 0) break;
            const uint32_t t0 = micros();
            if (g_ws[i].ws) wsFeed(i, buf, (size_t)n); else g_tcp[i].feed(buf, (size_t)n);
            any = true; g_lastActive[i] = millis();
            g_stats.feedUs += micros() - t0;
        }
        if (g_tcp[i].wantClose) { Serial.printf("[stream] slot %u failed auth — closed\n", i); g_cli[i].stop(); continue; }
        // A sender that stopped mid-frame would leave this parser eating the NEXT frame's header as
        // pixels. On a byte stream there is no safe resync, so close the socket; the client reconnects.
        if ((g_tcp[i].st == 1 || g_tcp[i].st == 2) && millis() - g_lastActive[i] > 5000) {
            Serial.printf("[stream] slot %u stalled mid-frame for 5 s — closed\n", i);
            g_tcp[i].abortFrame(); g_cli[i].stop(); g_stats.stalled++;
        }
    }
    g_stats.loops++;
    // fps / kbps over a 1 s window
    const uint32_t now = millis();
    if (now - g_winMs >= 1000) {
        g_stats.fps  = g_winFrames * 1000.0f / (float)(now - g_winMs);
        g_stats.kbps = (uint32_t)(g_winBytes / 1024.0f * 1000.0f / (float)(now - g_winMs));
        g_stats.busyPct = (uint8_t)((g_stats.feedUs / 10) / (now - g_winMs));      // % of wall time inside feed()
        g_stats.loopsPerSec = g_stats.loops * 1000 / (now - g_winMs);
        g_stats.feedUs = 0; g_stats.loops = 0;
        g_winMs = now; g_winFrames = 0; g_winBytes = 0;
    }
    return any;
}

void pump() {}   // the stream runs on its own task now; kept so main.cpp needs no change

uint8_t tcpClients() { uint8_t n = 0; for (uint8_t i = 0; i < MAX_TCP; ++i) if (g_cli[i] && g_cli[i].connected()) n++; return n; }
bool tcpConnected() { return tcpClients() > 0; }
const Stats& stats() { return g_stats; }
void countFrame(uint32_t bytes, bool ok) {
    g_stats.frames++; g_stats.bytes += bytes; g_stats.lastFrameMs = millis();
    if (!ok) g_stats.bad++;
    g_winFrames++; g_winBytes += bytes;
}
void setToken(const String& t) { g_token = t; Preferences pr; if (pr.begin("esptube", false)) { pr.putString("token", t); pr.end(); } }
String token() { return g_token; }

}  // namespace Esp1
