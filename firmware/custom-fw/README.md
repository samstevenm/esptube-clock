# esptube-fw

Custom ESP32 firmware for a 6-tube "Nixie-style" clock built from **6× ST7789
135×240 IPS panels** on a shared SPI bus, WS2812 underglow, and a WiFi REST API
with OTA updates.

## Hardware

Board identified as a **SI HAI IPS tube clock** (pins verified against
EleksTubeHAX's `SI_HAI` env, confirmed-correct on this hardware).

| Function            | Detail                                                           |
| ------------------- | ---------------------------------------------------------------- |
| MCU                 | ESP32-D0WD (WROOM-32D), 16 MB flash, no PSRAM                    |
| Panels              | 6× ST7789 135×240 IPS, portrait, RGB565, shared SPI bus          |
| SPI                 | MOSI=19, SCLK=18, DC=16, RST=23, MISO=-1, ~40 MHz, `CGRAM_OFFSET` |
| Chip-select         | **74HC595 shift register**: DATA=4, CLOCK=22, LATCH=21 (active-LOW at panels) |
| Display enable      | MOSFET on GPIO2 (HIGH = on)                                      |
| Underglow           | WS2812 ×6 on GPIO32, GRB (brightness-scaled; chain reversed vs displays — see below) |
| Buttons             | UP=35, MODE=34, DOWN=39, POWER=36 — input-only, active-LOW, **external** pull-ups, debounced |
| Populated tubes     | **persisted setting** (NVS mask, default `0x3E` = slots 1-5 on, slot 0 empty); set via web UI / `/config/populated` |

Chip-select is a 74HC595 (`TFT_eSPI` is compiled with `TFT_CS=-1` so the
library never touches CS). We clock out `(~digits_map) << 2` — active-LOW, with
the register's two high outputs Q7/Q6 unused. Every draw = *select one tube →
draw → deselect all*. `CGRAM_OFFSET` is **required** on this board; without it
content lands off-screen and the panels look blank.

### Populated tubes (persisted)

Displays are interchangeable and any slot may be empty or dead. Reliable
auto-detect isn't possible on this hardware, so the **populated set is a
persisted setting**: a 6-bit mask (bit i => tube i present) stored in NVS
(`Preferences` namespace `esptube`, key `popmask`, **default `0x3E`** = slots
1-5 present, slot 0 / far-right empty), loaded into a runtime mask at boot. It
drives `alive(i)`, the self-test, the clock layout, and `/status`.
`DEAD_TUBES_MASK` (config.h, default `0`) remains only as an optional
compile-time force-off, OR-ed into deadness.

Set it at runtime from the web UI (below) or `POST /config/populated {"mask":N}`.

`Tubes::detectPopulated()` still runs at boot but is **informational only** — it
selects each position alone via the 595, reads the ST7789 `RDDID`/`RDDST`/`RDDPM`
registers (possible because of `TFT_SDA_READ`), and LOGs the raw bytes
(`[detect] tube i: RDDID=0x.. RDDST=0x.. RDDPM=0x.. -> guess=..`) as a
calibration aid. It does **not** change the populated mask.

Note: the index→physical-tube mapping is **not yet calibrated** for this board
(slot 0 = far right). The boot diagnostic + self-test light distinct tubes in
distinct colors so the mapping can be read off by eye.

## Web control panel

`data/index.html`, served at `GET /`, is a **minimal safe fallback** — a banner
points to `tools/helper.html` (the full console) for per-tube widgets,
animations, images and emoji. The fallback keeps only safe basics: per-tube
text + glow + clear, mode buttons (Clock/Rainbow/Breathe/Comet/Off), brightness,
populated-tubes, and status. It has **no image-upload control** (raw-body image
posts can OOM the device — use the helper's `/raw565` / multipart `/image`
instead) and does GET-only polling on load (never auto-pushes content).

It is **embedded in the app image**, not served from LittleFS — a pre-build step
(`scripts/gen_web_index.py`, wired via `extra_scripts`) regenerates
`src/web_index.h` (a PROGMEM raw-string literal) from `data/index.html` on every
build, so editing the UI needs no manual re-paste and deploy stays a single
~1 MB app flash (no filesystem upload). The page uses same-origin relative
`fetch`; all API responses also send `Access-Control-Allow-Origin: *` and OPTIONS
preflight is handled, for external tools like the helper.

## Layout

```
custom-fw/
├── platformio.ini              # env, pinned platform, TFT_eSPI -D config, build_dir, extra_scripts
├── partitions_16mb_ota.csv     # 2 MB app ×2 (OTA) + ~11.9 MB LittleFS + coredump
├── data/
│   └── index.html              # web control panel UI (embedded into the app, NOT flashed to FS)
├── scripts/
│   └── gen_web_index.py        # pre-build: embeds data/index.html -> src/web_index.h
├── lib/
│   └── modified_TFT_eSPI/      # EleksTubeHAX's patched TFT_eSPI fork (vendored, see below)
├── include/
│   ├── secrets.h               # optional UNUSED fallback (gitignored; no real creds)
│   └── secrets.h.example
└── src/
    ├── config.h                # pins, tube count, DEAD_TUBES, hostname
    ├── web_index.h             # GENERATED (gitignored) PROGMEM copy of data/index.html
    ├── main.cpp                # boot self-test, WiFiManager, mDNS, LittleFS, server, ArduinoOTA
    ├── tubes.{h,cpp}           # shared-bus ST7789 display + persisted populated mask (NVS)
    ├── leds.{h,cpp}            # WS2812 underglow wrapper (brightness-scaled)
    ├── control.{h,cpp}         # mode state machine + NTP clock + brightness/timezone (NVS)
    ├── buttons.{h,cpp}         # debounced physical buttons -> Control
    └── api.{h,cpp}             # REST routes + embedded UI + config endpoints + CORS
```

Build artifacts go to `build_dir = /Users/sm/Developer/esptube-build` (local
disk, off the iCloud folder).

## Display driver — why the vendored TFT_eSPI

Stock `bodmer/TFT_eSPI@2.5.43` leaves these panels **backlit but blank** on
Arduino-ESP32 core 3.x (our platform `espressif32@7.1.3`). We instead vendor
**EleksTubeHAX's patched fork** (`TFT_eSPI@2.5.43-elekstubehax1`) at
`lib/modified_TFT_eSPI/`, whose SPI works on core 3.x. It is **not** in
`lib_deps` (that would create a duplicate `TFT_eSPI.h`); the local `lib/` copy
wins. Config is passed entirely via `-D` build_flags (`USER_SETUP_LOADED`),
mirroring EleksTubeHAX's **SI_HAI** setup for this ST7789 135×240 hardware:
MOSI=19/SCLK=18/DC=16/RST=23, `TFT_CS=-1`, **`CGRAM_OFFSET` (required)**,
`TFT_SDA_READ`, no explicit inversion (panel power-on default).

Shared-bus init in `Tubes::begin()`: bring the 595 control lines up, select
**all six tubes** (`csWrite(0x3F)`) so the ST7789 init sequence broadcasts to
every panel, run `tft.init()` → `setRotation` → `fillScreen(BLACK)` while still
selected, then deselect. Because CS is a shift register (its lines don't
collide with the SPI pins), no post-init GPIO reclaim is needed — unlike the
direct-GPIO boards.

## Partition layout (16 MB)

| Partition | Type | Offset     | Size        |
| --------- | ---- | ---------- | ----------- |
| nvs       | data | 0x9000     | 20 KB       |
| otadata   | data | 0xE000     | 8 KB        |
| app0      | app  | 0x10000    | 2 MB        |
| app1      | app  | 0x210000   | 2 MB        |
| spiffs    | data | 0x410000   | ~11.875 MB (LittleFS) |
| coredump  | data | 0xFF0000   | 64 KB       |

The filesystem partition is subtype `spiffs` (label `spiffs`) but formatted as
**LittleFS** (`board_build.filesystem = littlefs`); the default
`LittleFS.begin()` partition label matches, so no explicit label is needed.

## Build

```bash
cd custom-fw
pio run -e esptube
```

- No WiFi credentials are compiled in — provisioning is done on-device (see
  below). `secrets.h` is an optional, currently-unused fallback.
- Platform is pinned to the already-installed `espressif32@7.1.3` (Arduino
  core 3.x). `Adafruit NeoPixel` and `WiFiManager` download into the local
  `./.pio/libdeps`; `TFT_eSPI` is the vendored fork in `lib/` (see above).
- Output firmware: `.pio/build/esptube/firmware.bin`.

## Boot self-test & WiFi provisioning

- **Boot self-test:** the instant it powers on (before WiFi, independent of the
  filesystem), every *live* tube fills a distinct color and shows its index
  digit `0`..`5` large + centered. Dead tube (index 1) is skipped. This proves
  the panels/SPI bus with no network. It stays on screen until the first
  REST/clock update overwrites it.
- **WiFi (WiFiManager, non-blocking):** on boot it tries credentials saved in
  NVS; if there are none (or the join fails) it starts an **open captive-portal
  SoftAP named `esptube-setup`** where you enter the WiFi password on-device.
  `autoConnect()` runs in non-blocking mode (`setConfigPortalBlocking(false)`,
  serviced by `wm.process()` in `loop()`), so mDNS (`esptube.local`), the REST
  server, and ArduinoOTA **start immediately regardless of link state** — they
  answer on the SoftAP interface (`192.168.4.1`) and on home WiFi once creds are
  entered. A blank or wrong join can never strand the device.

## Flash (not performed by the scaffold)

USB (first time):

```bash
pio run -t upload            # add --upload-port /dev/cu.XXXX if needed
pio run -t uploadfs          # write the LittleFS image
```

OTA afterwards (no USB): either the `POST /ota` endpoint below, or ArduinoOTA
(`pio run -t upload --upload-port esptube.local`).

## REST API

Base: `http://esptube.local/` (mDNS) or the device IP.

| Method | Path                 | Body                       | Action |
| ------ | -------------------- | -------------------------- | ------ |
| GET    | `/`                  | —                          | serve the embedded web control panel (text/html) |
| GET    | `/status`            | —                          | `{uptime_ms, wifi, ip, hostname, free_heap, fs_used, fs_total, tubes:[{index, populated, content:{kind,text}, led:{r,g,b}}], dead_tubes, populated_mask, mode, led_effect, led_solid:{r,g,b}, brightness, tz, time_valid}` |
| GET    | `/health`            | —                          | `{"ok":true}` |
| GET    | `/config`            | —                          | `{populated_mask, tubes:[{index,populated}], mode, brightness, tz}` |
| POST   | `/config/populated`  | `{"mask":N}` (N=0–63) or `mask=N` | persist populated set to NVS |
| POST   | `/config/brightness` | `{"value":N}` (0–255) or `value=N` | set + persist global brightness (scales LEDs) |
| POST   | `/config/tz`         | `{"tz":"PST8PDT,M3.2.0,M11.1.0"}` or `tz=…` | set + persist POSIX timezone, re-apply NTP |
| POST   | `/mode/{clock\|off\|manual}` | —                  | set **display** (panel) mode → `{"ok":true,"mode":…}` |
| POST   | `/led/{off\|solid\|rainbow\|breathe\|comet}` | optional `{"r","g","b"}` for solid | set **LED-effect** layer → `{"ok":true,"led_effect":…,"led_solid":{…}}` |
| POST   | `/tube/{0-5}/text`   | raw text                   | draw centered text on a tube (→ MANUAL mode) |
| POST   | `/tube/{0-5}/image`  | BMP file, **multipart/form-data** | 24/8-bit BMP, decoded + blitted row-by-row (capped ~110 KB; → MANUAL) |
| POST   | `/tube/{0-5}/raw565` | **64800 bytes** RGB565 (multipart) | full-frame streamed straight to panel, no alloc (→ MANUAL) |
| POST   | `/tube/{0-5}/rgb`    | `{"r":N,"g":N,"b":N}` (any Content-Type) or `r=&g=&b=` | set that tube's underglow LED (clamped 0–255; → MANUAL) |
| POST   | `/tubes/clear`       | —                          | clear all panels + LEDs off (→ MANUAL) |
| POST   | `/button/{mode\|power\|up\|down}` | —             | real button action (aliases: `left`→up, `right`→down) |
| POST   | `/ota`               | firmware .bin (multipart)  | write to OTA slot via `Update.h`, then reboot |

Examples:

```bash
curl http://esptube.local/status
curl -X POST http://esptube.local/mode/clock
curl -X POST --data '3' http://esptube.local/tube/0/text
curl -X POST -H 'Content-Type: application/json' \
     --data '{"r":0,"g":0,"b":255}' http://esptube.local/tube/0/rgb
curl -X POST -H 'Content-Type: application/json' --data '{"value":128}' http://esptube.local/config/brightness
curl -X POST -H 'Content-Type: application/json' --data '{"tz":"PST8PDT,M3.2.0,M11.1.0"}' http://esptube.local/config/tz
curl -X POST -F 'firmware=@/Users/sm/Developer/esptube-build/esptube/firmware.bin' http://esptube.local/ota
```

Non-populated tubes are silently skipped by every display op and never block.

**Image memory-safety.** `/image` is multipart-only (no raw-body path), streamed
into a capped (~110 KB) buffer that aborts with `400` rather than OOM, then
decoded **one row at a time** into a ~270 B stack line buffer (no full-frame
heap alloc). `/raw565` never buffers a frame at all — it blits each row straight
to the panel as bytes arrive.

**`/raw565` byte order.** Body is exactly `135*240*2 = 64800` bytes, row-major,
**top-down**. Each pixel is **2 bytes, HIGH byte first** (big-endian RGB565):
`byte0 = (color>>8)&0xFF`, `byte1 = color&0xFF` where `color` is a standard
RGB565 value (e.g. red `0xF800` → `0xF8, 0x00`). The device reconstructs
`color = (byte0<<8)|byte1` and pushes with `setSwapBytes(true)` (same path as
`fillScreen(0xF800)`), so this order renders correct colors.

## Two layers: display + LED effect

The panels and the underglow are **independent layers** — e.g. the clock can run
on the panels while a rainbow runs on the underglow at the same time.

**Display layer** (`src/control.cpp`): `CLOCK` (NTP time), `OFF` (panels blank),
`MANUAL` (a REST content push took over; the clock stops overwriting). `POWER`
toggles the display on/off; a panel-content push auto-switches to `MANUAL`.

**LED-effect layer** (WS2812, stepped in `loop()` at ~40 ms, brightness-scaled,
independent of the display): `OFF`, `SOLID` (a solid color; per-tube `/rgb` sets
individual underglow when the effect is off/solid), `RAINBOW` / `BREATHE` /
`COMET` (live animations that override per-tube `/rgb` until set back to
off/solid). LED math cribbed from FastLED/EleksTubeHAX (RAW colors, so global
brightness still scales them): **Rainbow** = hue sweep with a per-tube phase
offset; **Breathe** = triangle fade with a drifting hue; **Comet** = a bright
head with a fading tail.

**Buttons** — the SAME handler serves physical buttons and REST `/button/*`.
Physical GPIOs: MODE=34, UP=35, DOWN=39, POWER=36 (input-only, external
pull-ups, active-LOW; ~600 ms long-press detected):

| Button | Effect |
| ------ | ------ |
| MODE   | **next LED effect** — cycles `Off → Rainbow → Breathe → Comet → Solid → Off` (panels keep showing the clock/content) |
| UP     | brightness +32 |
| DOWN   | brightness −32 |
| POWER  | display on/off toggle (`OFF ⇄` last non-off display mode), short or long |

REST `/button/{mode|up|down|power}` mirror these (`left`→up, `right`→down
aliases); `/button/mode` cycles the LED effect.

Panel-content pushes (`/tube/*/text|image|raw565`, `/tubes/clear`) switch the
**display** to `MANUAL`; `/tube/*/rgb` is underglow only and does **not** change
the display.

**Clock** — `configTzTime(pool.ntp.org, <POSIX TZ>)`; time renders across the
populated tubes each second, redrawing only tubes whose digit changed.
**Positional, native index 0 = far right**: tube i shows `HHMMSS[5-i]` — i=0
sec-ones, 1 sec-tens, 2 min-ones, 3 min-tens, 4 hr-ones, 5 hr-tens. Unpopulated
tubes stay blank. Current layout (index 0 / far-right empty) shows HH MM and
tens-of-seconds.

**Brightness** — global 0-255 (default 180, persisted). Scales the WS2812 LEDs.
TFT panels stay full-on (SI HAI has no safe PWM-dim path for the shared enable
pin), so only the LEDs dim.

**LED↔display alignment** — the WS2812 glow chain runs opposite the shift-
register display order, so `leds.cpp` maps display index i → strip pixel
`LED_COUNT-1-i` (single reversal point). After this, `/tube/i/rgb` and
`/tube/i/text` address the same physical tube.

**Per-tube boot kick** — some panels (reported on index 1) ignore their FIRST
individual CS transaction right after boot; only a *spanning* write (all CS
asserted at once) makes subsequent per-tube updates take. The broadcast init
fires the instant the panels power on, so the first lone per-tube select can
miss (CS/level-shifter settle timing, worst on the marginal index-1 FPC). Fix in
`Tubes::begin()`: after the broadcast init, wait ~50 ms, then individually
select + clear each tube once — so the first REAL per-tube update is no longer
that panel's first-ever individual selection. (If it persists, it's a hardware
FPC reseat on that tube.)
