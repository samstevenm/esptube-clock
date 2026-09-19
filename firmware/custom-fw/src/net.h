// net.h — WiFi that roams and recovers.
//
//   * Saved networks (up to MAX_NETS, NVS "net"): the clock scans, picks the strongest one it
//     knows and joins it. Add a network anywhere — the on-device page, the helper, the USB shell
//     (`wifi add <ssid> <pass>`) — and it is remembered. The credentials WiFiManager stored on
//     first setup are imported on the first boot of this firmware, so nothing is lost.
//   * Recovery AP: when nothing known is in range (a reboot away from home, an outage, a new
//     router) the clock brings up its own network `esptube-setup` (WPA2, default passphrase
//     `esptube-setup`, 192.168.4.1, captive portal) so a phone can teach it the new WiFi. It
//     keeps looking for known networks in the background and drops the AP ~90 s after it is
//     back on one. The menu item AP forces it at any time.
//   * The glass is not held hostage: the AP announces itself ONCE (a bounded nixie scroll,
//     skipped while streamed content is on the tubes) and then the clock/face/scene carries on.
//     Every basic mode works with no WiFi at all (time comes from the DS1302 RTC, see rtc.h).
#pragma once
#include <Arduino.h>
#include <IPAddress.h>
#include <Print.h>

namespace Net {

static const int MAX_NETS = 8;
struct Entry { String ssid; String pass; };

void begin();                                   // load NVS (+ import legacy creds), start looking
void pump();                                    // state machine + captive DNS; call from loop()

bool        connected();                        // STA has an IP
String      ssid();                             // STA network name ("" when not connected)
IPAddress   ip();                               // STA IP (0.0.0.0 when not connected)
const char* stateName();                        // idle|scanning|connecting|connected|lost|waiting

bool        apActive();
const char* apSsid();                           // "esptube-setup"
String      apPass();
IPAddress   apIp();                             // 192.168.4.1
int         apClients();
void        startAp(bool announce);             // recovery AP now (AP+STA; keeps trying known networks)
void        stopAp();
void        setApPass(const String& pass);      // >= 8 chars; "" = open network (not recommended)
int         warnDbm();                          // weak-signal warning: scroll "WIFI WEAK -82 DBM" on the clock face when the smoothed RSSI sits below this (0 = off)
void        setWarnDbm(int dbm);                // -95..-30 or 0; persisted (NVS net/warn); never touches pushed/streamed content
int         rssiAvg();                          // the smoothed RSSI (~20 s EMA), 0 until connected

int          count();                           // saved networks
const Entry& entry(int i);
bool         known(const String& ssid);
bool         add(const String& ssid, const String& pass);   // save (or update) and join it now
bool         forget(const String& ssid);
bool         join(const String& ssid);          // a saved network: connect to it now
void         reconnect();                       // rescan and join the strongest known network

int    scanSync();                              // blocking scan (~2-4 s) -> number of networks
String scanJson();                              // last scan: [{"ssid","rssi","ch","enc","known"}]
String statusJson();                            // everything the UIs need (no passwords)
void   printStatus(Print& o);                   // shell `wifi`

}  // namespace Net
