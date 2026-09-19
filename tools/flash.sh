#!/usr/bin/env bash
# Build the custom firmware and flash it to the clock over USB.
#
# Usage: ./flash.sh [serial-port]
#   default port: /dev/cu.usbserial-1220  (the clock enumerates ONLY through the
#   powered USB hub, not plugged straight into the Mac).
#
# After the first USB flash you can update over WiFi instead (no cable):
#   pio run -d ../firmware/custom-fw -e esptube -t upload --upload-port esptube.local
# or POST a new firmware.bin to  http://<clock-ip>/ota
set -euo pipefail

PORT="${1:-/dev/cu.usbserial-1220}"
HERE="$(cd "$(dirname "$0")" && pwd)"
PROJDIR="$(cd "$HERE/../firmware/custom-fw" && pwd)"
BUILD=/Users/sm/Developer/esptube-build/esptube          # matches platformio.ini build_dir
PIO="$(command -v pio || echo /opt/homebrew/bin/pio)"
ESPTOOL="$(command -v esptool || echo /opt/homebrew/bin/esptool)"
BOOTAPP0="$(ls "$HOME"/.platformio/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin | head -1)"

[ -e "$PORT" ] || { echo "!! Serial port $PORT not found. Is the clock on the powered hub? (ls /dev/cu.*)"; exit 1; }

echo "==> Building firmware from $PROJDIR"
"$PIO" run -d "$PROJDIR" -e esptube

echo "==> Flashing over $PORT (replaces the running firmware; ~1 min, verified per block)"
"$ESPTOOL" --chip esp32 --port "$PORT" --baud 115200 --before default_reset --after hard_reset \
  write_flash --flash_mode dio --flash_freq 40m --flash_size 16MB \
  0x1000  "$BUILD/bootloader.bin" \
  0x8000  "$BUILD/partitions.bin" \
  0xe000  "$BOOTAPP0" \
  0x10000 "$BUILD/firmware.bin"

echo "==> Done — the clock is rebooting into the new firmware."
echo "    Watch it:  ./monitor.sh"
echo "    Drive it:  ./esptube status   (set ESPTUBE_IP or use esptube.local)"
