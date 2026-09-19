// rtc.h — DS1302 real-time clock (3-wire, bit-banged), so the clock keeps time with no WiFi.
//   Boot: if the chip holds a sane time, seed the system clock from it (the nixie face is right
//   immediately, no NTP needed). Whenever NTP syncs, write the corrected time back. Stored as UTC.
#pragma once
#include <Arduino.h>
#include <time.h>

namespace Rtc {

void begin(int ce, int io, int sclk);
bool present();                       // a chip answered with a plausible time at begin()
bool read(struct tm& utc);            // false if the chip is halted / unset / absent
bool write(const struct tm& utc);     // also clears the clock-halt bit
bool seedSystemTime();                // settimeofday() from the chip; true if it did
bool saveSystemTime();                // chip <- system time (call after an NTP sync)
String describe();                    // one line for the shell / status
uint8_t* rawRegs();                   // last 8 raw burst bytes (debugging the pin map)

}  // namespace Rtc
