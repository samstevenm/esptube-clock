#pragma once
// ============================================================================
// peers.h — mDNS peer discovery: "any clock can find any other clock".
//
// Browsing mDNS is a HARD 3 s synchronous block (ESPmDNS mdns_query_ptr) with a
// single shared result list, so it must never run on loop() (it would stall the
// clock face + REST) nor on the esp1 stream task. It runs on its own low-priority
// task; GET /peers and the shell `peers` command serve a cached JSON with an age
// field and ask the task to refresh when stale — the same pattern as the WiFi
// scan cache (net.cpp g_scan/scanJson). This node advertises itself as
// _esptube._tcp with TXT id/name/model/fw/... (see main.cpp startMDNS).
// ============================================================================
#include <Arduino.h>

namespace Peers {

void   begin();                              // start the browse task — call once, AFTER MDNS.begin()
String json(uint32_t maxAgeMs = 15000);      // {"peers":[...],"age_s":N}; triggers a refresh if older than maxAgeMs

}  // namespace Peers
