# Learnings — the sharp edges

Hard-won gotchas from reviving this clock. If you (or a reader) hack one of these boards, this is
the list that would have saved days.

### Identification
- **Identify the variant from the PCB silkscreen FIRST.** It *looked* like an EleksTube "IPSTube"
  clone; it's a **SI HAI**. Five blank-screen iterations came from trusting the look. The silkscreen
  literally said "SI HAI IPS CLOCK". Lesson: read the board, don't pattern-match the genre.
- Tells that separate them: **SI HAI = 74HC595 shift-register chip-select + 4 buttons**; IPSTube =
  direct-GPIO CS + 1 button. Completely different display pins.

### Display bus (SI HAI)
- Chip-select is a **74HC595 shift register** (DATA=4, CLK=22, LATCH=21), not per-tube GPIOs.
  Select = clock out `(~digits_map)<<2` (Q7/Q6 unused, active-low), latch. Holding "CS" GPIOs low
  did nothing because there are no CS GPIOs.
- Display SPI: **MOSI=19, SCLK=18, DC=16, RST=23, CS=-1**. WS2812 underglow=**GPIO32**,
  display-enable MOSFET=**GPIO2**. Buttons UP/MODE/DOWN/POWER=**35/34/39/36**. RTC DS1302=33/25/26.
- **ST7789 135×240 needs `CGRAM_OFFSET`** or content lands off-screen (looks blank).
- **Stock TFT_eSPI 2.5.43 is broken on ESP32 Arduino core 3.x** (`espressif32@7.x` / pioarduino):
  panels stay backlit-but-blank. Use EleksTubeHAX's **patched fork** (`modified_TFT_eSPI`).
- Symptom decoder: *backlit but blank* = wrong driver/variant; *panels go black (init reached) but
  nothing draws* = CS/select not landing; *SPI data visibly bleeding onto the WS2812* = GPIO5 is the
  ESP32 VSPI native chip-select and the bus is active but not selecting a panel.

### Auto-detect: not possible here
- ST7789 read-back (`readcommand8` RDDID/RDDST/RDDPM) returns **0x00 on every tube** — these clone
  panels/FPCs don't wire the read path. So the firmware can't sense which slots are populated.
  Solution: a **user-set populated mask persisted in NVS** (each index = a fixed shift-register slot;
  empty slots stay dark).

### Flashing / USB
- The clock **powers + enumerates only through a powered USB hub**; the Mac's own port won't drive
  it (no barrel jack — power is over USB).
- **Small (~1 MB app) esptool writes are 100% reliable; a full 16 MB read is not** (the stock backup
  stalled at 14/16). Direction matters on a marginal link.
- The board **auto-resets via DTR/RTS** — no BOOT-button dance needed to flash.
- After flashing custom firmware, updates can go **OTA over WiFi** (`/ota` or ArduinoOTA) — no cable.

### Firmware/network
- **HTTPS page can't talk to an HTTP LAN device** (browser mixed-content block) — so the control
  panel is **served by the clock itself** (embedded in the app via a pre-build step), same-origin.
- **First-config boot quirk:** the HTTP server can bind before the WiFi (STA) interface exists →
  connection refused until one power-cycle. (Fix: rebind on got-IP — on the list.)
- **ESP32 GPIO 34–39 are input-only with NO internal pull-ups** — buttons on those pins need
  external pull-ups and `pinMode(INPUT)` (not `INPUT_PULLUP`).
- **Phantom button presses at boot.** The input-only pins read LOW for a moment after power-up, and a
  debounce timestamp initialised to 0 made the very first reading count as settled (`millis()` is
  already ~6 s by the time `loop()` runs). Every boot quietly pressed UP (brightness → 255, *persisted*)
  and long-pressed MODE (menu open/close). Arm each button only after it has read *released* for
  300 ms, and ignore everything in the first 1.5 s. Found by reading a boot log for something else.
- **A provisioning library is not a roaming strategy.** WiFiManager stores exactly one network in the
  WiFi driver's own NVS and blocks on a portal when it's missing. A 60-line state machine — scan
  (async), join the strongest *saved* network, fall back to a self-hosted AP, keep scanning in the
  background, close the AP once STA is back — is smaller than the library it replaced (−44 KB flash)
  and never blocks `loop()`. Read the old credentials with `esp_wifi_get_config()` **before** touching
  `WiFi.persistent()`; that's how an upgrade keeps the home network.
- **Recovery must not hold the glass.** The AP announces itself with one *bounded* nixie scroll
  (auto-stops after 45 s, skipped when pushed/streamed content is showing) — a clock that's off-line
  still has to be a clock and stay on whatever face or scene it was left on.
- **The DS1302 was there all along.** The board's battery-backed RTC (CE 26 · IO 25 · SCLK 33, 3-wire
  bit-bang) still held the stock firmware's 2023 timestamp. Seed the system clock from it at boot and
  write it on every SNTP sync (`esp_sntp_set_time_sync_notification_cb`) — the nixie face is right
  with no network at all. Store UTC in the chip; let `TZ` do local time. Convert with a civil-date
  formula, not `mktime()` (which applies the local zone).
- **A "successful" OTA can be a crash.** The core-3.x `WebServer` hands a multipart/form-data POST
  body to the upload callback through `upload()` but a plain body (`curl --data-binary @firmware.bin
  http://<clock-ip>/ota`) through `raw()` — and the *other* accessor is a null `unique_ptr`, so
  touching `upload()` on a raw body was an instant `LoadProhibited` panic on the first chunk. The
  node rebooted, which looked like a finished update — into the OLD image. Tells: HTTP 000 / empty
  reply and `reset_reason: "panic"` in `/status`. Fix: `collectHeaders("Content-Type")` and read
  `raw()` or `upload()` accordingly; `/status.build` (compile date/time) now says which image is
  actually running — check it after every OTA. espota (ArduinoOTA, port 3232) never had the bug and
  is how the earlier OTAs here actually landed.

### Making it look like a nixie (and testing it)
- **Render offline, colorize on the device.** An amber font is not a nixie. Pre-render the digits
  as 8-bit *intensity* plates (thin wire glyph + bloom; the stacked ghost cathodes + glass vignette
  as one shared base plate), RLE them into flash, and map intensity → RGB565 through a LUT at draw
  time. Cross-fades become a per-pixel blend of two plates; brightness becomes a LUT rebuild. No
  frame buffer needed on a 320 KB part.
- **A stuck half-blended glyph is a missed SPI transaction, not a logic bug.** Test the render core
  natively first (every glyph pair × every fade step, all 86,400 times through the transition logic —
  seconds on a laptop). When that passes and the glass still shows a mixed frame, the panel missed a
  write mid-fade: add a chip-select settle (30 µs) after every select.
- **Positional layouts hide digits in empty slots.** With slot 0 empty, a clock-style HHMMSS layout
  never shows seconds-ones — fine for a clock, wrong for a countdown ("is this in milliseconds?").
  Right-align onto *live* tubes for anything where the last digit matters.
- **Anything that takes over the glass must announce itself.** A rapid all-times sweep looked "stuck"
  and got power-cycled twice. The sweep now flashes TEST first and reports progress in `/status`.

### Throughput (measured, not guessed)
- **Bytes aren't the ceiling; requests are.** Sending only changed rows via `/blit` halved bytes,
  but a synchronous `WebServer` on a single-threaded loop tops out at ~4 tube-updates/s whether a
  push is 34 KB or 65 KB. Diffing is still right (it's free), but the next real gain is a persistent
  binary stream + the display writer on its own core. Measure before optimizing the wrong thing.
- **One push path.** Seven independent render→POST routines each had their own (or no) dirty
  tracking; one had a hash-before-POST bug that froze a panel silently. A single `pushFrame()` that
  records the last frame *after* success, and forgets it when the device draws its own face, fixed
  all of them at once.

### Streaming into an ESP32 (what the numbers taught)
- **On this board USB is the slowest way in.** The WROOM-32D has no native USB; the port is a UART
  bridge. Frames over it work (10 KB/s at 115200) but WiFi-TCP is 30× faster. "Wired" ≠ fast here.
- **Opening the serial port reboots the board.** The auto-reset circuit esptool relies on fires on
  the DTR pulse macOS drivers send on open. Any host tool must expect a ~6 s boot after opening —
  and a baud change that closes/reopens the port silently resets the node back to 115200.
- **"Higher baud never worked" was a handshake bug, not a hardware limit.** The bridge opened the
  port straight at 921600 while the node boots at 115200 — nothing could ever answer. Open at the boot
  rate, wait out the reset, ask the shell (`baud N`), *then* switch the host side — and **verify** with a
  `help` round-trip, because the USB-UART chip/driver decides what it can do (this one garbles at
  921600 and is solid at 460800: 40 KB/s, 5 tube-updates/s on the cable). Web Serial can't change baud without close/reopen (= another
  reset), so `baud N` arms the rate for exactly ONE reset in RTC memory. A *persisted* boot rate was
  tried for an hour and dropped: a rate the chip can't do leaves the shell unreachable.
- **"The shell is dead" has a 5-second hardware test.** `esptool --port … chip_id` talks to the ROM
  bootloader, no firmware involved. *"Download mode successfully detected, but getting no sync reply:
  the serial TX path seems to be down"* = the clock can talk to the Mac but not hear it — the
  Mac→clock wire (hub port, cable, the USB-UART chip's TX pin). Firmware can't cause that; re-seat
  the cable / change hub port before touching code. (An afternoon was spent suspecting a UART
  watchdog that was innocent.)
- **A build stamp must live in a file that always recompiles.** `__DATE__ __TIME__` inside api.cpp only
  changed when api.cpp itself was rebuilt, so `/status.build` "proved" an OTA hadn't landed when it
  had. The pre-build script now writes `build_stamp.h` on every build.
- **One owner per port, and the owner is usually you.** A helper tab with the 🧷 panel connected holds
  `/dev/cu.usbserial-*`; the bridge, `monitor.sh` and `flash.sh` then fail with "Resource busy".
  `lsof /dev/cu.usbserial-*` names the process.
- **A second transport must be automatic to be real.** Users don't pick "serial"; they plug the cable
  in and expect things to work when WiFi is gone. So: probe the network for 2 s, fall back to the
  cable, and switch back on your own — in the bridge (`--transport auto`) and in the helper (`api()`
  routes REST → shell lines and pixels → ESP/1 frames when the network stops answering).
- **The default 256-byte UART RX buffer eats frames.** Any loop stall > 20 ms drops bytes; set
  `Serial.setRxBufferSize(16384)` before `begin()`.
- **WiFi modem sleep costs ~100 ms per TCP ack.** `WiFi.setSleep(false)` — and re-assert it after
  WiFiManager, which re-inits the radio.
- **lwIP's receive window is fixed at 5.7 KB in the Arduino core**, so one connection ≈ window ÷ RTT
  ≈ 300 KB/s. More connections help only until the node's draw path becomes the limit.
- **The main loop cannot drain a stream.** Under load it ran ~30 iterations/s and the window only
  opens when the app reads. A dedicated FreeRTOS task fixed that — on core 1 at loop priority (core 0
  starved behind WiFi/lwIP) and it must yield a tick per pass or the idle task starves and the task
  watchdog reboots the chip.
- **UDP looks fast and isn't usable**: the node drains 1.6 MB/s in bursts but WiFi/lwIP lose 10–60 %
  of datagrams. Without reliability on top, no.
- **DMA in the vendored TFT_eSPI fork crashed the node; 80 MHz SPI gained nothing.** The per-frame
  cost (~25 ms) is decode + per-block select/settle + FIFO loading, not the clock. Measure before
  believing the datasheet.
- **Browser timers lie in background tabs.** `setTimeout` collapsed to ~1.5 fires/s in an unfocused
  pane, so a streaming loop clocked by page timers crawls. Clock it from a Web Worker (worker timers
  are not throttled) and from the node's acks. Also: `getDisplayMedia` (share a tab) is the universal
  video source — it is how a YouTube tab reaches the tubes without yt-dlp.
- **"Newest wins" slot eviction evicts yourself.** With all six TCP stream slots held, round-robin
  eviction of the newest slot meant a controller reconnecting five tubes kicked out its own fresh
  sockets whenever lwIP hadn't reaped the previous five yet — a churn loop the bridge shows as
  "connection closed by node — reconnecting" and 0 updates. Evict the least-recently-active slot
  (per-slot last-byte timestamp) instead.
- **`yt-dlp -g` URLs are dead on arrival for ffmpeg.** Google gates media URLs per player client
  (PO tokens), so handing the URL to ffmpeg gets HTTP 403. Run `yt-dlp -o -` and pipe into ffmpeg's
  stdin so yt-dlp carries its own client headers, and probe which player client streams today
  (`YT_CLIENTS = ("mweb", "default")`, 64 KB probe; as of 2026-09 `mweb` streams, the default web
  clients 403). This is a cat-and-mouse dependency: when it breaks, `brew upgrade yt-dlp` first, or
  share the YouTube tab from the helper's Live panel (`getDisplayMedia` needs no yt-dlp at all).
- **The old tricks still work.** Line doubling (send every other row, draw each twice) bought +50 %
  frame rate for free on the wire; 16-colour palettes with dithering look fine on 135×240 glass;
  changed-row-band blits and "keep the chip selected between blocks" are the CRT-era playbook
  applied to SPI. When the transfer is fixed-cost, send fewer pixels, not faster bits.

### Tooling
- System `python3` (Xcode) lacks `pyserial`; use esptool's bundled interpreter at
  `/opt/homebrew/Cellar/esptool/*/libexec/bin/python3` for serial scripts.
- `pio device monitor` needs a real TTY (won't run piped) — use it interactively (`tools/monitor.sh`).
- **Headless Chrome's first launch can take > 30 s here.** Google Chrome on this Apple-silicon Mac
  is the x64 build under Rosetta (Chrome itself logs that running x64 Chromium on Arm via Rosetta is
  "neither tested nor maintained"), and the first launch after a Chrome update is slow — the bridge's
  `page:` source looked broken when it was only waiting. The bridge now allows 60 s and reports
  Chrome's exit code; install the arm64 Chrome build.
- **A static page streaming "one update per tube, then nothing" is correct.** The band diff sends
  nothing for identical screenshots, so 0 upd/s on a still page is not a stall — the stats line shows
  `(total N)` for that reason, and `samples/page-demo.html` ticks seconds so it visibly changes.
- **One weather host is one point of failure.** The home DNS blocklist sinkholes `api.open-meteo.com` to
  0.0.0.0 (a browser sees "failed to fetch" in 1 ms). Three key-less providers answer with CORS `*`
  (open-meteo, met.no, wttr.in) and three geocoders do too (open-meteo, Nominatim, wttr.in); a ladder that
  remembers the last winner for 12 h makes the block invisible. fast.com's API has **no** CORS header, so a
  page can't run it — Cloudflare's `__down`/`__up` endpoints can. `test/weather-providers.sh` checks all six.
- **`LittleFS.usedBytes()` walks the whole partition.** With the 4 MB sidecar on a 12.5 MB LittleFS that is
  ~250 ms, and `/status` called it on every request while the helper polls every 5 s — a 250 ms loop stall
  every 5 s that ate stream acks and made casting feel "REALLLY slow". Cache it (60 s, refreshed by `/fs`
  writes): `/status` went 290 → 45 ms. Measure the endpoint, not just the radio.
- **Show the clock's RSSI, not the laptop's.** The ESP32 is the endpoint that struggles; it swung −51 → −76
  dBm in 20 minutes at its spot. The number belongs on the glass (📶 Signal widget / complication) and the
  clock should say so itself (`WIFI WEAK` scroll, bounded, clock-face-only) — recovery never holds the glass.
- **Camera / screen capture only exist on secure pages.** `navigator.mediaDevices` is `undefined` on plain
  http to a LAN name — so the moment the clock began hosting the helper, capture "broke" for anyone using
  that URL, with a TypeError instead of a reason. `localhost` is a secure context: `tools/console.py`
  serves the same page there (and gives it one stable origin). Detect `isSecureContext` and *say why*.
- **"USB is faster" was false until the link was fixed.** 460800 stop-and-wait = 33 KB/s vs 120–200 KB/s
  WiFi. Three changes made it 119 KB/s: a higher verified rate (CH340: 1 M and 1.5 M are clean, 921600 is
  not, 2 M garbles long replies), datagram framing with NOACK bands (no per-tile ack bubble), and the RX
  FIFO-full threshold at 32 instead of the core's 120 (8 bytes of margin = 40 µs at 2 Mbaud → overruns).
  Probe rates on the real hardware; "it garbles" at one rate says nothing about the next one up.
- **A cancel that queues behind the data it cancels is not a cancel.** Same socket: 985 ms. Its own
  control socket: 57–110 ms. Same idea on the cable: one datagram per write and ≤ 40 ms queued.
- **The generation counter must belong to the node.** A client-owned counter with a "newer" compare lets
  a second writer (bridge at 5, helper at 200) starve the first forever. Node-owned epoch + equality +
  "successor advances" has no such failure, and legacy clients just adopt the current epoch.
- **Check the epoch inside the lock.** A blit block parked on the panel mutex while `clearAll()` runs
  repaints *after* the clear unless the check happens after the lock is taken, and the bump before it.
- **An aborted upload must release what START took.** `/raw565` held the panel mutex for the whole HTTP
  upload; a client that hung up mid-body leaked it and froze every stream. Handle `UPLOAD_FILE_ABORTED`.
- **Background processes ignore SIGINT.** A test bridge started with `&` kept streaming for six minutes
  after `kill -INT`; use SIGTERM and verify the pid is gone before touching the port again.
- **`LINE_MAX` is a POSIX macro.** A header-only module that compiles natively can still collide on the
  target; prefix constants.
- **A delimiter that can appear by accident needs a second test.** The datagram framing uses `0x00` as its
  delimiter; one STRAY zero (the reset glitch when a terminal opens the port, or the two zero bytes inside a
  legacy ack) put both parsers into "datagram" state: the helper swallowed all following text (the serial
  console went silent) and the firmware ate the next command. The fix is deterministic, not a timer: a frame
  always starts `E5 7B` and a reply `A5`/`A7`, so the COBS form is `<code> E5 7B …` / `<code> A5|A7 …` —
  anything else after a zero is text and is replayed as text at once. Same release: the line filter dropped
  every command containing UTF-8 (`nixie 21°`); only control bytes mark debris now, and a short garbled
  line answers `ERR unreadable command` instead of nothing. Both are in the native and node tests.
- **State that describes the DEVICE belongs on the device.** The layout first lived in each page's localStorage, so the
  clock-hosted helper and the local file showed different tube positions for the same clock. It now lives at `/layout.json`
  with a revision stamp; pages adopt it on connect and write edits back. Browser storage is per origin — `file://`,
  `localhost` and `http://<clock>` are three different worlds.
