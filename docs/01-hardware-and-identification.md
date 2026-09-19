# 01 — Hardware & Device Identification

## Evidence observed (from the three photos, 2026-09-14)

**Photo 1 — hero, powered on**
- Six glass dome tubes, lit, showing nixie-style digits (`1 4 2 7 ? 6`), warm orange glow.
- Purple/pink RGB underglow beneath the tubes.
- Digit rendering is clearly a *raster image on a small display*, not a real neon cathode
  → this is the "IPS/TFT fake-nixie" genre.

**Photo 2 — teardown / detail**
- One glass dome removed at left; underneath is a small square IPS panel showing a digit.
- Main base PCB exposed. **ESP32-WROOM-32D** module silkscreen visible ("…RESSIF /
  …ROOM-32D" = Espressif WROOM-32D).
- A **display carrier board** is being lifted out: small black PCB, a top-side **FPC
  connector**, and the panel's **flex tail** ("014001 XH", "KA / KA" markings on the flex).
- Two tactile buttons visible on the main board; one dome tube at right still lit purple.

**Photo 3 — menu + damage**
- Eye-level; displays showing the stock **SET menu**, one label per tube:
  `SET Style` · `SET LCD Bright` · `SET RGB Bright` · `SET RGB Style` · (tube 5) · `SET OTHER`.
- **Tube 4 glass** shows a visible crack (cosmetic).
- **Tube 5** display is the one with suspected trace damage.
- Base PCB shows: **CR2032** coin cell (RTC backup), a **DC barrel jack** (red "DC-X1"),
  multiple driver ICs and passives.

## What this identifies it as

The combination — 6× IPS-in-glass, ESP32-WROOM-32D, RGB underglow, and specifically the
`Style / LCD Bright / RGB Bright / RGB Style / OTHER` button menu — is the signature of the
**EleksTube IPS clock family** and its many clones sold on AliExpress/Amazon. These share a
common design lineage and are collectively supported by one mature open-source firmware
project (see below).

### Known clone variants in this family
(pinouts and display wiring differ per variant — this is why we confirm before flashing)

- EleksTube IPS (original, several HW revisions)
- SI HAI IPS clock
- NovelLife SE (with/without gesture sensor)
- PunkCyber / "Punk Cyberpunk" clock
- **IPSTUBE** models (e.g. H401) — these tend to use rounded glass domes like ours

The rounded **glass dome** covers (vs. the original EleksTube's squarish acrylic) plus the
ESP32-WROOM-32D lean toward an **IPSTUBE-style** unit or a close generic clone — but this
is not yet confirmed.

## How we confirm the exact variant (do these before flashing/ordering)

1. **WiFi AP name** — power the clock, scan for its SSID. Stock firmwares broadcast a name
   that usually reveals the model/brand. Record it in `hardware/`.
2. **PCB silkscreen** — photograph the base PCB fully (both sides if it opens). Look for a
   model/rev string, and the **USB-UART bridge chip** near the USB connector (CP2102 =
   Silicon Labs, or CH340 = WCH). Its presence tells us whether the USB port is data-capable.
3. **Boot serial banner** — once we get a serial connection (see doc 02), the ESP32 prints a
   boot log; the stock firmware often prints its name/version.
4. **Display panel markings** — photograph the FPC tail and any controller markings on the
   removed panel; measure the active area. Needed to order the correct replacement for tube 5.

## Firmware base we'll build on

**EleksTubeHAX** — the community open-source firmware for this whole clone family
(actively maintained fork by aly-fly; original by SmittyHalibut). Relevant because:

- It already abstracts the **per-variant hardware config** (display driver, pin maps, RGB,
  buttons) behind a config header — the fastest way to a known-good baseline on this hardware.
- It uses **PlatformIO + Arduino-ESP32** and the **TFT_eSPI** display library.
- It supports **WiFi + MQTT** (incl. Home Assistant), NTP time, OTA — a strong starting point
  for our "network control surface + per-tube widget" goals.

Plan: get EleksTubeHAX building and running for our confirmed variant first (proves the
hardware + toolchain), *then* extend/replace it with our own control surface. To verify the
exact repo state, options, and current variant list, we'll pull it up live before relying on
specifics (versions and clone support change over time).

## Open questions to resolve
- Exact clone variant + hardware revision.
- Display panel: controller (ST7789?), resolution, size, FPC pinout, part source.
- Is the USB port data-capable, or is flashing via UART pads only?
- RGB LED type/count and button GPIOs (from the variant config once confirmed).
