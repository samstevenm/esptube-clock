# Pin map & hardware config — SI HAI IPS Clock (CONFIRMED)

> **This board is a SI HAI IPS Clock**, confirmed by PCB silkscreen ("SI HAI IPS CLOCK"),
> its 4 buttons (UP/MODE/DOWN/POWER), and — decisively — by flashing EleksTubeHAX **env `SI_HAI`**,
> which drove the panels correctly (colors + text rendered). These values are pulled verbatim from
> EleksTubeHAX `include/GLOBAL_DEFINES.h` (SI_HAI block) and are proven-correct on our unit.
>
> **Earlier note (superseded):** we first mis-identified it as IPSTube and spent several iterations
> with the wrong pins + a direct-GPIO chip-select. It drew nothing because **SI HAI selects displays
> through a 74HC595 shift register**, and every display pin differs. Lesson: identify the variant
> from the PCB silkscreen first.

## Variant
- EleksTubeHAX variant: **`HARDWARE_SI_HAI_CLOCK`**, PlatformIO env **`SI_HAI`**.
- SoC: ESP32-D0WD (WROOM-32D), **16 MB** flash (SI HAI stock is 4 MB — ours is a 16 MB build;
  repartition for a bigger LittleFS when wanted). RTC: DS1302.

## Displays — 6 × ST7789, 135 × 240, portrait, RGB565
| Signal | GPIO |
|---|---|
| MOSI | **19** |
| SCLK | **18** |
| DC | **16** |
| RST | **23** |
| CS | **−1** (handled by the shift register, below) |
| MISO | −1 (`TFT_SDA_READ`) |
- **`CGRAM_OFFSET` REQUIRED** — TFT_eSPI must apply the 135×240 offset or content lands off-screen
  (looks blank). This is a key difference from IPSTube (which had no offset).
- SPI 40 MHz. Driver via EleksTubeHAX's patched TFT_eSPI fork (stock TFT_eSPI is broken on core 3.x).

## Chip-select — 74HC595 SHIFT REGISTER (not direct GPIO)
| Signal | GPIO |
|---|---|
| DATA (DS) | **4** |
| CLOCK (SHcp) | **22** |
| LATCH (STcp) | **21** |
Select logic (matches EleksTubeHAX): `to_shift = (~digits_map) << 2` (Q7/Q6 unused, active-low),
then latch-low → `shiftOut(DATA, CLOCK, LSBFIRST, to_shift)` → latch-high. `digits_map` bit i =
tube i. All-select = `0x3F` (used during shared init); deselect = `0x00`.

## Underglow / power / buttons
| Function | GPIO |
|---|---|
| WS2812 underglow (6 LEDs, GRB) | **32** |
| Display enable / backlight MOSFET (`TFT_ENABLE_PIN`) | **2** |
| Button UP / LEFT | 35 |
| Button MODE | 34 |
| Button DOWN / RIGHT | 39 |
| Button POWER | 36 |
| DS1302 RTC: SCLK / IO / CE | 33 / 25 / 26 |

## Notes
- No `reclaimPins()` dance needed (that was an IPSTube direct-GPIO/HSPI-collision issue); the
  shift register uses ordinary GPIOs 4/22/21.
- Physical tube index → left/right position still to be **calibrated by eye** (light distinct
  colors per index, note which glass lights).
- Digit images: BMP, 135×240, blitted per-tube via `pushImage` after selecting that tube on the
  shift register — this is the path our REST `POST /tube/{i}/image` reuses.
