// ============================================================================
// main.cpp — esptube-fw entry point.
//   displays (boot self-test) -> LEDs -> LittleFS -> Net (roaming WiFi, NON-BLOCKING)
//   -> mDNS -> REST server -> ArduinoOTA. The REST server, mDNS and OTA start
//   regardless of WiFi state, so they are reachable immediately on the clock's
//   own `esptube-setup` recovery AP (192.168.4.1) and on any saved WiFi.
//   loop() services Net, the web server, OTA, and API work.
// ============================================================================
#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include <LittleFS.h>
#include <WebServer.h>
#include <Preferences.h>

#include "config.h"
#include "identity.h"        // per-device id (MAC), runtime hostname, unique recovery-AP SSID
#if __has_include("secrets.h")
#include "secrets.h"          // optional, UNUSED fallback (WiFi is NVS/captive-portal provisioned).
#endif                        //   Guarded so a clean clone (no secrets.h) still compiles — see secrets.h.example.
#include "tubes.h"
#include "leds.h"
#include "buttons.h"
#include "control.h"
#include "api.h"
#include "esp1.h"           // ESP/1 binary frame channel (TCP :5555)
#include "shell.h"            // UART command shell (+ ESP/1 frames over USB)
#include "net.h"              // roaming WiFi + recovery AP (non-blocking; no password compiled in)
#include "peers.h"            // mDNS peer discovery (background browse task)
#include "build_stamp.h"      // FW_VERSION (advertised in the mDNS TXT)

static WebServer server(80);

static void startMDNS() {
    if (MDNS.begin(Id::hostname())) {
        MDNS.addService("http", "tcp", 80);
        // _esptube._tcp: the discovery service other clocks / the helper browse for. TXT carries the
        // MAC id (the stable unique id), friendly name, hardware model, firmware version, geometry
        // and ports — so a browse answers "who's here + what are they" without a follow-up HTTP hit.
        MDNS.addService("esptube", "tcp", 80);
        MDNS.addServiceTxt("esptube", "tcp", "id",    Id::mac());
        MDNS.addServiceTxt("esptube", "tcp", "name",  Id::hostname());
        MDNS.addServiceTxt("esptube", "tcp", "model", Id::model());
        MDNS.addServiceTxt("esptube", "tcp", "fw",    FW_VERSION);
        MDNS.addServiceTxt("esptube", "tcp", "tubes", String(TUBE_COUNT).c_str());
        MDNS.addServiceTxt("esptube", "tcp", "w",     String(PANEL_W).c_str());
        MDNS.addServiceTxt("esptube", "tcp", "h",     String(PANEL_H).c_str());
        MDNS.addServiceTxt("esptube", "tcp", "proto", "1.1");
        MDNS.addServiceTxt("esptube", "tcp", "stream", String(Esp1::PORT).c_str());
        MDNS.addServiceTxt("esptube", "tcp", "ws",     String(Esp1::WS_PORT).c_str());
        Serial.printf("[mdns] http://%s.local/  (+ _esptube._tcp id=%s)\n", Id::hostname(), Id::mac());
    } else {
        Serial.println("[mdns] failed to start");
    }
}

static void startArduinoOTA() {
    ArduinoOTA.setHostname(Id::hostname());
    // When the REST admin token is set, port 3232 requires it too — otherwise a
    // native OTA client would be a second unauthenticated reflash path, and the
    // _arduino._tcp advert would keep announcing auth_upload=no. Set = auth on.
    String adm = Api::adminToken();
    if (adm.length()) ArduinoOTA.setPassword(adm.c_str());
    ArduinoOTA.onStart([]() { Serial.println("[arduinoota] start"); });
    ArduinoOTA.onEnd([]()   { Serial.println("[arduinoota] end"); });
    ArduinoOTA.onError([](ota_error_t e) { Serial.printf("[arduinoota] error %u\n", e); });
    ArduinoOTA.begin();
}

void setup() {
    // ESP/1 frames arrive over this UART too: the default 256-byte RX buffer drops
    // bytes whenever loop() stalls for >20 ms at 115200 (a nixie fade, an HTTP body),
    // and a dropped byte kills the frame. 16 KB gives ~170 ms of slack at 921600.
    Serial.setRxBufferSize(16384);
    // Boot rate: 115200, unless the shell's `baud N` armed a ONE-SHOT for this reset (RTC memory —
    // a Web Serial session must close/reopen to change rate, and reopening resets the board). A
    // persisted rate was tried and dropped: a rate the USB chip can't do left the shell unreachable.
    { Preferences p; if (p.begin("sys", false)) { p.remove("baud"); p.end(); } }   // clear the old footgun
    Serial.begin(Shell::bootBaud());
    delay(200);
    Serial.println("\n[esptube-fw] booting...");

    Id::begin();          // MAC-derived id + runtime hostname + unique recovery-AP SSID (before Net/mDNS/OTA)
    Serial.printf("[id] mac=%s host=%s.local ap=%s chip=%s\n", Id::mac(), Id::hostname(), Id::apSsid(), Id::chip());

    Tubes::begin();

    // --- DISPLAY DIAGNOSTIC: fill ALL panels at once (all selected via the
    // 74HC595) with solid colors, to test the shared-bus write path
    // independent of per-tube CS. Raw RGB565 to keep TFT_eSPI out of main. ---
    Serial.println("[diag] ALL panels -> RED");   Tubes::diagAllColor(0xF800); delay(1500);
    Serial.println("[diag] ALL panels -> GREEN"); Tubes::diagAllColor(0x07E0); delay(1500);
    Serial.println("[diag] ALL panels -> BLUE");  Tubes::diagAllColor(0x001F); delay(1500);

    Leds::begin();
    Leds::all(0, 0, 8);   // dim boot glow
    Leds::show();

    Buttons::begin();     // physical buttons (input-only GPIOs, external pull-ups)

    // Immediate hardware smoke test on every LIVE tube — independent of WiFi
    // and the filesystem. Stays on screen until the clock/REST overwrites it.
    Tubes::selfTest();

    if (!LittleFS.begin(true)) {  // format on first boot if needed
        Serial.println("[fs] LittleFS mount failed");
    } else {
        Serial.printf("[fs] LittleFS %u / %u bytes used\n",
                      (unsigned)LittleFS.usedBytes(), (unsigned)LittleFS.totalBytes());
    }

    // Non-blocking WiFi: joins the strongest saved network, or brings up the
    // recovery AP. The services below start regardless of link state so they
    // answer on the AP interface too.
    Net::begin();
    startMDNS();
    Peers::begin();          // background mDNS browse task -> GET /peers (never blocks loop())

    // Mode state machine + NTP clock + brightness/tz (loads NVS, starts SNTP).
    Control::begin();

    Api::begin(server);
    server.begin();
    Serial.println("[http] REST server started on :80");

    Esp1::begin();      // persistent binary frame channel
    Shell::begin();       // UART shell — type `help`

    startArduinoOTA();
#ifdef ESPTUBE_WG_PROBE
    extern void wgProbeBegin(); wgProbeBegin();     // measurement build only
#endif
#ifdef ESPTUBE_BLE_PROBE
    extern void bleProbeBegin(); bleProbeBegin();   // measurement build only
#endif
    // (No Leds::off() here: Control::begin() already cleared the boot glow and
    // then restored the persisted LED effect — clearing again would undo SOLID.)
}

void loop() {
    Net::pump();             // WiFi state machine + captive-portal DNS
    server.handleClient();
    Esp1::pump();          // drain the TCP stream client (draws rows as they arrive)
    Shell::pump();           // UART commands / frames
    ArduinoOTA.handle();
    Api::pump();
    Buttons::update();       // debounce + dispatch physical button presses
    Control::tick();         // render the clock (non-blocking) when in CLOCK mode
}
