#pragma once
// ============================================================================
// shell.h — text command shell on the USB UART + binary ESP/1 frames on the
// same port. Physical access is the security model. See docs/ESP1-PROTOCOL.md.
// ============================================================================
#include <Arduino.h>

namespace Shell {
void begin();
void pump();                       // loop(): run queued command lines (UART + WebSocket text) + the rate watchdog
void rxPump();                     // esp1 task: drain the UART in bulk -> text lines | legacy frames | COBS datagrams
// Command lines are EXECUTED only in loop() (exec touches Control/NVS/String state that loop() owns).
// Other tasks post them here; slot >= 0 = a WebSocket client that wants the reply as a text frame.
bool post(const char* line, size_t n, int8_t slot);
bool takeReply(int8_t& slot, String& reply);   // esp1 task: a finished WebSocket command's reply, if any
void linkProven();                 // a CRC-clean datagram arrived: this baud rate works (feeds the rate watchdog)
String uartJson();                 // {"baud","dg","crc_bad","resync","junk","overrun","fifo"} for /status
// Run one command line (also used by the on-device menu); reply goes to `out`.
void exec(const String& line, Print& out);
uint32_t bootBaud();                 // 115200, or the rate armed by `baud N` for exactly one reset
void     armBootBaud(uint32_t baud); // arm it (RTC memory; cleared when consumed or on power loss)
}
