#pragma once
// ============================================================================
// esp1.h — ESPTube Stream Protocol v1 ("ESP/1"): a persistent binary channel
// for frames, replacing one-HTTP-request-per-frame. Spec: docs/ESP1-PROTOCOL.md.
//
// Two transports share one parser: a TCP server on STREAM_PORT (one client) and
// the USB UART (see shell.cpp, which hands binary frames here). Rows are drawn
// as they arrive — no frame buffer — and every frame is acked when fully drawn.
// ============================================================================
#include <Arduino.h>
#include "tubes.h"

namespace Esp1 {

const uint16_t PORT = 5555;       // TCP (reliable; capped by lwIP's 5.7 KB window ÷ RTT)
const uint16_t UDP_PORT = 5556;   // UDP (same byte stream in ≤1472 B datagrams; acks go back to the sender)
const uint16_t WS_PORT  = 5557;   // WebSocket (for the browser helper): binary frames = ESP/1, text frames = shell commands

const uint8_t MAX_TCP = 6;        // one connection per tube = one lwIP window per tube

// A parser instance per transport/connection. sink(ctx, …) writes bytes back
// to that transport (acks, auth replies). Each parser owns its blit context so
// several connections can be mid-frame on different tubes.
struct Parser {
    typedef void (*Sink)(void* ctx, const uint8_t* data, size_t n);
    Sink  sink = nullptr; void* sinkCtx = nullptr;
    Tubes::BlitCtx blit;
    // ---- state (private-ish; zeroed by reset()) ----
    uint8_t  st = 0;              // 0 hdr, 1 payload, 2 skip (bad frame), 3 auth
    uint8_t  hdr[20]; uint8_t hlen = 0;
    uint8_t  op = 0, tube = 0, fmt = 0, seq = 0;
    uint8_t  gen = 0, flags = 0; bool v11 = false, advanced = false;   // ESP/1.1: header bytes 14/15 (were reserved)
    uint16_t x = 0, y = 0, w = 0, h = 0;
    uint32_t len = 0, remain = 0;
    uint8_t  status = 0;
    // blit decode state
    uint8_t  row[135 * 2]; uint16_t rowFill = 0; uint16_t rowBytes = 0;   // wire row (RAW: w*2, PAL4: ceil(w/2))
    uint16_t pix[135]; uint16_t pixFill = 0;                              // decoded row (RLE)
    uint16_t pal[16]; uint8_t palFill = 0;
    uint8_t  rle[3]; uint8_t rleFill = 0;
    uint32_t pixLeft = 0;                                                  // pixels still to decode (RLE)
    uint8_t  fillTube = 0; uint8_t ledBuf[18]; uint8_t ledFill = 0;
    char     auth[96]; uint8_t authLen = 0; bool authed = false;
    uint8_t  authFails = 0; bool wantClose = false;                       // too many bad HELLOs on one socket -> service() drops it
    void reset();
    void feed(const uint8_t* p, size_t n);
    size_t feedFrame(const uint8_t* p, size_t n, bool& done);   // at most ONE frame; done = parser idle again (UART legacy mode)
    void abortFrame();                                          // drop a half-received frame (datagram transports, stalled sockets)
    void fail(uint8_t st);                                      // ack `st` for the frame in progress, then drop it
    uint16_t echoCrc = 0xFFFF;                                  // op 7 ECHO: running CRC16 of the payload
private:
    void beginFrame();
    void endFrame();
    void ack();
    size_t payload(const uint8_t* p, size_t n);
    void pushRow(const uint16_t* px);
};

void begin();                 // TCP server (+ counters)
void pump();                  // accept / read the TCP clients + UDP; call from loop()
bool tcpConnected();          // any client connected
uint8_t tcpClients();         // how many

// Counters for /status.stream and the shell.
struct Stats { uint32_t frames = 0, bytes = 0, shortFrames = 0, bad = 0, lastFrameMs = 0; float fps = 0; uint32_t kbps = 0;
               uint32_t superseded = 0, stalled = 0;                                   // ESP/1.1: frames dropped as stale · sockets closed mid-frame
               uint32_t feedUs = 0, loops = 0; uint8_t busyPct = 0; uint32_t loopsPerSec = 0; };   // CPU inside the parser vs wall time
const Stats& stats();
void countFrame(uint32_t bytes, bool ok);

// Optional shared secret for the TCP channel (NVS "token"); empty = open on the LAN.
void   setToken(const String& t);
String token();

}  // namespace Esp1
