#!/usr/bin/env bash
# Weather-provider smoke: every host the helper's weather ladder can use must resolve, answer
# HTTP 200 with CORS "*" (the helper fetches from the browser), and return a parseable value.
# A sinkholed host (0.0.0.0 from a DNS ad-blocker) is reported, not fatal — that is exactly what
# the ladder exists for — but at least one weather provider AND one geocoder must pass.
#   test/weather-providers.sh [lat lon]        (default: San Francisco)
set -u
LAT=${1:-37.77}; LON=${2:-"-122.42"}
UA="Mozilla/5.0 esptube-helper-smoke (github.com/samstevenm/esptube-clock)"
wx_ok=0; geo_ok=0; fail=0
check() { # name url python-expr(reads j) kind
  local name=$1 url=$2 expr=$3 kind=$4 host ip hdr code cors body val
  host=$(sed -E 's#^https?://([^/]+).*#\1#' <<<"$url"); ip=$(dig +short "$host" 2>/dev/null | tail -1)
  if [ "$ip" = "0.0.0.0" ] || [ -z "$ip" ]; then printf '  ⚠ %-12s %s → DNS %s (sinkholed / unresolved)\n' "$name" "$host" "${ip:-none}"; return; fi
  hdr=$(mktemp); body=$(curl -s -m 15 -D "$hdr" -H "Origin: http://esptube.local" -H "User-Agent: $UA" "$url")
  code=$(awk 'NR==1{print $2}' "$hdr"); cors=$(grep -i '^access-control-allow-origin' "$hdr" | tr -d '\r' | awk '{print $2}'); rm -f "$hdr"
  val=$(python3 -c "import sys,json; j=json.load(sys.stdin); print($expr)" <<<"$body" 2>/dev/null)
  if [ "$code" = "200" ] && [ "$cors" = "*" ] && [ -n "$val" ]; then printf '  ✓ %-12s %s → %s\n' "$name" "$host" "$val"; [ "$kind" = wx ] && wx_ok=1 || geo_ok=1
  else printf '  ✗ %-12s %s → HTTP %s cors=%s val=%s\n' "$name" "$host" "${code:-none}" "${cors:-none}" "${val:-none}"; fail=1; fi; }
echo "weather providers ($LAT,$LON):"
check open-meteo "https://api.open-meteo.com/v1/forecast?latitude=$LAT&longitude=$LON&current=temperature_2m,weather_code&temperature_unit=fahrenheit" \
  "str(j['current']['temperature_2m'])+'°F wmo '+str(j['current']['weather_code'])" wx
check met.no "https://api.met.no/weatherapi/locationforecast/2.0/compact?lat=$LAT&lon=$LON" \
  "str(j['properties']['timeseries'][0]['data']['instant']['details']['air_temperature'])+'°C '+j['properties']['timeseries'][0]['data']['next_1_hours']['summary']['symbol_code']" wx
check wttr.in "https://wttr.in/$LAT,$LON?format=j1" "j['current_condition'][0]['temp_F']+'°F wwo '+j['current_condition'][0]['weatherCode']" wx
echo "geocoders (Reykjavik):"
check open-meteo "https://geocoding-api.open-meteo.com/v1/search?name=Reykjavik&count=1" "str(j['results'][0]['latitude'])+','+str(j['results'][0]['longitude'])" geo
check nominatim "https://nominatim.openstreetmap.org/search?q=Reykjavik&format=json&limit=1" "j[0]['lat']+','+j[0]['lon']" geo
check wttr.in "https://wttr.in/Reykjavik?format=j1" "j['nearest_area'][0]['latitude']+','+j['nearest_area'][0]['longitude']" geo
if [ $wx_ok = 1 ] && [ $geo_ok = 1 ]; then echo "PASS: at least one weather provider and one geocoder answer with CORS *"; exit 0; fi
echo "FAIL: no working weather provider or no working geocoder"; exit 1
