// ============================================================================
// buttons.cpp — physical buttons via the OneButton library (debounced click /
// long-press / multi-click), routed to Control::onButton() (the same handler
// REST /button uses).
//
// Why a library: the previous hand-rolled edge state machine mis-timed short vs
// long on this board (a tap in the menu could register as a long-press and just
// toggle the menu shut — so MODE never "selected"). OneButton is the solved
// version: one debounce point, clean click/longPress separation, and click-COUNT
// (getNumberClicks / attachMultiClick) — so a SINGLE-button board (e.g. IPSTube
// H401) can select with "2 clicks" using the same code path (see SINGLE-BUTTON).
//
// Board: GPIOs 34-39 are INPUT-ONLY with NO internal pull-ups (external pull-ups,
// active-LOW). So each button is set up as OneButton(pin, INPUT, activeLow=true).
// The objects are default-constructed and configured in begin() so no pinMode()
// runs at static-init time (before the Arduino core is up).
// ============================================================================
#include "buttons.h"
#include "config.h"
#include "control.h"
#include <Arduino.h>
#include <OneButton.h>

namespace {

// Input-only pins float until the external pull-ups settle after power-up; ignore
// anything in this window so a phantom LOW can't max brightness or open the menu.
const uint32_t BOOT_MUTE_MS = 1500;
uint32_t g_t0   = 0;
bool     g_live = false;

struct Btn { OneButton ob; uint8_t pin; Control::Button action; const char* name; };
Btn g_btn[4] = {
    { OneButton(), BUTTON_UP_PIN,    Control::Button::Up,    "UP"    },
    { OneButton(), BUTTON_MODE_PIN,  Control::Button::Mode,  "MODE"  },
    { OneButton(), BUTTON_DOWN_PIN,  Control::Button::Down,  "DOWN"  },
    { OneButton(), BUTTON_POWER_PIN, Control::Button::Power, "POWER" },
};

void onClick(void* p) {
    if (!g_live) return;
    Btn* b = (Btn*)p;
    Serial.printf("[btn] %s click\n", b->name);
    Control::onButton(b->action, /*longPress=*/false);
}
void onLong(void* p) {
    if (!g_live) return;
    Btn* b = (Btn*)p;
    Serial.printf("[btn] %s long\n", b->name);
    Control::onButton(b->action, /*longPress=*/true);
}

}  // namespace

namespace Buttons {

void begin() {
    g_t0 = millis();
    for (auto& b : g_btn) {
        b.ob.setup(b.pin, INPUT, /*activeLow=*/true);   // external pull-ups; pressed = LOW
        b.ob.setDebounceMs(50);
        b.ob.setClickMs(250);            // snappy single click (no double-click handler on this 6-tube board)
        b.ob.setPressMs(800);            // long-press threshold: an ordinary tap (<800ms) is a CLICK; only a deliberate hold is long
        b.ob.attachClick(onClick, &b);
        b.ob.attachLongPressStart(onLong, &b);   // fires ONCE at the threshold — no during-long auto-repeat (it spammed nav/brightness)
        // SINGLE-BUTTON boards: also attachDoubleClick/attachMultiClick here and map
        // click->navigate, 2 clicks->select, long->back. This board has four, so click=action.
    }
}

void update() {
    if (!g_live) {
        if (millis() - g_t0 < BOOT_MUTE_MS) return;
        for (auto& b : g_btn) b.ob.reset();   // start clean once the boot-mute window has passed
        g_live = true;
    }
    for (auto& b : g_btn) b.ob.tick();
}

}  // namespace Buttons
