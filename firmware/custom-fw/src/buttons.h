#pragma once
// ============================================================================
// buttons.h — 4 physical buttons on INPUT-ONLY GPIOs (34-39), active-LOW with
// external pull-ups. Debounced falling-edge detection in update(); each press
// is routed to Control::onButton() — the same handler the REST /button uses.
// ============================================================================

namespace Buttons {
void begin();
void update();   // call each loop()
}
