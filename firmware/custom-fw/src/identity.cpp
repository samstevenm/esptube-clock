// identity.cpp — see identity.h for the contract.
#include "identity.h"
#include "config.h"
#include <esp_system.h>
#include <esp_mac.h>
#include <Preferences.h>

namespace Id {
namespace {
char g_mac[13]  = "000000000000";
char g_mac3[7]  = "000000";
char g_host[33] = HOSTNAME;
char g_ap[40]   = "esptube-setup";
}

void begin() {
    uint8_t m[6] = {0};
    esp_read_mac(m, ESP_MAC_WIFI_STA);   // efuse MAC — valid before WiFi is up
    snprintf(g_mac,  sizeof g_mac,  "%02x%02x%02x%02x%02x%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
    snprintf(g_mac3, sizeof g_mac3, "%02x%02x%02x", m[3], m[4], m[5]);
    snprintf(g_ap,   sizeof g_ap,   "esptube-setup-%s", g_mac3);
    String n; { Preferences p; if (p.begin("esptube", true)) { n = p.getString("name", ""); p.end(); } }
    snprintf(g_host, sizeof g_host, "%s", n.length() ? n.c_str() : HOSTNAME);
}
const char* mac()      { return g_mac; }
const char* mac3()     { return g_mac3; }
const char* hostname() { return g_host; }
const char* apSsid()   { return g_ap; }
const char* chip()     { return ESP.getChipModel(); }
const char* model()    { return "sihai-6"; }

bool setName(const String& n) {
    if (n.length() > 24) return false;
    for (size_t i = 0; i < n.length(); ++i) { char c = n[i]; if (!(isalnum((int)c) || c == '-')) return false; }
    Preferences p; if (p.begin("esptube", false)) { if (n.length()) p.putString("name", n); else p.remove("name"); p.end(); }
    snprintf(g_host, sizeof g_host, "%s", n.length() ? n.c_str() : HOSTNAME);
    return true;
}

}  // namespace Id
