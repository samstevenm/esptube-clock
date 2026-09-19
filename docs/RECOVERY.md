# Recovery / bootstrap guide

If the clock loses WiFi, gets a bad config, or won't boot — here's how to get it back, easiest
first. Nothing here needs the full PlatformIO toolchain.

## 1. It lost WiFi (wrong password, network changed) — no flashing
The firmware auto-falls back: if it can't join, it opens its own WiFi network **`esptube-setup`**.
- Join `esptube-setup`, open `http://192.168.4.1`, enter your 2.4 GHz WiFi + password. Done.
- (WiFi creds live in the ESP32's NVS, not in any file.)

## 2. It won't boot / behaves badly — reflash, no toolchain: **web flasher**
A Chrome/Edge web flasher (ESP Web Tools + WebSerial) is hosted on the repo's GitHub Pages:

> **`https://samstevenm.github.io/esptube-clock/webflasher/`**  ← (live once Pages is enabled)

1. **Chrome or Edge on desktop** (WebSerial isn't in Safari/Firefox/mobile).
2. Plug the clock in over USB **through the powered hub** (it won't enumerate directly into a Mac).
3. Click **Install** → pick the port → confirm. It erases + flashes the whole device (`firmware.merged.bin`).
4. Re-provision WiFi via the `esptube-setup` portal (step 1).

The board auto-resets, so no button-holding is needed. If ESP Web Tools can't connect, unplug/replug
and retry, or use the USB path below.

## 3. Reflash from this repo over USB (developer path)
With PlatformIO + esptool installed:
```bash
tools/flash.sh           # builds firmware/custom-fw and flashes via USB
```
Or flash the prebuilt merged image directly with esptool:
```bash
esptool --chip esp32 --port /dev/cu.usbserial-XXXX --baud 115200 \
  --before default_reset --after hard_reset write_flash 0x0 webflasher/firmware.merged.bin
```
After the first USB flash, updates can go **over WiFi** (no cable): `POST /ota` a new `firmware.bin`,
or `pio run -e esptube -t upload --upload-port esptube.local`.

## 4. Restore the original stock firmware
A partial stock backup exists (`firmware/stock-backup*.bin`, kept locally — not in this repo).
```bash
esptool --chip esp32 --port /dev/cu.usbserial-XXXX --baud 115200 write_flash 0x0 stock-backup-16MB.bin
```
Note the backup is ~14/16 MB (the flaky USB link couldn't finish the last 2 MB — see LEARNINGS),
so a stock restore may be missing some digit-face assets. Our firmware is the recommended target.

## Rebuilding the web-flasher image
`webflasher/firmware.merged.bin` is `bootloader + partitions + boot_app0 + app` merged for offset 0:
```bash
D=/path/to/build/esptube    # e.g. the PlatformIO build_dir
BA=$(ls ~/.platformio/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin | head -1)
esptool --chip esp32 merge-bin -o webflasher/firmware.merged.bin \
  --flash_mode dio --flash_freq 40m --flash_size 16MB \
  0x1000 "$D/bootloader.bin" 0x8000 "$D/partitions.bin" 0xe000 "$BA" 0x10000 "$D/firmware.bin"
```
Then commit the new `firmware.merged.bin` (and bump `webflasher/manifest.json` version).
