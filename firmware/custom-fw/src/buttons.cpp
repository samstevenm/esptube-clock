// ============================================================================
// buttons.cpp — physical buttons via OneButton, ticked on a dedicated task.
//
// TWO deliberate choices, both from hard-won board behavior:
//
//  1. Ticked on its OWN 5 ms task, not from loop(). OneButton classifies press
//     duration from millis() deltas BETWEEN tick() calls. loop() stalls for tens
//     to hundreds of ms during a nixie cross-fade (blocking SPI), so a quick tap
//     that landed inside a gap was measured as an 800 ms hold — a tap "entered
//     the menu" (long) instead of selecting (click). A steady 5 ms task fixes the
//     timing regardless of what loop() is doing. The callbacks only enqueue; the
//     ACTION runs in loop() (Buttons::update drains the queue) so Control/SPI/NVS
//     stay single-threaded.
//
//  2. CLICK-ONLY — no short/long distinction. Short-vs-long was never reliable on
//     this hardware, and the menu doesn't need it: it is context-driven (MODE =
//     open/select/confirm, POWER = back/close, UP/DOWN = move — see control.cpp).
//     One press = one click = one action.
//
// Board: GPIOs 34-39 are INPUT-ONLY, NO internal pull-ups (external pull-ups,
// active-LOW). Objects are default-constructed and set up in begin() so no
// pinMode() runs at static-init time.
// ============================================================================
#include "buttons.h"
#include "config.h"
#include "control.h"
#include <Arduino.h>
#include <OneButton.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

namespace {

const uint32_t BOOT_MUTE_MS = 1500;   // input-only pins float until the external pull-ups settle
const uint32_t MIN_GAP_MS   = 250;    // per-button lockout: reject a repeat of the SAME button within this window
QueueHandle_t  g_q = nullptr;         // button actions (uint8_t = Control::Button), task -> loop()

// Why the lockout: these buttons DOUBLE-FIRE. A single physical press emits two OneButton "click"
// callbacks ~tens-of-ms apart (contact bounce the 40 ms debounce doesn't fully absorb, made worse by
// the fast 5 ms tick sampling every edge). Measured on-device: one MODE press -> two clicks -> the
// menu opened AND immediately selected FACE ("MODE doesn't select" — it over-selected). A 250 ms
// per-button lockout collapses each press to exactly one action; it's invisible for human menu paces.
struct Btn { OneButton ob; uint8_t pin; Control::Button action; const char* name; uint32_t lastMs; };
Btn g_btn[4] = {
    { OneButton(), BUTTON_UP_PIN,    Control::Button::Up,    "UP"    },
    { OneButton(), BUTTON_MODE_PIN,  Control::Button::Mode,  "MODE"  },
    { OneButton(), BUTTON_DOWN_PIN,  Control::Button::Down,  "DOWN"  },
    { OneButton(), BUTTON_POWER_PIN, Control::Button::Power, "POWER" },
};

// Runs on the button task. Only enqueue — the display/NVS work happens in loop().
void onClick(void* p) {
    Btn* b = (Btn*)p;
    uint32_t now = millis();
    if (now - b->lastMs < MIN_GAP_MS) {                 // bounce/double-fire of the same button — drop it
        Serial.printf("[btn] %s (drop dup +%lums)\n", b->name, (unsigned long)(now - b->lastMs));
        return;
    }
    b->lastMs = now;
    Serial.printf("[btn] %s @%lu\n", b->name, (unsigned long)now);
    uint8_t a = (uint8_t)b->action;
    if (g_q) xQueueSend(g_q, &a, 0);
}

void tickTask(void*) {
    vTaskDelay(pdMS_TO_TICKS(BOOT_MUTE_MS));        // ignore phantom float-LOW right after boot
    for (auto& b : g_btn) b.ob.reset();
    for (;;) {
        for (auto& b : g_btn) b.ob.tick();
        vTaskDelay(pdMS_TO_TICKS(5));               // steady cadence — independent of loop() render stalls
    }
}

}  // namespace

namespace Buttons {

void begin() {
    g_q = xQueueCreate(16, sizeof(uint8_t));
    for (auto& b : g_btn) {
        b.ob.setup(b.pin, INPUT, /*activeLow=*/true);   // external pull-ups; pressed = LOW
        b.ob.setDebounceMs(40);
        b.ob.attachClick(onClick, &b);                  // click-only (no long/double — see header)
    }
    // Core 1 (with loop/esp1), priority 2 (above loop's 1): preempts a loop() render stall to tick on
    // time. It only does digitalRead + xQueueSend — never SPI — so preempting a panel write is safe.
    xTaskCreatePinnedToCore(tickTask, "btns", 4096, nullptr, 2, nullptr, 1);
}

void update() {   // loop(): drain queued presses; Control::onButton runs here, single-threaded with rendering
    if (!g_q) return;
    uint8_t a;
    while (xQueueReceive(g_q, &a, 0) == pdTRUE) Control::onButton((Control::Button)a, /*longPress=*/false);
}

}  // namespace Buttons
