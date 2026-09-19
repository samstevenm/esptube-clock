#!/usr/bin/env bash
# REST integration smoke test for the ESPTube clock firmware.
# Hits every endpoint against a live clock and asserts the response.
# Usage: test/rest-smoke.sh [clock-ip]   (default: $ESPTUBE_IP or esptube.local)
set -u
IP="${1:-${ESPTUBE_IP:-esptube.local}}"
BASE="http://$IP"
pass=0; fail=0
chk(){ # desc  expected-substring  curl-args...
  local desc="$1" exp="$2"; shift 2
  local out; out=$(curl -sS -m8 "$@" 2>&1)
  if printf '%s' "$out" | grep -qF "$exp"; then printf '  \033[32mPASS\033[0m  %s\n' "$desc"; pass=$((pass+1))
  else printf '  \033[31mFAIL\033[0m  %s (want "%s", got: %s)\n' "$desc" "$exp" "${out:0:140}"; fail=$((fail+1)); fi
}
echo "== ESPTube REST smoke test @ $BASE =="

# --- reads ---
chk "GET /health"            '"ok":true'       "$BASE/health"
chk "GET /status has ip"     '"ip"'            "$BASE/status"
chk "GET /status has mode"   '"mode"'          "$BASE/status"
chk "GET /status per-tube"   '"populated"'     "$BASE/status"
chk "GET /config"            'populated_mask'  "$BASE/config"
chk "GET / (fallback UI)"    'ESPTube'         "$BASE/"

# --- display content (switches to manual) ---
chk "POST /mode/manual"      '"mode":"manual"' -X POST "$BASE/mode/manual"
chk "POST /tube/2/text"      '"ok":true'       -X POST --data 'T' "$BASE/tube/2/text"
chk "POST /tube/2/rgb"       '"r":255'         -X POST -H 'Content-Type: application/json' --data '{"r":255,"g":0,"b":0}' "$BASE/tube/2/rgb"

# --- raw565 image (memory-safe path); 64800-byte solid green frame ---
TMP="$(mktemp)"; python3 -c "import sys;sys.stdout.buffer.write(bytes([0x07,0xE0])*(135*240))" > "$TMP"
chk "POST /tube/2/raw565"    '"bytes":64800'   -X POST -F "file=@$TMP;type=application/octet-stream" "$BASE/tube/2/raw565"
rm -f "$TMP"
# --- region blit: a 40x20 red rect at (20,30); and one that clamps at the right edge ---
TMP="$(mktemp)"; python3 -c "import sys;sys.stdout.buffer.write(bytes([0xF8,0x00])*(40*20))" > "$TMP"
chk "POST /tube/2/blit 40x20"  '"bytes":1600'   -X POST -F "file=@$TMP;type=application/octet-stream" "$BASE/tube/2/blit?x=20&y=30&w=40&h=20"
chk "POST /tube/2/blit clamped" '"ok":true'     -X POST -F "file=@$TMP;type=application/octet-stream" "$BASE/tube/2/blit?x=120&y=200&w=40&h=20"
chk "POST /tube/2/blit bad w"   '"ok":false'    -X POST -F "file=@$TMP;type=application/octet-stream" "$BASE/tube/2/blit?x=0&y=0&w=0&h=20"
rm -f "$TMP"

# --- config + buttons ---
chk "POST /tubes/clear"      '"ok":true'       -X POST "$BASE/tubes/clear"
chk "POST /config/brightness" '"value":180'   -X POST -H 'Content-Type: application/json' --data '{"value":180}' "$BASE/config/brightness"
chk "POST /button/mode"      '"ok":true'       -X POST "$BASE/button/mode"

# --- LED-effect layer (independent; runs live over the display) ---
chk "POST /led/rainbow"          '"ok":true'                 -X POST "$BASE/led/rainbow"
chk "status led_effect=rainbow"  '"led_effect":"rainbow"'    "$BASE/status"
chk "POST /mode/clock"           '"mode":"clock"'            -X POST "$BASE/mode/clock"
chk "both layers live (clock+rainbow)" '"led_effect":"rainbow"' "$BASE/status"
chk "POST /led/solid {r,g,b}"    '"ok":true'                 -X POST -H 'Content-Type: application/json' --data '{"r":0,"g":128,"b":255}' "$BASE/led/solid"
chk "POST /led/off"              '"ok":true'                 -X POST "$BASE/led/off"

# --- device-native presets (short MODE / web cycle the same index) ---
chk "GET /status has preset"     '"preset"'                  "$BASE/status"
chk "POST /preset/1 (digital)"   '"preset_name":"digital"'   -X POST "$BASE/preset/1"
chk "POST /preset/next"          '"ok":true'                 -X POST "$BASE/preset/next"
chk "POST /preset/4 (date)"      '"preset_name":"date"'      -X POST "$BASE/preset/4"
chk "status mode=nixie (date)"   '"mode":"nixie"'            "$BASE/status"
chk "POST /preset/0 (nixie)"     '"preset_name":"nixie"'     -X POST "$BASE/preset/0"
chk "POST /preset/9 rejected"    '"ok":false'                -X POST "$BASE/preset/9"

# --- device-rendered nixie messages (no pixels pushed) ---
chk "POST /nixie/text static"     '"kind":"static"'    -X POST -H 'Content-Type: application/json' --data '{"text":"HELLO","effect":"static"}' "$BASE/nixie/text"
chk "status mode=nixie"           '"mode":"nixie"'     "$BASE/status"
chk "POST /nixie/text scroll"     '"kind":"scroll"'    -X POST -H 'Content-Type: application/json' --data '{"text":"HELLO WORLD 20°","effect":"scroll","ms":200}' "$BASE/nixie/text"
chk "POST /nixie/text bad effect" '"ok":false'         -X POST -H 'Content-Type: application/json' --data '{"text":"X","effect":"spin"}' "$BASE/nixie/text"
chk "POST /nixie/countdown"       '"kind":"countdown"' -X POST -H 'Content-Type: application/json' --data '{"seconds":5}' "$BASE/nixie/countdown"
# ---- v1.9.5: master clear, wifi, idle timer, hosted helper ----
chk "POST /tubes/clear (master)"  '"cleared":"all"'    -X POST "$BASE/tubes/clear"
chk "status: manual after clear"  '"mode":"manual"'    "$BASE/status"
chk "GET /wifi has saved list"    '"saved"'            "$BASE/wifi"
chk "POST /config/idle 30"        '"idle_min":30'      -X POST -H 'Content-Type: application/json' --data '{"minutes":30}' "$BASE/config/idle"
chk "status has idle_min"         '"idle_min"'         "$BASE/status"
# ---- v1.9.7: cheap ping, weak-signal warning threshold, smoothed rssi ----
chk "GET /ping"                   '"ok":true'          "$BASE/ping"
chk "POST /config/netwarn -80"    '"netwarn":-80'      -X POST -H 'Content-Type: application/json' --data '{"dbm":-80}' "$BASE/config/netwarn"
chk "POST /config/netwarn bad"    'dbm -95..-30'       -X POST -H 'Content-Type: application/json' --data '{"dbm":-20}' "$BASE/config/netwarn"
chk "POST /config/netwarn -78"    '"netwarn":-78'      -X POST -H 'Content-Type: application/json' --data '{"dbm":-78}' "$BASE/config/netwarn"
chk "status has netwarn+rssi_avg" '"rssi_avg"'         "$BASE/status"
chk "GET /wifi has warn_dbm"      '"warn_dbm"'         "$BASE/wifi"
chk "GET /helper (hosted, gzip)"  'serialPanel'        --compressed "$BASE/helper"
chk "GET /manifest.webmanifest"   '"start_url":"/helper"' "$BASE/manifest.webmanifest"
chk "GET /fs lists LittleFS"      '"files"'            "$BASE/fs"
chk "POST /nixie/stop"            '"mode":"clock"'     -X POST "$BASE/nixie/stop"

# --- clock-sweep test: every second from 23:59:50 to 00:00:05 through the real
#     render path (covers the 6-tube midnight rollover), with fades ---
chk "POST /test/clocksweep"       '"total":16'         -X POST -H 'Content-Type: application/json' --data '{"from":86390,"to":5,"step_ms":0,"fade":1}' "$BASE/test/clocksweep"
sleep 6
chk "sweep finished (16 steps)"   '"done":16'          "$BASE/status"
chk "sweep inactive"              '"active":false'     "$BASE/status"

# --- restore a sane state ---
curl -sS -m5 -X POST "$BASE/led/off"    >/dev/null 2>&1
curl -sS -m5 -X POST "$BASE/preset/0"   >/dev/null 2>&1
curl -sS -m5 -X POST "$BASE/mode/clock" >/dev/null 2>&1

echo "-------------------------------------------"
printf "PASS=%d  FAIL=%d\n" "$pass" "$fail"
[ "$fail" -eq 0 ]
