#!/usr/bin/env bash
# Serial console for the tube clock. Run in a real terminal.
# Usage: ./monitor.sh [serial-port]   (default: the powered-hub port)
# Exit the monitor with Ctrl-C.  Baud is 115200.
set -euo pipefail
PORT="${1:-/dev/cu.usbserial-1220}"
PIO="$(command -v pio || echo /opt/homebrew/bin/pio)"
echo "Serial console on $PORT @115200 (Ctrl-C to quit)…"
exec "$PIO" device monitor -p "$PORT" -b 115200 --quiet
