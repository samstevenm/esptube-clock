#pragma once
// ============================================================================
// api.h — REST routing for esptube-fw (built-in WebServer, JSON responses).
// ============================================================================
#include <WebServer.h>

namespace Api {

// Register all routes on the given server. The server itself is started by
// the caller (main.cpp) after begin() returns.
void begin(WebServer& server);

// Service background work owned by the API layer: performs the deferred reboot
// after a successful /ota upload. Call once per loop().
void pump();

// The opt-in admin token (empty = none). Gates every mutating REST route once
// set; main.cpp adopts it as the ArduinoOTA password so port 3232 is not a
// second unauthenticated reflash path.
String adminToken();
void   setAdminToken(const String& t);   // set/clear from the UART shell (physical access)

}  // namespace Api
