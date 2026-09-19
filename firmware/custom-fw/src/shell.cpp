// ============================================================================
// shell.cpp — UART command shell + binary frame pass-through.
//   Lines ending in '\n' that don't start with 0xE5 are commands; a 0xE5 at the
//   start of a line begins an ESP/1 frame, which is handed to a Esp1::Parser
//   whose acks go back out the UART.
// ============================================================================
#include "shell.h"
#include "net.h"
#include "build_stamp.h"
#include "rtc.h"
#include <sys/time.h>
#include <Preferences.h>
#include "config.h"
#include "tubes.h"
#include "leds.h"
#include "control.h"
#include "esp1.h"
#include "esp1_frame.h"
#include "api.h"
#include "identity.h"
#include "peers.h"
#include <WiFi.h>

namespace {

Esp1::Parser g_ser;
Esp1Frame::UartFramer g_fr;     // one byte stream: text lines | legacy raw frames | COBS datagrams
volatile bool g_dgReply = false, g_shellReady = false;
uint32_t g_lastRxMs = 0; uint8_t g_fifoThr = 0;
volatile uint32_t g_uartOverrun = 0;

// A datagram is answered with a datagram: 00 <cobs(reply + crc16)> 00 in ONE write, so it is atomic
// against log lines printed by the other task and the host never has to fish A5 out of shell text.
void writeFramed(const uint8_t* d, size_t n) {
    uint8_t raw[16], enc[22]; if (n > 12) n = 12; memcpy(raw, d, n);
    const uint16_t c = Esp1Frame::crc16(raw, n); raw[n] = (uint8_t)(c >> 8); raw[n + 1] = (uint8_t)c;
    enc[0] = 0; const size_t m = Esp1Frame::cobsEncode(raw, n + 2, enc + 1); enc[1 + m] = 0;
    Serial.write(enc, m + 2);
}
void serialSink(void*, const uint8_t* d, size_t n) { if (g_dgReply) writeFramed(d, n); else Serial.write(d, n); }

struct StrPrint : public Print { String s; size_t write(uint8_t b) override { s += (char)b; return 1; } size_t write(const uint8_t* b, size_t n) override { for (size_t i = 0; i < n; ++i) s += (char)b[i]; return n; } };
struct Cmd { char line[200]; int8_t slot = -1; volatile uint8_t state = 0; uint32_t order = 0; String reply; };   // 0 free · 1 queued · 2 running · 3 reply ready
Cmd g_cmds[3]; uint32_t g_cmdOrder = 0;

void frLine(void*, const char* l, size_t n) { if (!Shell::post(l, n, -1)) Serial.println("ERR busy (command queue full)"); }
void frDatagram(void*, const uint8_t* f, size_t n) { Shell::linkProven(); g_ser.abortFrame(); g_dgReply = true; g_ser.feed(f, n); g_dgReply = false; }
size_t frLegacy(void*, const uint8_t* p, size_t n, bool* done) { bool d = false; const size_t u = g_ser.feedFrame(p, n, d); *done = d; return u; }
void frBadLine(void*, size_t n) { Serial.printf("ERR unreadable command (%u bytes with control characters) - type it again\n", (unsigned)n); }
void frBad(void*, uint8_t st, uint8_t seq) { const uint8_t a[4] = { 0xA5, seq, st, Tubes::epoch() }; writeFramed(a, 4); }

// first token of `s` (quoted with "..." if it has spaces) -> tok; rest -> s
static bool takeTok(String& s, String& tok) {
    s.trim(); if (!s.length()) return false;
    if (s[0] == '"') { int e = s.indexOf('"', 1); if (e < 0) return false; tok = s.substring(1, e); s = s.substring(e + 1); }
    else { int e = s.indexOf(' '); tok = e < 0 ? s : s.substring(0, e); s = e < 0 ? String() : s.substring(e + 1); }
    s.trim(); return true;
}
static void wifiCmd(String arg, Print& o) {
    String sub; if (!takeTok(arg, sub)) { Net::printStatus(o); return; }
    sub.toLowerCase();
    if (sub == "json") { o.println(Net::statusJson()); }
    else if (sub == "list") { o.printf("saved (%d):\n", Net::count()); for (int i = 0; i < Net::count(); ++i) o.printf("  %d. %s\n", i + 1, Net::entry(i).ssid.c_str()); }
    else if (sub == "scan") { int n = Net::scanSync(); o.printf("%d network(s):\n", n);
        for (int i = 0; i < n; ++i) o.printf("  %-32s %4d dBm ch%-2d %s%s\n", WiFi.SSID(i).c_str(), (int)WiFi.RSSI(i), WiFi.channel(i), WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "open" : "wpa", Net::known(WiFi.SSID(i)) ? "  (saved)" : ""); }
    else if (sub == "add") { String ssid, pass; if (!takeTok(arg, ssid)) { o.println("ERR usage: wifi add <ssid> <pass>"); return; } takeTok(arg, pass);
        o.println(Net::add(ssid, pass) ? "OK saved — joining" : "ERR bad ssid/pass"); }
    else if (sub == "forget") { String ssid; takeTok(arg, ssid); o.println(Net::forget(ssid) ? "OK forgotten" : "ERR not saved"); }
    else if (sub == "join") { String ssid; takeTok(arg, ssid); o.println(Net::join(ssid) ? "OK joining" : "ERR not saved (wifi add first)"); }
    else if (sub == "ap") { String on; takeTok(arg, on); on.toLowerCase();
        if (on == "off") { Net::stopAp(); o.println("OK ap off"); } else { Net::startAp(true); o.printf("OK ap \"%s\" pass \"%s\" at 192.168.4.1\n", Net::apSsid(), Net::apPass().c_str()); } }
    else if (sub == "appass") { String p; takeTok(arg, p); if (p.length() && p.length() < 8) { o.println("ERR 8+ chars (or empty for open)"); return; } Net::setApPass(p); o.println("OK ap password set"); }
    else if (sub == "reconnect") { Net::reconnect(); o.println("OK rescanning"); }
    else if (sub == "warn") {   // wifi warn [-95..-30|off]: the weak-signal nixie warning threshold
        String v; takeTok(arg, v); v.toLowerCase();
        if (v == "off" || v == "0") Net::setWarnDbm(0); else if (v.length()) { long d = v.toInt(); if (d > -30 || d < -95) { o.println("ERR wifi warn <-95..-30>|off"); return; } Net::setWarnDbm((int)d); }
        if (Net::warnDbm()) o.printf("weak-signal warning below %d dBm (rssi now %d, avg %d)\n", Net::warnDbm(), (int)WiFi.RSSI(), Net::rssiAvg()); else o.printf("weak-signal warning off (rssi now %d)\n", (int)WiFi.RSSI()); }
    else o.println("ERR wifi: scan | list | add | forget | join | ap [on|off] | appass | reconnect | warn [dbm|off]");
}

void help(Print& o) {
    o.println("commands: help status preset <n|next> nixie <text> flash <text> scroll <text> countdown <s> stop clear");
    o.println("          text <tube> <words> rgb <tube> r g b idle [min]");
    o.println("          led <off|solid|rainbow|breathe|comet> bright <0-255> mode <clock|off|manual>");
    o.println("          sweep [from to fade] fps baud <n> token <x|clear> admin <x|clear> name <x|clear> peers time [epoch] rtc [set] reboot");
    o.println("wifi:     wifi | wifi json | wifi scan | wifi list | wifi add <ssid> <pass> | wifi forget <ssid> | wifi join <ssid>");
    o.println("          wifi ap [on|off] (recovery AP esptube-setup @192.168.4.1) | wifi appass <pass> | wifi reconnect | wifi warn [dbm|off]");
    o.println("          (quote an ssid with spaces: wifi add \"My Net\" secret)");
    o.println("          uart [fifo <n>] (link counters / RX FIFO threshold)");
    o.println("binary:   an ESP/1 frame (magic E5 7B) may start at any line; acks A5 <seq> <status> <epoch|00>");
    o.println("          or as a datagram: 00 <COBS(frame + crc16)> 00 - self-delimiting, CRC-checked, acked the same way");
}

}  // namespace

// one-shot boot rate: survives a reset (DTR / software), not a power cycle
RTC_NOINIT_ATTR static uint32_t g_bootBaudReq;
RTC_NOINIT_ATTR static uint32_t g_bootBaudMagic;
static const uint32_t BOOT_BAUD_MAGIC = 0xB0D0B0D1u;

namespace Shell {

// Rate watchdog: a switch the other side can't actually follow (this USB chip garbles 921600) must
// never strand the shell. After any switch, if no CLEAN command line arrives within the grace period
// (8 s in-session, 20 s after a one-shot boot) the UART goes back to the previous rate by itself.
static volatile uint32_t g_rateSince = 0; static uint32_t g_prevRate = 115200, g_rateGrace = 8000; static volatile bool g_cmdSinceRate = false;   // (linkProven() writes these from the esp1 task)
static void rateSwitched(uint32_t from, uint32_t grace) { g_prevRate = from; g_rateSince = millis() ? millis() : 1; g_rateGrace = grace; g_cmdSinceRate = false; }

uint32_t bootBaud() {
    uint32_t b = 115200;
    if (g_bootBaudMagic == BOOT_BAUD_MAGIC && g_bootBaudReq >= 9600 && g_bootBaudReq <= 3000000) b = g_bootBaudReq;
    g_bootBaudMagic = 0;                                   // consumed: the next reset is 115200 again
    if (b != 115200) rateSwitched(115200, 20000);          // one-shot boot: prove yourself within 20 s
    return b;
}
void armBootBaud(uint32_t baud) { g_bootBaudReq = baud; g_bootBaudMagic = BOOT_BAUD_MAGIC; }

void begin() {
    g_ser.sink = serialSink; g_ser.reset(); g_ser.authed = true; g_ser.st = 0;   // UART = physical access, no token
    g_fr.onLine = frLine; g_fr.onDatagram = frDatagram; g_fr.onLegacy = frLegacy; g_fr.onBad = frBad; g_fr.onBadLine = frBadLine; g_fr.reset();
    // RX FIFO-full interrupt at 32 bytes instead of the core's 120 (of a 128-byte FIFO): at 1-2 Mbaud 8 bytes of
    // margin is 40-80 us and the FIFO overran (measured: 1 lost datagram per ~70 KB at 1 M; zero at 32, up to 2 M).
    Serial.setRxFIFOFull(32); g_fifoThr = 32;
    Serial.onReceiveError([](hardwareSerial_error_t e) { if (e == UART_FIFO_OVF_ERROR || e == UART_BUFFER_FULL_ERROR) g_uartOverrun = g_uartOverrun + 1; });
    g_shellReady = true;
}
void linkProven() { g_cmdSinceRate = true; g_rateSince = 0; }

bool post(const char* line, size_t n, int8_t slot) {
    for (auto& c : g_cmds) if (c.state == 0) {
        if (n > sizeof(c.line) - 1) n = sizeof(c.line) - 1;
        memcpy(c.line, line, n); c.line[n] = 0; c.slot = slot; c.order = ++g_cmdOrder; c.state = 1; return true;
    }
    return false;
}
bool takeReply(int8_t& slot, String& reply) {
    for (auto& c : g_cmds) if (c.state == 3) { slot = c.slot; reply = c.reply; c.reply = ""; c.state = 0; return true; }
    return false;
}
String uartJson() {
    String j = "{\"baud\":"; j += (uint32_t)Serial.baudRate(); j += ",\"dg\":"; j += g_fr.datagrams; j += ",\"crc_bad\":"; j += g_fr.crcBad;
    j += ",\"resync\":"; j += g_fr.resync; j += ",\"junk\":"; j += g_fr.junkLines; j += ",\"overrun\":"; j += (uint32_t)g_uartOverrun; j += ",\"fifo\":"; j += (uint32_t)g_fifoThr; j += "}";
    return j;
}

// esp1 task (every ~1 ms): the UART is drained HERE, in bulk, so a loop() stall (a nixie fade, an
// HTTP body, an NVS write) can no longer overflow the ring mid-frame, and at 1-2 Mbaud the hardware
// FIFO is emptied promptly. Text lines are posted to loop(); pixels are drawn from this task like
// every other ESP/1 transport.
void rxPump() {
    if (!g_shellReady) return;
    static uint8_t buf[512];
    int budget = 16;                                       // <= ~8 KB per pass; the sockets need turns too
    while (budget-- && Serial.available()) {
        const int n = Serial.read(buf, sizeof buf);
        if (n <= 0) break;
        g_lastRxMs = millis();
        g_fr.feed(buf, (size_t)n);
    }
    // A sender that stopped mid-frame must not leave the shell swallowed: UART only (a socket is closed instead).
    if (g_fr.midFrame() && !Serial.available() && (uint32_t)(millis() - g_lastRxMs) > 300) {
        if (g_fr.mode == Esp1Frame::UartFramer::LEGACY) g_ser.fail(Esp1Frame::ST_BAD_LEN);
        g_fr.idleAbort();
    }
}

void exec(const String& raw, Print& o) {
    String line = raw; line.trim();
    if (!line.length()) return;
    { bool clean = true; for (size_t i = 0; i < line.length(); ++i) { uint8_t c = (uint8_t)line[i]; if (c < 0x20 || c > 0x7E) { clean = false; break; } }
      if (clean) { g_cmdSinceRate = true; g_rateSince = 0; } }   // a readable line at this rate: the link works
    int sp = line.indexOf(' ');
    String cmd = sp < 0 ? line : line.substring(0, sp), arg = sp < 0 ? String() : line.substring(sp + 1);
    cmd.toLowerCase(); arg.trim();

    if (cmd == "help") help(o);
    else if (cmd == "status") {
        const Esp1::Stats& s = Esp1::stats();
        time_t now = time(nullptr);
        o.printf("{\"mode\":\"%s\",\"preset\":\"%s\",\"preset_index\":%u,\"led\":\"%s\",\"bright\":%u,\"ip\":\"%s\",\"wifi\":%s,\"ssid\":\"%s\",\"ap\":%s,"
                 "\"populated_mask\":%u,\"time_valid\":%s,\"epoch\":%lu,\"rtc\":%s,\"heap\":%u,\"uptime_s\":%u,\"build\":\"" BUILD_STAMP "\","
                 "\"stream\":{\"tcp\":%s,\"fps\":%.1f,\"kbps\":%u,\"frames\":%u,\"bad\":%u}}\n",
                 Control::modeName(), Control::presetName(Control::preset()), (unsigned)Control::preset(), Control::effectName(), Control::brightness(),
                 WiFi.localIP().toString().c_str(), Net::connected() ? "true" : "false", Net::ssid().c_str(), Net::apActive() ? "true" : "false",
                 (unsigned)Tubes::populatedMask(), Control::timeValid() ? "true" : "false", (unsigned long)now, Rtc::present() ? "true" : "false",
                 (unsigned)ESP.getFreeHeap(), (unsigned)(millis() / 1000),
                 Esp1::tcpConnected() ? "true" : "false", s.fps, (unsigned)s.kbps, (unsigned)s.frames, (unsigned)s.bad);
    }
    else if (cmd == "preset") { if (arg == "next") Control::nextPreset(); else if (!Control::setPresetByIndex(arg.toInt())) { o.println("ERR bad preset"); return; } o.printf("OK preset %s\n", Control::presetName(Control::preset())); }
    else if (cmd == "nixie" || cmd == "flash" || cmd == "scroll") {
        const char* eff = cmd == "nixie" ? "static" : cmd.c_str();
        if (!arg.length() || !Control::nixieText(arg.c_str(), eff, 400)) { o.println("ERR usage: nixie|flash|scroll <text>"); return; }
        o.printf("OK %s\n", eff);
    }
    else if (cmd == "countdown") { long s = arg.toInt(); if (s <= 0) { o.println("ERR seconds"); return; } Control::nixieCountdown((uint32_t)s); o.println("OK countdown"); }
    else if (cmd == "clear") { Control::masterClear(); o.println("OK cleared all"); }
    else if (cmd == "rgb") {    // rgb <tube> <r> <g> <b>: one tube's underglow (LED layer only)
        int t = -1, r = 0, g = 0, b = 0; if (sscanf(arg.c_str(), "%d %d %d %d", &t, &r, &g, &b) != 4 || t < 0 || t >= TUBE_COUNT) { o.println("ERR usage: rgb <tube 0-5> <r> <g> <b>"); return; }
        Leds::setTube((uint8_t)t, (uint8_t)constrain(r, 0, 255), (uint8_t)constrain(g, 0, 255), (uint8_t)constrain(b, 0, 255)); Leds::show(); o.printf("OK rgb %d\n", t); }
    else if (cmd == "text") {   // text <tube> <words...>: built-in font on one tube
        int sp2 = arg.indexOf(' '); int t = arg.substring(0, sp2 < 0 ? arg.length() : sp2).toInt(); String txt = sp2 < 0 ? String() : arg.substring(sp2 + 1);
        if (t < 0 || t >= TUBE_COUNT) { o.println("ERR usage: text <tube 0-5> <text>"); return; }
        Control::notifyManualContent(); Tubes::drawText((uint8_t)t, txt); o.printf("OK text %d\n", t); }
    else if (cmd == "idle") {   // idle [minutes]: MANUAL -> clock after this long without content (0 = never)
        if (arg.length()) { long m = arg.toInt(); if (m < 0 || m > 1440) { o.println("ERR idle 0..1440"); return; } Control::setIdleMinutes((uint16_t)m); }
        o.printf("idle return: %u min%s\n", Control::idleMinutes(), Control::idleMinutes() ? "" : " (never)"); }
    else if (cmd == "stop") { Control::nixieStop(); Control::clockSweepStop(); o.println("OK clock"); }
    else if (cmd == "led") { if (!Control::setEffectByName(arg.c_str())) { o.println("ERR effect"); return; } o.printf("OK led %s\n", Control::effectName()); }
    else if (cmd == "bright") { long v = arg.toInt(); if (v < 0) v = 0; if (v > 255) v = 255; Control::setBrightness((uint8_t)v); o.printf("OK bright %ld\n", v); }
    else if (cmd == "mode") { if (!Control::setModeByName(arg.c_str())) { o.println("ERR mode"); return; } o.printf("OK mode %s\n", Control::modeName()); }
    else if (cmd == "wifi") wifiCmd(arg, o);
    else if (cmd == "time") {   // time [epoch]: set the system clock (and the RTC chip) — for clocks with no WiFi
        if (arg.length()) { unsigned long e = strtoul(arg.c_str(), nullptr, 10); if (e < 1700000000UL) { o.println("ERR epoch seconds (>= 1700000000)"); return; }
            struct timeval tv = { (time_t)e, 0 }; settimeofday(&tv, nullptr); Rtc::saveSystemTime(); Control::forceClockRedraw(); }
        time_t now = time(nullptr); struct tm lt; localtime_r(&now, &lt); char b[48]; strftime(b, sizeof b, "%Y-%m-%d %H:%M:%S %Z", &lt);
        o.printf("time: %s (epoch %lu, %s; rtc %s)\n", b, (unsigned long)now, Control::timeValid() ? "valid" : "NOT SET", Rtc::present() ? "ok" : "unset"); }
    else if (cmd == "rtc") { if (arg == "set") { o.println(Rtc::saveSystemTime() ? "OK rtc set from system time" : "ERR rtc write"); } o.println(Rtc::describe()); }
    else if (cmd == "sweep") {
        long from = 0, to = 86399, fade = 1;
        if (arg.length()) sscanf(arg.c_str(), "%ld %ld %ld", &from, &to, &fade);
        Control::clockSweepStart((uint32_t)from, (uint32_t)to, 0, fade != 0); o.printf("OK sweep %ld..%ld fade=%ld\n", from, to, fade);
    }
    else if (cmd == "uart") {   // uart [fifo <1-120>]: link counters; the RX FIFO-full threshold (lower = earlier interrupts = fewer overruns at 1-2 Mbaud)
        if (arg.startsWith("fifo")) { long v = arg.substring(4).toInt(); if (v < 1 || v > 120) { o.println("ERR uart fifo <1..120>"); return; } Serial.setRxFIFOFull((uint8_t)v); g_fifoThr = (uint8_t)v; }
        o.println(uartJson()); }
    else if (cmd == "fps") { const Esp1::Stats& s = Esp1::stats(); o.printf("fps=%.1f kbps=%u frames=%u bad=%u tcp=%d draw_ms=%u\n", s.fps, (unsigned)s.kbps, (unsigned)s.frames, (unsigned)s.bad, Esp1::tcpConnected(), (unsigned)(Tubes::lastDrawUs() / 1000)); }
    else if (cmd == "baud") {   // baud N [once]: switch now; `once` also brings the UART up at N after the next reset (one time)
        long b = arg.toInt(); bool once = arg.indexOf("once") > 0 || arg.indexOf("next") > 0;
        if (b < 9600 || b > 3000000) { o.println("ERR baud <9600..3000000> [once]"); return; }
        if (arg.indexOf("save") > 0) o.println("note: `save` is gone — nothing persists; use `baud N once` for the reopen trick");
        if (once) armBootBaud((uint32_t)b);
        o.printf("OK baud %ld%s (reverts by itself if nothing readable arrives within 8 s)\n", b, once ? " once-after-reset" : "");
        uint32_t from = Serial.baudRate(); Serial.flush(); Serial.updateBaudRate((uint32_t)b); rateSwitched(from, 8000); }
    else if (cmd == "token") { if (arg == "clear") arg = ""; Esp1::setToken(arg); o.printf("OK token %s\n", arg.length() ? "set" : "cleared"); }
    else if (cmd == "admin") { if (arg == "clear") arg = ""; Api::setAdminToken(arg); o.printf("OK admin %s\n", arg.length() ? "set (REST/OTA now require it)" : "cleared (REST/OTA open on the LAN)"); }
    else if (cmd == "name") { if (arg == "clear") arg = ""; if (!Id::setName(arg)) { o.println("ERR name: letters/digits/'-', <=24 (or clear)"); return; } o.printf("OK name -> %s.local (reboot to re-announce mDNS/DHCP)\n", Id::hostname()); }
    else if (cmd == "peers") { o.println(Peers::json()); }   // other _esptube._tcp clocks on the LAN (cached; refreshes on demand)
    else if (cmd == "reboot") { o.println("OK rebooting"); o.flush(); delay(100); ESP.restart(); }
    else o.printf("ERR unknown '%s' (help)\n", cmd.c_str());
}

void pump() {
    if (g_rateSince && !g_cmdSinceRate && (uint32_t)(millis() - g_rateSince) > g_rateGrace) {   // nobody could talk at this rate
        uint32_t bad = Serial.baudRate(); g_rateSince = 0; Serial.flush(); Serial.updateBaudRate(g_prevRate);
        Serial.printf("[shell] no readable command at %u baud — back to %u\n", (unsigned)bad, (unsigned)g_prevRate);
    }
    // Queued command lines, oldest first (the UART itself is read by rxPump() on the esp1 task).
    for (;;) {
        Cmd* next = nullptr;
        for (auto& c : g_cmds) if (c.state == 1 && (!next || c.order < next->order)) next = &c;
        if (!next) break;
        next->state = 2;
        if (next->slot < 0) { exec(String(next->line), Serial); next->state = 0; }
        else { StrPrint out; exec(String(next->line), out); next->reply = out.s; next->state = 3; }
    }
}

}  // namespace Shell
