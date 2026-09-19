# Web control panel (on-device fallback) — spec

The clock serves its own small control page at **`http://esptube.local/`** (or its IP) — no app, no
hosting; it's embedded in the firmware and talks to the on-board REST API same-origin. It is the
**fallback**: the full console is [`tools/helper.html`](../tools/helper.html) (widgets, images,
animations, scenes, drag-across-tubes).

Source of truth: [`firmware/custom-fw/data/index.html`](../firmware/custom-fw/data/index.html)
(self-contained HTML/CSS/JS). The pre-build step `scripts/gen_web_index.py` embeds it as PROGMEM
(`src/web_index.h`) on every build — edit the file, rebuild, flash.

## Layout & controls

**Header** — title + a status pill polling `GET /status` every 5 s (IP · heap · NTP state).

**Top bar** — Clear all (`POST /tubes/clear`) · Refresh.

**Per-tube cards** (physical order left→right = index 5…0; unpopulated slots dimmed)
| Control | Endpoint |
|---|---|
| text + Draw (≤8 chars, built-in font) | `POST /tube/{i}/text` |
| colour + Glow | `POST /tube/{i}/rgb` `{r,g,b}` |
| Clear | text `""` + rgb `0,0,0` |

**Device face** — what the clock draws by itself; the physical MODE button cycles the same list.
| Buttons | Endpoint |
|---|---|
| 🕯️ Nixie · Digital · LED show · Off · 📅 Date · Next | `POST /preset/{0-4\|next}` |

**Nixie message** — the clock renders it in nixie letters (A–Z 0–9 `- : . ! ? ° % + /`); nothing pushed.
| Control | Endpoint |
|---|---|
| text · Static/Flash/Scroll · every N ms · Show | `POST /nixie/text {"text","effect","ms"}` |
| N s · Countdown | `POST /nixie/countdown {"seconds"}` |
| Back to clock | `POST /nixie/stop` |

**Display & underglow**
| Control | Endpoint |
|---|---|
| Panels: Clock / Off | `POST /mode/{clock\|off}` (`manual` is set automatically by any content push) |
| LED effect: Off · Solid (colour) · Rainbow · Breathe · Comet | `POST /led/{…}` (`solid` takes `{r,g,b}`); persisted |
| Brightness (0–255) — dims the nixie face and the underglow | `POST /config/brightness {"value"}`; persisted |

**Which tubes are populated** — checkbox per slot + Save layout → `POST /config/populated {"mask"}`
(bit i = slot i; the panels can't be auto-detected). Loaded from the device once, then not clobbered
by the poll while you edit.

**📹 Live** — the page captures, compresses and streams video itself over one WebSocket per tube
(`ws://<clock>:5557`, ESP/1 frames, PAL4x2 by default).
| Control | What it does |
|---|---|
| Source: 🎭 Characters · 📷 Camera · 🖥️ Screen/tab · 🎞️ Video/GIF file · ✨ Animation frame | Characters loads `samples/characters.js` (a plain script, so `file://` works) and plays one character per tube, cast shifting along every *rotate every* seconds; camera/screen ask Chrome for permission |
| Set (dwarves · island · winter) · rotate every N s | which cast, how often it shifts |
| Layout: Across all tubes / Same on every tube · Format · dither · fps · ▶ Go live | 16-colour median-cut + Bayer dither, changed-row bands only; auto fps follows the clock's acks |
| 🎭 Cast: load dwarves / island / winter / mix / auto · five slots · face palette (drag onto a slot or a tube card; click = next free tube; click a slot = clear it) | `state.cast[tube] = {set,id}` — cast tubes are fixed, the rest rotate through the family; persisted, in backups |
| 🎨 Look: background (black · colour · gradient · glow · studio frame) · colours · filter presets · ☀ ◐ 🎨 🌈 sliders · reset | drawn under / applied over every Live frame before quantisation (`state.look`, persisted) |
| 🧹 Clear (header) | **master clear**: stops Live, the bridge, the cycle and the pusher, then `POST /tubes/clear` |

**🔌 Wired / stream** — remote control of `tools/bridge/bridge.py serve` on this machine (always on the
page; while the bridge isn't answering it shows the three commands that start it and disables ▶).
| Control | Bridge API |
|---|---|
| Source menu (character sets · webcams · test) or a typed spec (`file:` `url:` `dir:` `page:` `screen:`) | `POST /bridge/start {source,…}` |
| cast checkboxes · rotate every N s (character sets) | `chars`, `rotate_every` in the same body |
| Layout · Format · fps · ▶ Stream · ⏹ Stop | `/bridge/start` · `/bridge/stop`; stats from `/bridge/status` every 5 s |

**🧷 USB serial terminal** — Web Serial (Chrome/Edge) to the clock's UART; no clock endpoint involved.
| Control | What it does |
|---|---|
| 🔌 Connect (port picker) · baud | opens the port (resets the clock — boot log first); 115200 default, remembered |
| 🧷 control over the cable: auto / always / never | auto: while the clock isn't answering over the network, `api()` sends REST calls as shell lines (`preset`, `mode`, `led`, `bright`, `time`, `nixie`/`flash`/`scroll`, `countdown`, `stop`, `sweep`, `reboot`, `wifi …`; `status`/`wifi json` replies are parsed) and pixel pushes / 📹 Live frames as ESP/1 frames with acks; switches back when the network answers |
| console · autoscroll · timestamps · 🧹 Clear · 💾 Save log | everything the firmware prints, optionally time-stamped, downloadable |
| command line (Enter, ↑/↓ history) · quick chips | sends shell lines: `help status preset nixie flash scroll countdown stop led bright mode wifi sweep fps baud token time rtc reboot` |
| 📶 Join a WiFi over the cable: SSID · password · Join & remember · Scan · Recovery hotspot | `wifi add "SSID" pass` · `wifi scan` · `wifi ap on` — the password goes to the clock only |

**📶 WiFi** (⚙️ Setup & advanced) — saved networks and the recovery hotspot, over REST.
| Control | Endpoint |
|---|---|
| status line (network · IP · RSSI · hotspot state) | `GET /wifi` |
| 🔍 Scan → list with signal bars, *use* (fills the form) / *join* (saved) | `POST /wifi/scan` · `POST /wifi/join {"ssid"}` |
| SSID · password · 📶 Join & remember | `POST /wifi/add {"ssid","pass"}` (saves, up to 8, joins now) |
| saved list: join · forget | `POST /wifi/join` · `POST /wifi/forget {"ssid"}` |
| 🛟 Hotspot on / off · 🔄 Reconnect | `POST /wifi/ap {"on"}` · `POST /wifi/reconnect` |
| 🕐 Set time from here (next to the timezone) | `POST /config/time {"epoch"}` — also writes the DS1302 RTC |

**📶 Signal** (⚙️ Setup & advanced) — is the path good enough to cast?
| Control | What it does |
|---|---|
| status line | the clock's own RSSI (bars · dBm · good/ok/poor/bad, from `/status` every 5 s) · last ⚡ result · last 🌐 result |
| ⚡ Test clock link | median of 5× `GET /ping`, then 3 RAW565 frames to the first working tube over the ESP/1 WebSocket (the cast path), one ack each → KB/s + ack ms; re-sends what the tube already shows, restores the clock face if it wasn't in manual; falls back to REST pushes when all 6 stream slots are busy |
| 🌐 Test internet | this browser's latency / download (6 s streamed) / upload (4 MB) via `speed.cloudflare.com` (CORS `*`; fast.com's API has no CORS header) |
| 📶 Show on tubes | every working tube → the 📶 Signal widget, pushed |
| flag weak WiFi at −70…−85 dBm | `state.sig` — four polls (~20 s) at or below the bar: one toast per 10 min + a 📶 badge on every frame this page pushes or streams until it recovers (3 dB hysteresis); the clock's own `wifi warn` covers the clock face |

**Widgets** also include **📶 Signal** (bars, dBm, ping/cast/ack, Mbps; auto-push 15 s–5 min) and the complications **📶 rssi · 🔗 link · 🌐 net**.
**🌤️ Weather** tries open-meteo → met.no → wttr.in (geocoding open-meteo → Nominatim → wttr.in, cached 30 d); the provider that answered is remembered 12 h and printed on the card.

**Served by the clock** — the same page at `http://esptube.local/helper` (or `192.168.4.1/helper` on
the hotspot): `SERVED_BY_CLOCK` makes the clock the default target (same origin), the PWA manifest +
`apple-mobile-web-app-*` meta let a phone *Add to Home Screen*, and 🎭 Characters load
`/samples/characters.js` from the clock's LittleFS once uploaded (`POST /fs/put/<path>`).

## Behind the scenes
- Same-origin fetches; `Access-Control-Allow-Origin: *` is also set for external tools (the helper).
- `GET /status` reports `mode`, `preset`, `led_effect`, `brightness`, `tz`, `time_valid`, per-tube
  content + LED, `draw_ms`, `reset_reason`, `nixie` (when a message shows) and `sweep` (test progress).
- Physical buttons: MODE = next face (long-press = on-device menu) · UP/DOWN = brightness · POWER = on/off.
  `POST /button/{mode|up|down|power}` runs the identical handler.

## Helper shell (tools/helper.html, v2.0)

**Command bar** (sticky): target · ⏸ Pause · **🧹 Stop & clear** (Esc — the one stop: Live, bridge, cycle, animation, then `Outbox.supersede({clear})`) · 🕯️ Clock face · ⌘K · chips: who paints the tubes · which link carries pixels (USB baud / WiFi RSSI / KB/s).

**🧲 Dock**: every section is a panel (`data-panel`: tubes · stage · clock · scenes · span · anim · live · bridge · serial · tools). Drag by the title (pointer events), snap to the dashed placeholder, ↔ half/full, ▾ collapse, ✕ hide (restore from the bar or ⌘K). Presets *everyday · cast · setup · dev*; yours = *custom*; saved in `state.dock`.

**Keys**: Esc stop & clear · ⌘↵ push · P pause · 1–6 select tube · [ ] previous / next scene · ⌘K palette.

**The pipeline**: `pushFrame` / `pushRaw` → `Outbox.submit(tube, raw565)` → dirty 16-row blocks → RLE565 or RAW565 → link: cable datagrams · shared WebSocket · REST. `Outbox.supersede()` on stop / clear / pause / Live stop.

**🧷 link**: auto (cable first) · USB only · both links (split the tubes) · WiFi only.

**🧊 Stage**: views · show (on the clock now / my drafts) · pitch · snap · 🎯 Calibrate · 💾 to the clock · ⤓ from the clock · ⬇ layout.json / .usda / .usdz · inspector (x y z rx ry rz).

**📡 Stream**: on an insecure page (the clock-hosted copy) camera / screen sources show why they are off and link to `http://localhost:8765`.

**🔁 Reboot** — command bar (helper) and top bar (on-device page): two clicks (arm → "Sure? click again" for 3 s → go), `reboot` over the cable when it is connected else `POST /reboot`, then polls `GET /ping` and reports "the clock is back (N s)". Also in ⌘K. 🧷 Connect reuses a cable the browser already has permission for (`navigator.serial.getPorts()`), so the picker only appears the first time.
