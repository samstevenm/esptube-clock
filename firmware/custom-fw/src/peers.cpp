// peers.cpp — see peers.h. Sole caller of MDNS.queryService (its results are a
// shared global, valid only until the next query), so no other code browses.
#include "peers.h"
#include <ESPmDNS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

namespace Peers {
namespace {

String            g_json = "[]";
uint32_t          g_at   = 0;            // millis of the last successful browse (0 = never)
volatile bool     g_refresh = false;     // set by json() when the cache is stale; cleared by the task
SemaphoreHandle_t g_lock = nullptr;

void jstr(String& j, const char* key, const String& val) {
    j += ",\""; j += key; j += "\":\"";
    for (size_t i = 0; i < val.length(); ++i) { char c = val[i]; if (c == '"' || c == '\\') j += '\\'; if ((uint8_t)c >= 0x20) j += c; }
    j += "\"";
}

void browseOnce() {
    const int n = MDNS.queryService("esptube", "tcp");   // ~3 s block — fine on this task, never on loop()
    String j = "[";
    for (int i = 0; i < n; ++i) {
        if (i) j += ",";
        j += "{\"host\":\""; j += MDNS.hostname(i); j += "\"";
        jstr(j, "ip", MDNS.IP(i).toString());
        j += ",\"port\":"; j += MDNS.port(i);
        static const char* kTxt[] = { "id", "name", "model", "fw", "tubes", "proto" };
        for (const char* k : kTxt) if (MDNS.hasTxt(i, k)) jstr(j, k, MDNS.txt(i, k));
        j += "}";
    }
    j += "]";
    if (g_lock) xSemaphoreTake(g_lock, portMAX_DELAY);
    g_json = j; g_at = millis() ? millis() : 1;
    if (g_lock) xSemaphoreGive(g_lock);
}

void task(void*) {
    vTaskDelay(pdMS_TO_TICKS(2000));   // let mDNS settle after boot
    browseOnce();                      // one browse so the first /peers isn't empty
    for (;;) {
        if (g_refresh) { g_refresh = false; browseOnce(); }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

}  // namespace

void begin() {
    g_lock = xSemaphoreCreateMutex();
    // core 0 (with WiFi/lwIP), low priority: the 3 s query yields the CPU while it waits, so it
    // starves nothing; 6 KB stack covers mdns_query_ptr + JSON building.
    xTaskCreatePinnedToCore(task, "peers", 6144, nullptr, 1, nullptr, 0);
}

String json(uint32_t maxAgeMs) {
    if (!g_at || (uint32_t)(millis() - g_at) > maxAgeMs) g_refresh = true;   // ask the task to refresh; return what we have now
    String out;
    if (g_lock) xSemaphoreTake(g_lock, portMAX_DELAY);
    out = "{\"peers\":"; out += g_json; out += ",\"age_s\":"; out += g_at ? (int)((millis() - g_at) / 1000) : -1; out += "}";
    if (g_lock) xSemaphoreGive(g_lock);
    return out;
}

}  // namespace Peers
