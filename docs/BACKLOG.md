# Backlog / follow-up

Running list of everything requested for this project, so nothing gets lost between sessions.
**Done** items are shipped + in git; **Open** items are queued.

Legend: ✅ done · 🔜 next · ⬜ queued · 🔬 needs hardware/observation

## Open (queued)

### Device / firmware
- ✅ **Device-native presets (v1) — short MODE + web cycle them; NVS-persisted.** 0 = Wide Nixie
  clock (default) · 1 = Big digital · 2 = LED show (clock+comet) · 3 = Off. `POST /preset/next|{i}`,
  `/status.preset`. Helper has a 🎭 Device-face selector that drives + mirrors it. Flashed, 24/24 smoke.
- ✅ **Real nixie face (2026-09-17).** Pre-rendered cathode-wire plates (Nixie One, OFL) with bloom,
  stacked ghost cathodes and glass vignette, RLE in flash (~132 KB); brightness-scaled amber LUT;
  4-frame cross-fades interleaved across tubes (~26 ms/frame, `/status.draw_ms`). Generator:
  `tools/gen_nixie_glyphs.py`. Flashed, 24/24 smoke, confirmed on the glass.
- ✅ **UP/DOWN now visibly dim the nixie face** (LUT rebuilt on brightness change; in-place repaint).
- ✅ **Nixie letters + device-rendered messages (2026-09-17).** Glyph set 0–9 A–Z `- : . ! ? ° % + /`
  (46 plates + shared base, ~420 KB); `Mode::Nixie` engine: static / flash / scroll / countdown via
  `POST /nixie/text|countdown|stop`; helper 🕯️ Nixie message panel; messages as 🎭 device scenes.
- ✅ **"0 stuck over a 1" glitch** — logic proven clean by `test/nixie-native.sh` (every glyph pair ×
  every fade step exact; all 86,400 times through tick()); on-glass cause = a panel missing an SPI
  transaction mid-fade → 30 µs CS settle after every select. Device-side `POST /test/clocksweep`
  drives every valid time through the real path; smoke runs the midnight slice. **Full-day sweep run
  on the clock 2026-09-17 (no fades): all 86,400 times rendered across three runs (two cut short by
  manual power-cycles — `reset_reason: poweron`, not crashes), worst step 144 ms, heap flat at ~221 KB.**
- ✅ **Fade-on full-day soak (2026-09-17 evening): PASS** — all 86,400 times with 4-frame cross-fades
  in ~27 min, no reboot, slowest step 677 ms (5-tube rollover), heap flat ~216 KB. No stuck/mixed
  glyph observed by the tester in this run; the CS settle stays.
- ✅ **`/status.reset_reason`** — crash vs. power-cycle without a serial cable.
- ✅ **`/ota` crashed on a raw body (fixed 2026-09-17).** The core-3.x `WebServer` gives a multipart
  body via `upload()` and a `curl --data-binary` body via `raw()`; reading the wrong one was a
  `LoadProhibited` panic that looked like a successful update (reboot → old image, `reset_reason:
  "panic"`). Now picks by `Content-Type`; **`/status.build`** (compile date/time) shows which image
  is running — check it after every OTA. espota (port 3232) was never affected.
- ⬜ **More presets** — a Date/mini-calendar preset on-device (needs a firmware date renderer);
  optionally let the helper define the preset rotation. Rich pixel scenes (weather/ticker/photos)
  stay a helper-push feature.
- ⬜ **Nixie tuning knobs** — ghost intensity / bloom / digit size are generator constants; consider
  a `/config/nixie` (ghost on/off, warmth) if wanted. Optional faint warm underglow for preset 0.
- ⬜ **Display writer on its own task** (core 1, op queue) so fades/uploads don't stall HTTP/LEDs —
  the stream task already runs on core 1; the REST/fade path still shares the main loop.
- ✅ **`POST /reboot`**, **LED effect + solid colour persisted**, **📅 Date preset** (preset 4, nixie
  letters, refreshes at midnight), **TEST announce** before a clock sweep (2026-09-17).
- ✅ **Countdown fixed** — right-aligned onto live tubes (its seconds used to land in the empty slot 0).
- ✅ **UX audit** ([UX-AUDIT.md](UX-AUDIT.md)) — findings 1–8 fixed (labeled numbers, one obvious
  way to choose what shows, advanced folded away, ⋯ on scene rows, ℹ️ notes).
- ✅ **On-device menu (v1)** — long-press MODE opens it; UP/DOWN move, MODE selects, POWER exits.
  Items: Clock · LED off/rainbow/breathe/comet/solid · Bright ± · All-off · Exit. *Flashed.* (Will be
  reconciled with MODE-cycles-scenes above.)
- ✅ **Tube 1 diagnosed — NOT faulty.** Lit all colors in the boot self-test AND showed the per-tube
  green push → display, FPC, and firmware path all work; the old "won't update" glitch is resolved by
  the init-kick. No replacement needed.
- ✅ **Helper overrides the device — fixed (2026-09-17).** `restoreLast` defaults OFF; choosing a
  device face (helper button, physical MODE, or a 🎭 device scene) puts the helper **🔌 hands-off**
  (no auto-push / restore) until the next explicit push; device faces are first-class scenes and the
  default cycle starts on 🕯️ Nixie (device).
- ✅ **Persist `led_effect` in NVS** — done (with the solid colour).
- ⬜ **Periodic reboot / heap watch.** Heap is currently healthy (~210 KB); revisit only if long-run
  fragmentation from image pushes shows up. Low priority.

### Rendering / performance (the "make it smooth + lean" thread)
- ✅ **Region-blit protocol (2026-09-17).** `POST /tube/{i}/blit?x&y&w&h` streams a sub-window; clamped;
  smoke-tested. Helper: **one push path** `pushFrame()` (last-known frame per tube, row-band diff → skip /
  blit / full; recorded only after success; cache dropped on reboot / device face / clear). Replaces the
  seven old paths, the sampled hash and the stale-panel bug. Measured: 43 blits : 12 frames on the marquee.
- ✅ **Streaming transport + display writer task (2026-09-17 evening).** ESP/1 binary channel
  ([ESP1-PROTOCOL.md](ESP1-PROTOCOL.md)): TCP :5555 (up to 6 clients, one per tube), UDP :5556 (lossy, not
  used), the same frames over the USB UART; RAW / RLE / PAL4 formats, per-frame acks, optional token;
  stream task on core 1; `/status.stream` counters; `test/esp1-bench.py`, `test/esp1-serial.py`.
  **Measured ceiling ~20 tube-updates/s (≈4 fps per tube × 5)** — CPU/overhead-bound draw path.
- ✅ **UART shell** (`help status preset nixie flash scroll countdown stop led bright mode wifi sweep
  fps baud token reboot`) + binary frames over USB @115200; `Serial.setRxBufferSize(16384)`.
- ✅ **Mac bridge** `tools/bridge/bridge.py`: webcam / file / GIF / HEVC / YouTube (yt-dlp) / frame
  dirs / dwarves / test → per-tube PAL4/RLE/RAW → one TCP connection per tube; `serve` exposes a
  localhost API the helper's 🔌 Wired / stream panel drives; `shell`, `bench`, `--fake` node.
- ✅ **PoC: Snow White & the seven dwarves** — `tools/render_characters.py` (pure-Python rasteriser,
  8 original CC0 characters, 36-frame turntables) → `samples/dwarves/`; runs on the clock at ~4 fps/tube.
- ✅ **Character sets in the helper (2026-09-17, later)** — three sets of eight (`dwarves`, `island`,
  `winter`; original evocative designs), quality pass (3× supersampling, smoother shading, idle motion,
  faces), `samples/characters.js` sidecar so the 📹 Live panel plays them with no bridge; the 🔌 panel
  offers `chars:<set>` with a cast picker; bridge `play chars:<set> [--chars a,b]`.
- ✅ **🔌 Wired panel always visible** with start-up instructions (it used to be hidden until the bridge
  answered, so nobody found it).
- ✅ **🧷 USB serial terminal in the helper** (Web Serial): boot log, shell with history + chips,
  timestamps, log download, ℹ️ how-it-works; every chip verified against the firmware over the cable.
- ✅ **WiFi roaming + recovery AP (v1.9.0)** — `src/net.*` replaces WiFiManager: up to 8 saved networks
  (legacy creds imported), strongest-known join, self-hosted `esptube-setup` hotspot with captive
  portal when nothing known is in range (bounded glass announce, auto-closes 90 s after STA is back),
  menu **AP**, shell `wifi …`, REST `/wifi/*`, on-device page + helper (REST and over-the-cable) UIs.
- ✅ **Time without WiFi** — DS1302 RTC driver (`src/rtc.*`): seeded at boot, written on every NTP
  sync; `time <epoch>` / `POST /config/time` / helper 🕐 *Set time from here* for never-online clocks.
- ✅ **USB serial as a first-class transport** — bridge `--transport auto|tcp|serial` (network if the
  node answers, else the cable; baud hand-shake 115200 → `--baud` via the shell; probes a saved boot
  rate); helper *control over the cable* (auto/always/never): REST → shell lines, pixel pushes + 📹 Live
  → ESP/1 frames with acks over Web Serial, automatic switch-back; firmware `baud N` arms a one-shot
  boot rate (RTC memory) for the helper's ⚡ Fast link, `wifi json`, richer shell `status`.
- ✅ **Boot join** — the last-known-good network is tried blind before the scan (Sam: "it isn't
  auto-joining on reboots"; the first scan after WiFi init misses APs).
- ✅ **Phone app = the clock hosts the helper** — `/helper` (gzipped from flash, ~60 KB), PWA manifest
  + icon, LittleFS asset store (`/fs/put`, `/samples/`) for the characters sidecar; *Add to Home
  Screen* on iPhone. Web Serial doesn't exist on iOS — the phone path is WiFi/hotspot only.
- ✅ **Transport parity over the cable** — shell `clear`, `rgb <tube> r g b`, `text <tube> …`, `idle [min]`;
  the helper routes `/tubes/clear`, `/tube/i/rgb`, `/tube/i/text`, `/config/idle` over serial.
- ✅ **Master clear + idle return (v1.9.5)** — `POST /tubes/clear` stops message/sweep/LED effect and blanks
  everything; the helper's 🧹 stops Live/bridge/cycle/pusher first; MANUAL hands back to the clock after
  `idle_min` (default 30) without content. Low-heap guard on stream accepts.
- ✅ **🎭 Cast + 🎨 Look in the helper** — drag faces onto tubes/slots, load/mix families (persisted);
  backgrounds + filters for every Live source. ✅ transparent RGBA sprites (sidecar 4.07 MB with alpha; bridge
  `--bg black|#rrggbb|#a-#b|glow:#rrggbb`, `bg` in `/bridge/start`). 🔜 cast+look as a scene.
- ✅ **Phantom button presses at boot (v1.9.6)** — brightness maxed and MODE long-pressed on every boot;
  buttons now arm only after a quiet released state.
- 📋 **The plan:** `docs/PLAN-80-20.md`.
- ✅ **Best cable rate measured:** 921600 garbles on this USB-UART chip; **460800 works — 40 KB/s, 5
  tube-updates/s, ack ~350 ms** (4× the 115200 baseline). Bridge default `--baud 460800`; every rung is
  verified; the node's UART watchdog reverts a bad rate on its own.
- ⬜ **Bluetooth** — investigated, `docs/GUIDE.md` §13: a NimBLE Nordic-UART shell + Web Bluetooth in
  the helper is the viable path (measured +241 KB flash / +9.7 KB RAM — fits the 2 MB slot at 82 %);
  BT Classic SPP does not fit. Not started.
- ✅ **On-device menu v2** in nixie letters: FACE · LED · BRI+ · BRI- · WIFI (scrolls the IP) · TEST ·
  FPS · BOOT · EXIT.
- ✅ **📹 Live in the helper (browser does the heavy lifting):** camera / screen-or-tab share / video
  & GIF files / animation studio → JS 16-colour median cut + Bayer dither → PAL4x2 line doubling →
  changed-row bands → one **WebSocket per tube** (firmware `:5557`, text frames = shell). Worker-clocked
  (page timers throttle in background tabs), ack-driven, auto fps, self-reconnecting. **Measured
  6.3 fps/tube × 5** on the animation studio. Camera/screen need the user's own Chrome (the Claude pane
  blocks device capture).
- ✅ **Bridge round 3:** changed-row-band diffs, `pal4x2`, `--dither fs|bayer`, `--fps auto`, `page:URL`
  (headless Chrome), `screen:N`, install help + `doctor`. Demos run on the clock (dwarves, GIF loop,
  YouTube via yt-dlp, web page).
- ✅ **Stream slot eviction (firmware `esp1.cpp`, 2026-09-17).** With all 6 TCP slots held, "newest
  wins" round-robin eviction made a reconnecting controller evict its own fresh sockets until lwIP
  reaped the old five — the bridge's "connection closed by node — reconnecting" churn loop with
  0 updates. Now evicts the least-recently-active slot (per-slot last-byte timestamp).
- ✅ **Bridge `url:` (YouTube) fixed (2026-09-17).** `yt-dlp -g` URLs now get HTTP 403 from Google
  (media URLs gated per player client with PO tokens); the bridge runs `yt-dlp -o -` piped into
  ffmpeg's stdin and first probes which player client streams today (`YT_CLIENTS`, `mweb` first).
  On a source ending with no frames it prints ffmpeg's last stderr lines and suggests `brew upgrade
  yt-dlp` or sharing the tab from the helper's 📹 Live panel. **Measured on the clock: 480p →
  26–30 tube-updates/s ≈ 5.5 fps/tube, ~200 KB/s, busy 68 %** (75 upd/s against `--fake`: the
  node's draw path is the ceiling).
- ✅ **Bridge `page:` fixed (2026-09-17).** Headless Chrome (x64 under Rosetta) took > 30 s on its
  first launch; deadline now 60 s and the error carries Chrome's exit code. A static page = one
  update per tube then nothing (band diff) — correct, not a stall; the stats line shows `(total N)`
  and `samples/page-demo.html` ticks seconds.
- ✅ **Complications** (🧩 time/date/temp/text/heap in corners/strips over any widget) and the **🌐 HTML
  widget** (per tube + spanning) in the helper. **Weather:** `api.open-meteo.com` is sinkholed to 0.0.0.0 by
  the home DNS blocklist; the helper now tries open-meteo → met.no → wttr.in (and three geocoders), so it works
  regardless (`test/weather-providers.sh`).
- ⬜ **yt-dlp client selection is a moving target.** Google rotates which player clients stream
  without PO tokens; expect `url:` to break again. Keep `YT_CLIENTS` current and `brew upgrade yt-dlp`
  (2026.8.19 available vs 2026.7.4 installed) — or use the helper's tab share, which needs no yt-dlp.
- ⬜ **Install the arm64 Chrome build** — the installed Chrome is x64 under Rosetta (slow first
  launches for `page:`; Chrome's own log calls the combination neither tested nor maintained).
- ✅ **Weather hardened against the DNS sinkhole** — provider ladder + geocoder ladder + 30-day geocode cache;
  allow-listing `api.open-meteo.com` is now optional (it only buys open-meteo's true daily hi/lo back).
- ✅ **`/status` stalled the clock ~250 ms per poll** (LittleFS usage walk) — cached 60 s; `GET /ping` added.
- ✅ **📶 Signal** — clock RSSI / ping / cast KB/s + ack / internet Mbps on the tubes, proactive weak-WiFi
  flag in the helper and a bounded `WIFI WEAK` scroll from the clock itself (`wifi warn`, `/config/netwarn`).
- ⬜ **The clock's spot is marginal**: RSSI swung −51 → −76 dBm within 20 min today. Casting stutters below
  −70; move the clock or the AP. The 📶 tools now show it instead of guessing.
- 🔬 **📹 Live camera / screen share must be tested in the user's own Chrome** — the Claude preview
  pane blocks `getUserMedia` / `getDisplayMedia`, so those paths can't be verified from a Claude session.
- ⬜ **Higher UART baud** (`baud N` + live change lost the link with this bridge/driver) — low value.
- ⬜ **DMA blit flushes** (crashed with the vendored fork; code behind `g_dma=false`) and a leaner
  decode (straight into the block buffer, 16-row blocks) — maybe 1.5× on the draw path.
- ✅ **Row-band diffs in the bridge** (shipped in v1.7 round 3: `--band`, changed-row bands per tube).
- ⬜ **Column trimming in the diff** (currently row bands only) — cheap once the stream exists.
- ⬜ **News as a single-tube, old-school scrolling ticker** on the blit path.
- ⬜ **Double-buffer + fade-swap** (dim/cross-fade instead of chop) — old-school, nearly free on ST7789.
- ✅ **Streaming transport** — ESP/1 over TCP :5555 / WebSocket :5557 / UART (UDP tried, too lossy).
- ✅ **Cheap encodings** — PAL4 (4bpp + 16-colour palette) and PAL4x2 (line-doubled) shipped; 1bpp still open.

### Content / UI polish
- ✅ **Drag a picture across the tubes** (spanning · ✋ Free fit): pointer drag / wheel zoom /
  double-click reset on the preview, throttled live push, per-panel full-coverage hash recorded only
  after a successful POST. The **seam gap** setting keeps the picture continuous across the physical
  gap between tubes.
- ⬜ **Smooth fades everywhere** (not just on drag-swap) once double-buffer/fade lands.
- ⬜ **Quarto blog post** for diligentservices.io (the saga + photos + LEARNINGS are all in-repo).

## Done (shipped, in git)
- ✅ Identify + revive the SI HAI board; custom firmware (REST, NTP clock, per-tube text/image/glow),
  helper web console, CLI, docs, Chrome web flasher, recovery guide, public GPL-3.0 repo.
- ✅ Two-layer control: LED effects (comet/rainbow/…) run live over the clock/content.
- ✅ Physical buttons + **MODE regression fix** (returns to clock first, then cycles effect; long-press = off).
- ✅ Scene manager (save/load/edit/rename/duplicate/export-file/import) + **scene cycling**.
- ✅ Per-tube **Ticker** (crypto/stocks/weather/JSON) with polling interval + readability controls.
- ✅ **Nixie glyph** face (per-tube clock + spanning big-clock).
- ✅ **Date / mini-calendar** per-tube, spanning **month calendar**, and **single-tube time+date+calendar**.
- ✅ Weather auto-fetch (default daily); ticker interval polling.
- ✅ **Glyphs everywhere** (kid / non-English friendly); cleaner save interface.
- ✅ **Drag-to-swap** tubes + blank **clone Holder** + fade-up on push.
- ✅ **Storage-full fix** — images auto-downscale before hitting localStorage.
- ✅ **API keys** saved in localStorage + Backup, auto-fill blank fields; never pushed to device/git.
- ✅ **⭐ Starter scenes** seeder.
- ✅ **Web mirrors device state + stale indicator** — the header always shows the device's real
  mode/effect/brightness/per-tube content; a "live / ⚠ stale Ns / ⚠ offline" badge and dimming show
  when the data is old.
- ✅ **Storage: always-compress images** — every loaded image is downscaled + JPEG-compressed to a
  byte target (~32 KB/tube, ~100 KB spanning) before touching localStorage.
- ✅ **Storage: content-addressed image pool (de-dup).** The real fix for "storage full": state and
  scenes are stored EXTERNALIZED — each unique image lives once in `esptube_imgpool`, tubes/scenes keep
  only `#img:<hash>` refs. Same image on 6 tubes = 1 copy (was 12 across state+scene). Reference-counted
  GC on save; omit-if-too-big fallback; a one-time boot migration shrinks pre-existing bloat (measured
  471 KB → 48 KB on a 2-scene test); accurate 🗄️ meter (chars vs ~5 MB). Backup carries the pool.
- ✅ **Content divorced from device → IndexedDB store.** Image blobs now live in IndexedDB (large
  quota; verified 12 MB / 57 imgs, survives reload) mirrored in memory for sync rendering; localStorage
  keeps only tiny `#img:` refs (~14 KB) so it never fills. Dedup preserved. Stored at ~4× device
  fidelity (render/push still downscale to 135×240). One-time migration moves old localStorage pools in.
- ✅ **Master clear & reset** — 🧨 Reset all wipes localStorage + IndexedDB and reloads.
- ✅ **Helper on GitHub Pages + local** — served at `…/tools/helper.html`; a banner explains that an
  HTTPS-hosted copy can't reach a local `http://` clock (mixed content), so run it locally to control
  the clock. Works from `file://` and `http://localhost` as before.
- ✅ Consistent tube index (0 = far right); LED index aligned to display index.
- ✅ README **replacement parts** (glass tube / display module / whole clock) + sourcing + swap how-to.
- ✅ [ESP1-PROTOCOL.md](ESP1-PROTOCOL.md) — the stream wire format and the measured numbers, in one place.

## v2.0 (2026-09-18)
- ✅ ESP/1.1: node-owned epoch, CANCEL, ECHO, NOACK, exact lengths, datagrams over UART, UDP per-datagram.
- ✅ Mutex leak on aborted REST uploads; `/raw565` on the blit path; broadcast clear; `/status` primed at boot.
- ✅ Cable: CH340 identified, 1.5 M verified ladder (helper + bridge), 119 KB/s, cable-first policy.
- ✅ Helper Outbox (one pipeline, latest-wins, supersede) · UI shell (dock, command bar, ⌘K, keys, toasts).
- ✅ Layout schema + 🧊 Stage + calibration + USD/USDZ + `/layout.json` + bridge `--geometry`.
- ✅ Capture on secure pages: `tools/console.py`, guard + explanation in the helper.
- ⬜ **Measure or calibrate the tube pitch** — 35 mm is an estimate; nothing in the repo is measured.
- ⬜ **Helper over the cable, end to end, in a real Chrome** (Web Serial needs a user gesture; the preview pane cannot pick a port): connect → ⚡ Fast link → push → Live → Stop & clear.
- ⬜ Exact device-face emulation in the Stage: stream the PROGMEM nixie plates (`GET /nixie/plates`) and port `nixieCompose` (the Stage uses the font look-alike today; Digital/Date faces are labelled, not drawn).
- ⬜ Merge the two 📡 Stream panels into one with an engine switch (they share a name and vocabulary now).
- ⬜ 2 M baud for pixels: frame long text replies too, then 2 M passes verification (149 KB/s measured).
- ⬜ Spanning with per-display y offsets / rotation (the Layout module already computes them; callers use x only).
- ⬜ Bridge: both links at once; UDP client; fake node that speaks 1.1.

## Other tube-clock boards + device autonomy (2026-09-18)
A second board is on order: another six-tube ESP32 clock with the same 6 × ST7789 135×240 panels but **8 MB flash** (ours 16),
**one button** (ours four), **no RTC**, 6 LEDs, sometimes no display-power transistor, and the same slow DIO-40 MHz flash. It
should run this firmware with a board profile rather than a fork.
- ⬜ **Board profiles**: `boards/<name>.h` (pins, chip-select scheme 74HC595 vs GPIO list, button count + gestures, RTC, LED
  count, flash/partition) + one PlatformIO env per board; `/status` reports `board · panels[w,h] · buttons · leds · rtc`.
- ⬜ **Helper reads capabilities** instead of hard-coding `NTUBE=6 · CW=135 · CH=240` and the `sihai-6` layout model; add a
  profile for the one-button board.
- ⬜ **More than one clock**: the helper has a single target (`base()`); a device list fed by the mDNS `_esptube._tcp` browse
  (every clock already advertises id · name · model) so any clock's helper can drive another clock on the same LAN.
- ⬜ **Device autonomy.** Everything rich needs the helper running. Worth doing on the device itself: face packs / a photo
  album on LittleFS (8 MB free), a mode playlist, weather fetched by the clock, a night dimming schedule, alarms, 12/24 h +
  blank leading zero, time zone from IP geolocation (matters now that it roams), MQTT / Home Assistant discovery.
