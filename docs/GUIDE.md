# ESPTube Clock — Hacking Guide

Everything you need to build, flash, connect, and drive this clock solo.

---

## 1. What it is (1-paragraph recap)

A **SI HAI IPS Clock**: an ESP32 (WROOM-32D, 16 MB flash) driving **6 ST7789 135×240
IPS panels** in glass tubes, with a WS2812 RGB LED under each and 4 buttons. The panels
are on a shared SPI bus selected by a **74HC595 shift register**. We replaced the broken
stock firmware with **our own firmware** (`firmware/custom-fw/`) that exposes an **HTTP REST
API** so scripts can push text / colors / images to any tube. Full pin map:
[`hardware/pinmap.md`](../hardware/pinmap.md).

## 2. What works today

- Custom firmware boots, runs a self-test, joins WiFi (captive portal), serves REST + OTA.
- REST: per-tube **text**, **RGB underglow**, **BMP image**, clear, virtual buttons, status.
- Flashing over **USB** (via the powered hub) *and* **OTA over WiFi** after the first flash.
- **Known limitations:** tube auto-detect isn't possible (panels don't support SPI read-back),
  so all 6 slots are always "active" and empty/dead slots simply stay dark. Physical tube 1
  (2nd from right) currently has a torn flex = dark.

## 3. Layout

```
esptube-clock/
├── firmware/
│   ├── custom-fw/          ← THE firmware source you edit (PlatformIO project)
│   │   ├── src/            main.cpp, tubes.*, leds.*, api.*, config.h
│   │   ├── lib/modified_TFT_eSPI/   patched display driver (needed for ESP32 core 3.x)
│   │   ├── include/secrets.h(.example)
│   │   └── platformio.ini  (build_dir points to LOCAL disk, not iCloud)
│   ├── esptube-custom-B.bin   last-flashed binary
│   ├── stock-backup*.*        partial stock flash backup + log
│   └── FLASHING.md            raw esptool commands / restore notes
├── tools/
│   ├── esptube             REST CLI (Python, no deps)
│   ├── flash.sh            build + USB-flash
│   └── monitor.sh          serial console
├── hardware/pinmap.md      the confirmed SI HAI pin map
├── docs/                   this guide + spec/identification/connection docs
└── images/                 photos (+ manifest) for the eventual blog
```

Builds are written to `/Users/sm/Developer/esptube-build/` (local, so iCloud isn't thrashed).
The EleksTubeHAX reference firmware lives at `~/Developer/esptube-clock/EleksTubeHAX/`.

## 4. Prerequisites (already installed on this Mac)

- **PlatformIO** (`pio`) and **esptool** — via Homebrew. If missing: `brew install platformio esptool`.
- The ESP32 toolchain is already downloaded into `~/.platformio`.

## 5. Build & flash

**One command (build + USB flash):**
```bash
cd "…/esptube-clock/tools"
./flash.sh                       # uses /dev/cu.usbserial-1220 (the powered hub)
```
The clock powers/enumerates **only through the powered USB hub**, not the Mac's port directly.
Small (~1 MB) writes are reliable; a full 16 MB read is not (that's why the stock backup is partial).

**Build only:** `pio run -d ../firmware/custom-fw -e esptube`

**Update over WiFi (no cable) after the first flash:**
```bash
pio run -d ../firmware/custom-fw -e esptube -t upload --upload-port esptube.local   # espota, port 3232
# or: ~/.platformio/packages/framework-arduinoespressif32/tools/espota.py -i <clock-ip> -p 3232 -f /Users/sm/Developer/esptube-build/esptube/firmware.bin
# or: curl --data-binary @/Users/sm/Developer/esptube-build/esptube/firmware.bin http://<ip>/ota
# then: curl -s http://<clock-ip>/status | grep build    # a reboot alone proves nothing; the build stamp does
```

If esptool can't sync over USB, hold nothing — our board auto-resets; just retry. If truly stuck,
drop `--baud` to 115200 (already the default in flash.sh).

## 6. Connect to WiFi

The clock keeps up to **eight networks**: at boot it first tries the one that worked last time
without scanning (a boot-time scan often misses an AP that is right there, and hidden SSIDs never
show), then scans and joins the strongest one it knows (`src/net.*`).

1. **Recovery hotspot.** Whenever no saved network is in range (first boot, a move, a new router, an
   outage) the clock raises `esptube-setup` — WPA2, password **`esptube-setup`**, `192.168.4.1` —
   and scrolls that once on the glass. Join it on a phone; the captive-portal sheet *is* the clock's
   page. Pick your 2.4 GHz network (`YOUR_WIFI_SSID`), enter the password **on the device** (never in the
   source). The clock joins, remembers it, and closes the hotspot ~90 s later. Long-press MODE → **AP**
   raises the hotspot on demand; it never interrupts a face or scene beyond that one announce.
2. **Anywhere else.** Add / forget / join networks from the helper (⚙️ Setup → 📶 WiFi over REST, or the
   🧷 serial panel over the cable with no network at all), the on-device page, or the shell:
   `wifi` · `wifi scan` · `wifi list` · `wifi add "Name" pass` · `wifi forget Name` · `wifi join Name` ·
   `wifi ap on|off` · `wifi appass <8+ chars>` · `wifi reconnect`.
3. Find it afterwards: `esptube.local` (mDNS), `tools/esptube status`, or your router. Current IP:
   **<clock-ip>**.

**No WiFi needed for the clock itself.** The board's DS1302 RTC (battery-backed) seeds the time at
boot and is written on every NTP sync; a clock that has never been online is set with the helper's
🕐 *Set time from here* (`POST /config/time`) or `time <epoch>` in the shell (`rtc` shows the chip).

## 7. Drive it — REST API

Set the address once: `export ESPTUBE_IP=<clock-ip>`  (or rely on `esptube.local`).

**With the CLI** (`tools/esptube`):
```bash
./esptube status                 # JSON: ip, heap, per-tube alive/populated, dead_tubes
./esptube health
./esptube text 0 HI              # draw "HI" on tube 0 (0 = far RIGHT)
./esptube rgb 0 255 0 0          # tube 0 underglow -> red
./esptube image 3 face.bmp       # blit a 24- or 8-bit BMP (135×240) to tube 3
./esptube clear                  # blank all tubes
./esptube button mode            # inject a virtual button (mode|left|right|power)
```

**Raw HTTP** (same thing, for your own scripts in any language):
| Method & path | Body | Effect |
|---|---|---|
| `GET /status` | — | full state JSON |
| `GET /health` | — | `{"ok":true}` |
| `POST /tube/{0-5}/text` | raw text | draw centered text |
| `POST /tube/{0-5}/rgb` | `{"r":N,"g":N,"b":N}` | set that tube's underglow |
| `POST /tube/{0-5}/image` | BMP bytes (multipart) | blit image (24/8-bit, centered; ≤110 KB) |
| `POST /tube/{0-5}/raw565` | 64,800 B RGB565 big-endian (multipart) | full-frame push, streamed row by row |
| `POST /tube/{0-5}/blit?x=&y=&w=&h=` | w×h×2 B RGB565 big-endian (multipart) | write just that rectangle (clamped to the panel) — what the helper sends for changed row bands |
| `POST /reboot` | — | restart the clock (~0.5 s later) |
| `POST /tubes/clear` | — | **master clear** (returns `{"epoch":N}`; supersedes frames still arriving over any stream, so nothing repaints over it): stops message/countdown, sweep and LED effect; every panel black, every glow off; stays dark until content or the idle timer |
| `POST /mode/{clock\|off\|manual}` | — | display layer |
| `POST /led/{off\|solid\|rainbow\|breathe\|comet}` | solid: `{"r","g","b"}` optional | underglow layer (runs over the display) |
| `POST /preset/{next\|0-4}` | — | device-native face: 0 🕯️ Nixie · 1 Digital · 2 LED show · 3 Off · 4 📅 Date (what short MODE cycles) |
| `POST /nixie/text` | `{"text":"HELLO","effect":"static\|flash\|scroll","ms":400}` | message in nixie glyphs, rendered by the clock (A–Z 0–9 `- : . ! ? ° % + /`) |
| `POST /nixie/countdown` | `{"seconds":90}` | countdown (HHMMSS / MMSS), flashes 0000 at the end, back to the clock |
| `POST /nixie/stop` | — | back to the clock |
| `POST /config/brightness` · `/config/tz` · `/config/populated` | `{"value":N}` · `{"tz":"POSIX"}` · `{"mask":N}` | persisted settings |
| `POST /test/clocksweep` · `/test/stop` | `{"from":0,"to":86399,"step_ms":0,"fade":1}` | drive every time in the range through the real render path; progress in `/status.sweep` |
| `POST /config/token` | `{"token":"…"}` (empty = open) | shared secret for the ESP/1 stream channel |
| `POST /config/time` | `{"epoch":N}` | set the clock (and the DS1302 RTC) — for a clock with no internet |
| `POST /config/idle` | `{"minutes":N}` | MANUAL → clock face after N minutes without content (default 30; 0 = never); `/status.idle_min` |
| `POST /config/netwarn` | `{"dbm":-78}` | weak-signal warning: when the smoothed RSSI sits below this for 60 s the clock scrolls `WIFI WEAK -82 DBM` once (≤ 1 per 10 min, 9 s, **only on its own clock face** — never over pushed/streamed content, messages or "off"); 0 = off; persisted; `/status.netwarn` |
| `GET /layout.json` | — | where every display physically is (`esptube.layout/1`, millimetres, ~125 bytes for an even row); written with `POST /fs/put/layout.json` by the helper's 🧊 Stage; read by phones, the bridge (`--geometry clock`), other computers. 404 until saved |
| `GET /ping` | — | `{"ok":true,"t":millis}` — the cheapest round trip (the helper's ⚡ link test). `/status` is ~45 ms now: its LittleFS usage figure is cached 60 s (a full-partition walk cost ~250 ms per poll before) |
| `GET /helper` · `/manifest.webmanifest` · `/icon.png` | — | the full helper (gzipped, from flash) as a PWA — open on a phone, *Add to Home Screen* |
| `GET /fs` · `POST /fs/put/<path>` (raw body) · `POST /fs/delete?path=/x` · `GET /samples/…` | — | LittleFS asset store (e.g. `characters.js` for the hosted helper) |
| `GET /wifi` | — | `{connected,ssid,ip,rssi,rssi_avg,warn_dbm,state,ap,ap_ssid,ap_pass,ap_ip,ap_clients,saved[],scan[]}` (no passwords out; `rssi_avg` = ~20 s EMA) |
| `POST /wifi/scan` | — | scan (~3 s) and return the list with `known` flags |
| `POST /wifi/add` · `/wifi/forget` · `/wifi/join` | `{"ssid","pass"}` · `{"ssid"}` · `{"ssid"}` | remember + join now (up to 8) · forget · join a saved one |
| `POST /wifi/ap` · `/wifi/appass` · `/wifi/reconnect` | `{"on":bool}` · `{"pass"}` · — | recovery hotspot on/off · its password (8+ chars) · rescan & join strongest |
| **ESP/1 stream** — TCP `:5555` (≤6 clients) · **WebSocket `:5557`** (binary = frames, text = shell) · UDP `:5556` · the USB UART | 20-byte header + RAW565 / RLE565 / PAL4 / PAL4x2 payload, 4-byte acks | binary frame channel for video ([`ESP1-PROTOCOL.md`](ESP1-PROTOCOL.md)); `/status.stream` has fps/KB/s/busy% |
| **UART shell** @115200 | `help status preset nixie flash scroll countdown stop led bright mode wifi sweep fps baud token reboot` | text lines on the USB serial port (`tools/bridge/bridge.py shell`); note: opening the port reboots the board |
| `POST /button/{mode\|up\|down\|power}` | — | virtual button (same handler as the physical ones) |
| `POST /ota` | firmware.bin — raw body (`curl --data-binary @firmware.bin`) or multipart | over-the-air firmware update; afterwards check `build` in `/status` — a reboot alone can be a panic back into the old image (`reset_reason: "panic"`) |

`GET /status` also reports `build` (firmware compile date/time — which image is actually running),
`preset`, `led_effect`, `draw_ms` (last nixie draw), `reset_reason` (crash vs. power-cycle),
`nixie` (when a message is showing) and `sweep` (test progress).

**Tests:** `test/rest-smoke.sh <ip>` (38 assertions against a live clock, including region blits, the
nixie endpoints, the Date face and a midnight clock-sweep slice), `test/clock-sweep.sh <ip> [from] [to] [fade]` (every valid time through the real
render path — whole day ≈ 8 min without fades on the 5-tube layout, ~4× with), and `test/nixie-native.sh` (no hardware: every glyph
pair × every fade step, and all 86,400 clock times through the transition logic).

**Tube index model:** index **0 = far right**, increasing to the left (0..5). Each index is a fixed
physical *slot* on the shift register — plug a panel into a slot and it shows that index's content;
empty slots stay dark. Rearrange panels freely; indices stay tied to slots.

Example — push the time from a cron/script:
```bash
now=$(date +%H%M%S)          # 6 digits; tube 1 is dark so it'll have a gap
for i in 0 2 3 4 5; do ./esptube text $i "${now:$((5-i)):1}"; done
```

## 8. Watch the serial log (debugging)

```bash
cd "…/esptube-clock/tools" && ./monitor.sh      # 115200; Ctrl-C to quit
```
The firmware logs boot, tube detect read-back, WiFi, and each REST call. (Serial needs the USB
hub connection; REST/OTA do not.)

No terminal needed: the helper's **🧷 USB serial terminal** panel (Chrome/Edge, Web Serial) opens the
same port from the page — boot log, crash backtraces, the text shell (`help` lists it) with history and
one-click command chips, timestamps and a log download. Opening the port resets the clock (DTR
auto-reset) — the boot log comes first, that's normal. One owner per port: close it before
`flash.sh` / `monitor.sh` / the bridge (`lsof /dev/cu.usbserial-*` shows who has it).

**The cable as a control and streaming link.** *Control over the cable* (auto) makes the whole helper
work with no network: REST calls become shell lines, pixel pushes and 📹 Live become ESP/1 frames with
acks on the same port, and it switches back when the clock answers over WiFi again. The bridge
(`--transport auto`, the default) does the same for its sources and steps the link up to `--baud`
(921600) by asking the shell. Shell extras for this: `status` (JSON incl. `populated_mask`, `wifi`,
`time_valid`, `build`), `wifi json`, `baud N [once]` (switches now; `once` also arms N for exactly one reset — the helper's ⚡ Fast
link; nothing persists), `time [epoch]`, `rtc [set]`, `clear`, `text <tube> <words>`, `rgb <tube> r g b`,
`idle [min]`.

## 9. Firmware architecture (`firmware/custom-fw/src/`)

- **`config.h`** — pins, tube count, `DEAD_TUBES_MASK` (manual force-off), hostname.
- **`tubes.{h,cpp}`** — the display layer: 74HC595 chip-select (`csWrite`), `tft.init()`,
  `selectTube/deselect`, `drawText`, `drawImageBMP` (BMP parser), `fill`, `clearAll`, `selfTest`,
  and the (currently non-functional) read-back `detectPopulated`.
- **`leds.{h,cpp}`** — WS2812 wrapper (Adafruit NeoPixel) on GPIO32.
- **`api.{h,cpp}`** — REST routes + body parsing + the virtual-button queue.
- **`main.cpp`** — boot order (tubes → self-test → LittleFS → non-blocking WiFiManager → mDNS →
  REST server → ArduinoOTA), and `loop()`.

To add an endpoint: add a route in `api.cpp`, call into `Tubes::`/`Leds::`, rebuild, `./flash.sh`
(or OTA). To change a pin: `config.h` **and** the matching `-D` in `platformio.ini` (the display
pins live in build_flags; `config.h`'s SPI macros are documentation only).

## 10. Troubleshooting

| Symptom | Cause / fix |
|---|---|
| Panels backlit but blank | Wrong variant/pins — this board is **SI HAI** (shift-register CS). Config is already correct; if you fork it, keep `CGRAM_OFFSET` and the 595 CS. |
| `curl` connection refused, but ping works | HTTP server bound before WiFi came up (first-config boot). **Power-cycle once.** |
| Nothing enumerates over USB | Must go through the **powered hub**; the Mac's port alone won't power it. |
| A tube stays dark | Empty/dead slot (e.g. tube 1's torn flex) — expected; not a bug. |
| `esptube.local` doesn't resolve | Use the IP; mDNS is flaky on some networks/VLANs. |
| Big flash read fails | Known — the USB link is fine for ~1 MB writes, not 16 MB reads. |

## 11. The story (for the blog)

It *looked* like an EleksTube "IPSTube" clone, so the first firmware used direct-GPIO chip-select —
and drew nothing but backlight. Five iterations of correct-looking code later, a top-down photo
showed the silkscreen: **SI HAI IPS CLOCK**, which selects its panels through a 74HC595 shift
register on entirely different pins. One variant switch and the glass lit up. The read-back
auto-detect we tried afterward proved the panels don't support SPI reads at all — a fine dead end
to document. Full arc + photos are in `images/` and the `docs/` history.

## 13. Bluetooth — investigated (2026-09-17)

The ESP32-WROOM-32D has BT Classic + BLE. Measured on this firmware (`pio run -e esptube_bleprobe`,
NimBLE-Arduino 2.x with a Nordic-UART service compiled in): **+241 KB flash, +9.7 KB static RAM**
(1.49 → 1.73 MB, 82 % of the 2 MB OTA slot; ~30 KB more heap while a client is connected). It fits
without touching the partition table. Options, ranked:

1. **BLE "Nordic UART" shell + Web Bluetooth** — the same text shell the cable and WebSocket carry,
   over BLE; the helper gets a 🦷 Connect button (Chrome/Edge Web Bluetooth) and any NUS terminal app on
   a phone works too. No WiFi, no cable, no pairing UI. This is the useful one: WiFi setup and status
   from a phone anywhere. ~2 days of firmware + helper work.
2. **BLE WiFi provisioning (Espressif `WiFiProv`)** — Espressif's phone apps configure WiFi over BLE.
   Cheaper to add but locks the flow to their apps; (1) covers it with our own UI.
3. **BT Classic SPP (`BluetoothSerial`)** — a wireless serial port the Mac could pair; the existing
   🧷 Web Serial panel would work unchanged. But bluedroid costs ~+550 KB flash and ~+50 KB RAM,
   which does not fit the 2 MB slot, and coexistence with the WiFi stream channel is poor. No.
4. **Streaming frames over BLE** — ~10–20 KB/s at best on this part: stills only, never video. No.

Not started; BLE would also need the stream task's RAM budget re-checked under load.

## 12. Publishing (maintainer note)

The GitHub repo is a **scrubbed mirror** of a private working copy: the working copy also holds
local network details, a hand-off file, and photos with the house in frame. Nothing is mirrored
automatically. The working copy carries `tools/sync-public.sh` (not itself mirrored), which copies the
committed tree into the public checkout, skips the paths in `tools/public-exclude.txt`, replaces the
LAN IP / SSID / MAC with `<clock-ip>` / `YOUR_WIFI_SSID` / `<your-device-mac>` in text files, strips
GPS EXIF from photos, removes files that were dropped or excluded, and refuses to commit if a leak
guard matches. `tools/sync-public.sh` stages only; `-m "message"` commits, `--push` pushes.

If you fork this repo you don't need any of that — everything here builds and runs as-is.

## 14. Opening the console — and why camera / screen capture need `localhost`

Browsers only define `navigator.mediaDevices` (camera, screen and tab capture) on a **secure page**:
`file://`, `https://` or `localhost`. The copy the clock hosts (`http://esptube.local/helper`) is plain
http on a LAN name — fine for everything else, but capture simply does not exist there.

```bash
python3 tools/console.py <clock-ip>     # serves tools/helper.html + samples/ at http://localhost:8765 and opens it
```
Standard library only; nothing else in the working copy is served. It is also **one stable origin**, so
scenes, the panel arrangement and the layout live in one place instead of depending on which folder the
file was opened from. `bridge.py serve` starts the same console. On the clock-hosted page 📡 Stream says
exactly this and links to the localhost console instead of failing.

## 15. The cable is a first-class link

| | |
|---|---|
| Bridge chip | CH340 (USB 1a86:7523) |
| Verified ladder | **1.5 M** → 1 M → 460800 → 230400 → 115200 (each rung: `help` + three clean `status` JSON replies; 921600 fails; 2 M garbles long replies) |
| Speed (ESP/1.1 datagrams) | 1.5 M: 119 KB/s, 14 tile-updates/s, ping 3 ms · bridge `chars:` 15–18 updates/s |
| Policy | commands over the cable whenever it is connected; pixels follow at ≥ 1 Mbaud; *USB only · WiFi only · both* override; Live can change link mid-stream |
| Shell | `uart` (counters: `dg crc_bad resync junk overrun fifo`) · `uart fifo <n>` · `baud N [once]` |

The helper's ⚡ Fast link and the bridge's `open_serial()` walk the same ladder. `--transport auto` in
the bridge takes a free cable before WiFi.

## 16. 🧊 Stage — the layout

`state.layout` (helper) = `esptube.layout/1`: millimetres, +x right, +y up, +z toward the viewer; `pos`
is the centre of the active area (14.864 × 24.912 mm for the 135 × 240 panel), `rot` = XYZ degrees. A
uniform row is one generator; a moved display makes the list explicit; a missing display is a hole. The
tube pitch is an **estimate (35 mm) until calibrated**: 🎯 Calibrate draws diagonals + a 10 mm ruler across
the physical wall — nudge until the lines run straight through the gaps — or type a measured value.
🖼️ Spanning → Geometry → *physical* then cuts spanning content by the real positions. Export:
`layout.json`, `.usda`, `.usdz` (Blender: File → Import → USD; iPhone/Finder: Quick Look).

### 16a. The known physical layout, and one layout for every page
`Layout.MODELS['sihai-6']` is the measured model of this clock: base 216 × 63.5 mm (8½″ × 2½″), six tubes at a 30.5 mm pitch
(±1 mm, derived from the same measurement), screen centres 27.5 mm above the base. **↺ measured layout** in the 🧊 Stage
toolbar returns to it; dragging a tube or typing x / y / z in the inspector departs from it (the HUD says *edited*).
The layout lives **on the clock** (`/layout.json`, with a `rev` timestamp): every page — the local file, the localhost console,
the clock-hosted copy, a phone — loads it on connect and saves its edits back within ~1 s; the newest edit wins. Before this,
each page kept its own copy in browser storage and two pages could show two different arrangements.
