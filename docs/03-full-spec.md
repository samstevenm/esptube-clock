# 03 — Full Spec Breakdown

_Confidence tags: **[confirmed]** = read from the device/flash this session · **[likely]** =
strong inference, not yet device-verified · **[TBD]** = still to confirm (how noted)._

---

## 1. Compute — ESP32-WROOM-32D module

| Spec | Value | Source |
|---|---|---|
| SoC | **ESP32-D0WD rev v1.0** | [confirmed] esptool |
| Cores | Dual-core Xtensa LX6, up to **240 MHz** | [confirmed] |
| SRAM | **520 KB** on-chip | [confirmed] datasheet for D0WD |
| PSRAM | **None** (WROOM-32D has no PSRAM) | [likely] — matters: no big external RAM for full-frame buffers |
| WiFi | **2.4 GHz** 802.11 b/g/n (no 5 GHz) | [confirmed] |
| Bluetooth | v4.2 BR/EDR + BLE (unused by clock) | [confirmed] chip |
| Flash | **16 MB** (mfr 0x20, dev 0x4018), 3.3 V | [confirmed] esptool |
| WiFi MAC | `<your-device-mac>` | [confirmed] |

**Implication:** 520 KB SRAM, no PSRAM. A single full-color frame at ~240×240×2 bytes ≈
115 KB, so we can buffer **one panel at a time** comfortably but not six simultaneously in
RAM — we render per-tube sequentially (which is exactly how the stock bmp pipeline works).

## 2. Storage — 16 MB flash layout **[confirmed, parsed from live flash]**

| Partition | Type | Offset | Size | Purpose |
|---|---|---|---|---|
| (bootloader) | — | 0x1000 | ~28 KB | 2nd-stage bootloader |
| (partition table) | — | 0x8000 | 3 KB | this table |
| `nvs` | data/nvs | 0x9000 | 20 KB | key-value store: WiFi creds, settings |
| `otadata` | data/ota | 0xe000 | 8 KB | tracks active OTA slot |
| `app0` | app/ota_0 | 0x10000 | **6 MB** | firmware slot A (active) |
| `app1` | app/ota_1 | 0x610000 | **6 MB** | firmware slot B (OTA target) |
| `spiffs` | data/spiffs | 0xcb0000 | **3.31 MB** | **digit images** (`/0.bmp`…`/15.bmp`) + assets |
| (unallocated) | — | 0xf60000 | ~640 KB | free tail |

- **Two 6 MB app slots** = A/B OTA updates are supported (belt-and-suspenders: a bad flash
  can roll back). 6 MB per slot is very generous; our custom app will be a fraction of that.
- **3.31 MB SPIFFS** currently holds the digit face BMPs. This is the image budget on the
  *stock* scheme.
- **We control the partition map.** With our own firmware we can repartition the 16 MB freely
  — e.g. 2 MB app + a single big **LittleFS (~12 MB)** for many image/widget assets, or keep
  A/B OTA + a larger data FS. Nothing about the hardware limits us to the stock split.

## 3. Stock firmware **[confirmed from flash]**

- Framework: **Arduino-ESP32**, built on **ESP-IDF v3.3.4** (~Mar 2021). Old but fine.
- Digit rendering: loads **`/<n>.bmp` from SPIFFS** and blits to each panel. Files `0.bmp`–
  `9.bmp` are digits; `10.bmp`–`15.bmp` are extra faces/glyphs. → the per-tube image path we reuse.
- Time: **NTP**, hard-coded `ntp1.aliyun.com` (China). We'll repoint to `pool.ntp.org` / a
  local source in custom firmware.
- Network setup: **WiFiManager** captive portal at `192.168.4.1` (AP `ESP32TUBE`), plus an
  **OTA "Update"** page → firmware can be reflashed over WiFi as well as USB.

## 4. Displays / tubes

| Spec | Value | Source |
|---|---|---|
| Count | **6** identical modules | [confirmed] |
| Type | Small **IPS TFT LCD**, color, on a per-tube carrier PCB with an **FPC** flex tail | [confirmed] photos |
| Controller | **ST7789** SPI (IPSTube variant match) | [likely→confirm at first flash] |
| Resolution | **135 × 240**, portrait, RGB565 | [likely] from IPSTube config; cross-check vs stock BMP |
| Bus | Shared SPI (MOSI=32, SCLK=33, DC=25, RST=26), **per-tube direct-GPIO CS** = {15,2,27,14,12,13} active-LOW | see `hardware/pinmap.md` |
| Per-tube RGB | Each carrier has its own **addressable RGB LED** (WS2812-class), giving the underglow | [confirmed] photo; [TBD] exact count/type |
| Tube cover | Clear **glass dome** over each module | [confirmed] |

**Note:** the digit BMPs are numbered, so the display just needs a bitmap of the right
dimensions — perfect for pushing arbitrary content (a value, an icon, a mini-widget).

## 5. Power & I/O

- **DC barrel jack** on the base (red "DC-X1" connector) — primary power [confirmed photo].
  Voltage [TBD] (likely 5 V; confirm from the barrel/label before powering from anything else).
- **USB** present; enumerated as a serial bridge **only via a powered hub** (`/dev/cu.usbserial-1220`),
  not direct-to-MacBook. Auto-reset (DTR/RTS) works, so flashing needs no button-holding.
- **Buttons:** physical tactile switches on the board (≥2 seen in teardown; the stock menu
  cycles 5 SET functions: Style / LCD Bright / RGB Bright / RGB Style / OTHER). Exact button
  count + GPIOs [TBD].
- **RTC backup:** CR2032 coin cell keeps time across power loss [confirmed photo].

## 6. What's broken right now

| # | Item | State | Plan |
|---|---|---|---|
| 1 | **Stock firmware** | "doesn't work" per owner; old 2021 Arduino build | Replace with custom firmware (backup taken first) |
| 2 | **Tube 5 display** | **Dead** — FPC **ribbon cable torn/kinked** (not the panel logic) | Swap display module (or just the flex); confirm exact panel first |
| 3 | **Tube 4 glass dome** | **Shattered** (cosmetic) — the display under it still works | Order a replacement glass dome |
| 4 | **USB direct connection** | Doesn't enumerate direct to MBA | Workaround confirmed: **powered USB hub** |

## 7. Fit against the project goals

- **"Simple interface to control it / feed arbitrary info / show images"** → very feasible.
  The stock `/N.bmp`→panel pipeline proves the path. Custom firmware exposes an HTTP/WebSocket
  (and/or MQTT) API; a script pushes a value or image per tube; firmware blits it. Graceful
  degradation: skip tubes marked dead (e.g. tube 5) and drive the rest.
- **"Virtual buttons"** → firmware maps network commands to the same internal events the
  physical buttons fire.
- **Storage for images** → 3.3 MB today; repartitionable to ~12 MB LittleFS if we want a big
  local asset library. Or stream images over the network on demand (no local storage needed).
- **Expansion** → spare GPIOs, I2C/SPI headroom, BT unused; realistic add-ons later (sensor,
  gesture, etc.) but not needed for the core goals.

## 8. Still to confirm (and how)
1. **Exact display resolution + controller** → parse a digit BMP from the SPIFFS partition
   (needs the full 16 MB backup, which is completing now), and/or a quick probe sketch.
2. **Display select scheme + button/RGB GPIOs** → cross-reference EleksTubeHAX variant configs
   and/or continuity-probe the carrier connector.
3. **DC input voltage** → read the barrel/PSU label before powering externally.
4. **USB-UART bridge chip** (CP2102 vs CH340) → `ioreg`/`system_profiler` VID:PID.
