#pragma once
// ============================================================================
// identity.h — per-device identity.
//
// The WiFi MAC is the device's true unique key (id). The mDNS/DHCP hostname
// stays HOSTNAME ("esptube") by DEFAULT so `esptube.local` keeps working for a
// single clock and existing tooling doesn't break; with several clocks give
// each one a unique name (NVS "esptube"/"name", set over the shell `name <x>`
// or `Id::setName`) which then becomes <name>.local. The recovery-AP SSID is always
// made unique ("esptube-setup-<mac3>") so two clocks in recovery are tellable
// apart. Discovery (mDNS TXT / GET /peers) always carries the MAC id + model.
// ============================================================================
#include <Arduino.h>

namespace Id {

void begin();               // read the MAC, load the NVS name override — call once, early in setup()
const char* mac();          // 12 lowercase hex, no separators: the stable device id
const char* mac3();         // last 3 MAC bytes ("463eb0"): the short unique suffix
const char* hostname();     // runtime hostname: the NVS name if set, else HOSTNAME ("esptube")
const char* apSsid();       // recovery-AP SSID, unique per device: "esptube-setup-<mac3>"
const char* chip();         // ESP.getChipModel(), e.g. "ESP32-D0WD"
const char* model();        // hardware model for discovery/layout: "sihai-6"
bool setName(const String& n);   // persist a DNS-safe name override ("" resets to the default)

}  // namespace Id
