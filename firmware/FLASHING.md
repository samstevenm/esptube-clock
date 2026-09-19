# Flashing reference

**Port:** `/dev/cu.usbserial-1220` (via powered hub). **Only flash over a solid USB link** —
a dropped byte mid-write can corrupt the image. Finish the full stock backup first.

## Path A — EleksTubeHAX smoke test (ready)
Built in `~/Developer/esptube-clock/EleksTubeHAX/.pio/build/IPSTube/`. App fits 49% of the
2 MB slot; LittleFS image is the full 13.3 MB. Flash **these four only** — omit the framework's
`boot_app0.bin` (it would land at 0xe000, inside NVS, and this table has no OTA slot so it's
useless):

```bash
PORT=/dev/cu.usbserial-1220
D=~/Developer/esptube-clock/EleksTubeHAX/.pio/build/IPSTube
esptool --chip esp32 --port "$PORT" --baud 460800 \
  --before default_reset --after hard_reset \
  write_flash --flash_mode dio --flash_freq 40m --flash_size 16MB \
  0x1000   "$D/bootloader.bin" \
  0x8000   "$D/partitions.bin" \
  0x10000  "$D/IPSTube_v1.3.13.bin" \
  0x210000 "$D/littlefs.bin"
```
(If the link is marginal, drop `--baud` to 115200. Our board auto-resets, so no BOOT-hold needed —
but if esptool can't sync, hold the BOOT button while it connects.)

**Success looks like:** 5 of 6 tubes show digits (tube 5 is dead — torn flex — and stays dark;
that's expected and fine). If tubes are blank/garbled, the pin/variant guess is wrong → we
revisit IPSTube vs MarvelTubes Gen2 (check PCB: button count, audio codec, RTC chip).

## Restore stock (safety net)
Once the full 16 MB backup exists:
```bash
esptool --chip esp32 --port "$PORT" --baud 460800 write_flash 0x0 \
  "/Users/sm/Library/Mobile Documents/com~apple~CloudDocs/Household/1- Projects/esptube-clock/firmware/stock-backup-16MB.bin"
```

## Path B — custom REST firmware (built)
Project: `~/Developer/esptube-clock/custom-fw` (env `esptube`). App `firmware.bin` ≈ 909 KiB.
Partition table **has otadata + app0/app1** (real OTA) + ~11.9 MB LittleFS — so unlike Path A,
`boot_app0.bin` at 0xe000 IS wanted here. Simplest safe flash (PlatformIO handles offsets +
boot_app0 correctly because this table has otadata):
```bash
cd ~/Developer/esptube-clock/custom-fw && pio run -e esptube -t upload --upload-port /dev/cu.usbserial-1220
```
Set real WiFi creds first: fill `include/secrets.h` (resolve the password via `op`, don't commit).
After this first USB flash, future updates go over WiFi: `POST /ota` with a new `firmware.bin`,
or `pio run -e esptube -t upload` over ArduinoOTA (`esptube.local`). No more USB needed.

Endpoints once running (base `http://esptube.local`): `GET /status`, `GET /health`,
`POST /tube/{0-5}/text|image|rgb`, `POST /tubes/clear`, `POST /button/{mode|left|right|power}`,
`POST /ota`. Full curl examples in the project `README.md`.
