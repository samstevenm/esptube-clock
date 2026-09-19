#!/usr/bin/env bash
# Drive every valid time (or a range) through the clock's REAL nixie render path and
# watch it finish. The firmware steps the simulated time as fast as the fades allow
# (or paced by STEP_MS), draws exactly what the live clock would, and reports progress
# in /status.sweep. Watch the glass while it runs: any stuck/mixed glyph is a hardware
# transaction miss (the logic itself is covered by nixie-native.sh).
#
# Usage: test/clock-sweep.sh [clock-ip] [from-sec] [to-sec] [fade 0|1] [step_ms]
#   default: whole day 0..86399, fade=0, step 0 — ~8 min on the 5-tube layout (the empty
#   seconds-ones slot means 9 of 10 steps draw nothing); ~30 min with all 6 tubes; fade=1 ≈ 4x.
#   e.g. midnight slice with fades:  test/clock-sweep.sh esptube.local 86390 5 1
set -u
IP="${1:-${ESPTUBE_IP:-esptube.local}}"; FROM="${2:-0}"; TO="${3:-86399}"; FADE="${4:-0}"; STEP="${5:-0}"
BASE="http://$IP"
r=$(curl -sS -m8 -X POST -H 'Content-Type: application/json' \
      --data "{\"from\":$FROM,\"to\":$TO,\"step_ms\":$STEP,\"fade\":$FADE}" "$BASE/test/clocksweep") || { echo "start failed"; exit 1; }
echo "started: $r"
want=$(printf '%s' "$r" | python3 -c 'import sys,json; print(json.load(sys.stdin)["sweep"]["total"])')
t0=$(date +%s); last=-1; lastup=0; done=0; total=$want; maxms=0; heap=0; reason=""
while :; do
  sleep 5
  s=$(curl -sS -m8 "$BASE/status") || { echo; echo "!! status failed (device offline?)"; continue; }
  read -r act done total maxms heap reason up <<<"$(printf '%s' "$s" | python3 -c 'import sys,json; d=json.load(sys.stdin); w=d["sweep"]; print(w["active"], w["done"], w["total"], w["max_draw_ms"], d["free_heap"], d.get("reset_reason",""), d.get("uptime_ms",0))')"
  up=$((up/1000))
  printf '\r  %s/%s steps · max draw %s ms · heap %s · up %ss   ' "$done" "$total" "$maxms" "$heap" "$up"
  if [ "$up" -lt "$lastup" ]; then echo; echo "FAIL: the clock REBOOTED during the sweep at ~$last/$want steps (reset_reason=$reason)"; exit 2; fi
  lastup=$up
  if [ "$act" = "False" ]; then echo; break; fi
  if [ "$done" = "$last" ]; then echo; echo "!! no progress in 5 s"; fi
  last=$done
done
el=$(( $(date +%s) - t0 ))
if [ "$total" = "$want" ] && [ "$done" = "$total" ] && [ "$total" -gt 0 ]; then
  echo "PASS: $done/$total times rendered in ${el}s, slowest step ${maxms} ms, heap ${heap}"; exit 0
else echo "FAIL: ended at $done/$total of $want wanted (reset_reason=$reason)"; exit 1; fi
