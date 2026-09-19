// rtc.cpp — DS1302 over three GPIOs. Protocol: CE high, command byte LSB-first on IO clocked by
// SCLK rising edges; for reads the chip drives IO after the command and each bit is sampled
// before the next clock pulse. Burst read 0xBF / burst write 0xBE cover the 7 time registers +
// write-protect in one transfer. Times are kept in UTC; the TZ env does local time.
#include "rtc.h"
#include <sys/time.h>

namespace Rtc {
namespace {

int g_ce = -1, g_io = -1, g_sclk = -1;
bool g_present = false;
uint8_t g_raw[8];

inline void clkPulse() { digitalWrite(g_sclk, HIGH); delayMicroseconds(2); digitalWrite(g_sclk, LOW); delayMicroseconds(2); }

void writeByte(uint8_t v) {
    pinMode(g_io, OUTPUT);
    for (int i = 0; i < 8; ++i) { digitalWrite(g_io, (v >> i) & 1); delayMicroseconds(1); clkPulse(); }
}
uint8_t readByte() {
    pinMode(g_io, INPUT);
    uint8_t v = 0;
    for (int i = 0; i < 8; ++i) { if (digitalRead(g_io)) v |= (1 << i); clkPulse(); }
    return v;
}
void start() { digitalWrite(g_sclk, LOW); digitalWrite(g_ce, HIGH); delayMicroseconds(4); }
void stop()  { digitalWrite(g_ce, LOW); delayMicroseconds(4); }

uint8_t bcd2bin(uint8_t b) { return (uint8_t)(((b >> 4) & 0x0F) * 10 + (b & 0x0F)); }
uint8_t bin2bcd(uint8_t v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

void burstRead() {
    start(); writeByte(0xBF);
    for (int i = 0; i < 8; ++i) g_raw[i] = readByte();
    stop();
}
void burstWrite(const uint8_t* r) {
    start(); writeByte(0x8E); writeByte(0x00); stop();     // write-protect off
    start(); writeByte(0xBE);
    for (int i = 0; i < 8; ++i) writeByte(r[i]);
    stop();
}

bool decode(const uint8_t* r, struct tm& t) {
    if (r[0] & 0x80) return false;                         // CH: clock halted (never set)
    uint8_t sec = bcd2bin(r[0] & 0x7F), min = bcd2bin(r[1] & 0x7F);
    uint8_t hour = (r[2] & 0x80) ? (uint8_t)(bcd2bin(r[2] & 0x1F) % 12 + ((r[2] & 0x20) ? 12 : 0)) : bcd2bin(r[2] & 0x3F);
    uint8_t date = bcd2bin(r[3] & 0x3F), mon = bcd2bin(r[4] & 0x1F), year = bcd2bin(r[6]);
    if (sec > 59 || min > 59 || hour > 23 || date < 1 || date > 31 || mon < 1 || mon > 12) return false;
    if (year < 24 || year > 89) return false;              // 2024..2089: anything else is an unset chip
    t = {}; t.tm_sec = sec; t.tm_min = min; t.tm_hour = hour; t.tm_mday = date; t.tm_mon = mon - 1; t.tm_year = 100 + year;
    return true;
}

}  // namespace

void begin(int ce, int io, int sclk) {
    g_ce = ce; g_io = io; g_sclk = sclk;
    pinMode(g_ce, OUTPUT); digitalWrite(g_ce, LOW);
    pinMode(g_sclk, OUTPUT); digitalWrite(g_sclk, LOW);
    pinMode(g_io, INPUT);
    struct tm t; g_present = read(t);
    Serial.printf("[rtc] DS1302 ce=%d io=%d sclk=%d: %s\n", ce, io, sclk, describe().c_str());
}

bool present() { return g_present; }
uint8_t* rawRegs() { return g_raw; }

bool read(struct tm& utc) {
    if (g_ce < 0) return false;
    burstRead();
    bool allFF = true, all00 = true;
    for (int i = 0; i < 7; ++i) { if (g_raw[i] != 0xFF) allFF = false; if (g_raw[i] != 0x00) all00 = false; }
    if (allFF || all00) return false;                      // nothing wired / floating bus
    return decode(g_raw, utc);
}

bool write(const struct tm& utc) {
    if (g_ce < 0) return false;
    uint8_t r[8] = { bin2bcd((uint8_t)utc.tm_sec), bin2bcd((uint8_t)utc.tm_min), bin2bcd((uint8_t)utc.tm_hour),
                     bin2bcd((uint8_t)utc.tm_mday), bin2bcd((uint8_t)(utc.tm_mon + 1)), bin2bcd((uint8_t)(utc.tm_wday == 0 ? 7 : utc.tm_wday)),
                     bin2bcd((uint8_t)(utc.tm_year % 100)), 0x80 };
    burstWrite(r);
    struct tm back; bool ok = read(back) && back.tm_min == utc.tm_min && back.tm_hour == utc.tm_hour;
    g_present = g_present || ok;
    return ok;
}

// civil UTC -> epoch without touching TZ (mktime would apply the local zone)
static time_t tmToUtc(const struct tm& t) {
    int y = t.tm_year + 1900, m = t.tm_mon + 1, d = t.tm_mday; y -= m <= 2;
    int era = (y >= 0 ? y : y - 399) / 400; unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1; unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long days = (long)era * 146097 + (long)doe - 719468;
    return (time_t)days * 86400 + t.tm_hour * 3600 + t.tm_min * 60 + t.tm_sec;
}

bool seedSystemTime() {
    struct tm t; if (!read(t)) return false;
    struct timeval tv = { tmToUtc(t), 0 }; settimeofday(&tv, nullptr);
    Serial.printf("[rtc] system time seeded from the chip: %04d-%02d-%02d %02d:%02d:%02d UTC\n",
                  t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
    return true;
}

bool saveSystemTime() {
    time_t now = time(nullptr); if (now < 1700000000) return false;   // not a real time yet
    struct tm g; gmtime_r(&now, &g);
    bool ok = write(g);
    Serial.printf("[rtc] chip <- %04d-%02d-%02d %02d:%02d:%02d UTC %s\n", g.tm_year + 1900, g.tm_mon + 1, g.tm_mday, g.tm_hour, g.tm_min, g.tm_sec, ok ? "ok" : "FAILED");
    return ok;
}

String describe() {
    struct tm t; char b[96];
    if (read(t)) { snprintf(b, sizeof b, "%04d-%02d-%02d %02d:%02d:%02d UTC (raw %02X %02X %02X %02X %02X %02X %02X)", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec, g_raw[0], g_raw[1], g_raw[2], g_raw[3], g_raw[4], g_raw[5], g_raw[6]); }
    else { snprintf(b, sizeof b, "no valid time (raw %02X %02X %02X %02X %02X %02X %02X %02X)", g_raw[0], g_raw[1], g_raw[2], g_raw[3], g_raw[4], g_raw[5], g_raw[6], g_raw[7]); }
    return String(b);
}

}  // namespace Rtc
