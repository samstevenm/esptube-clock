# tools/bridge — Mac-side video bridge (ESP/1)

`bridge.py` turns anything the Mac can show into ESP/1 frames for the tubes: webcam, the Mac's
screen, a web page, video files / GIFs, YouTube URLs, folders of frames, the rendered character
sets (`chars:dwarves`, `chars:island`, `chars:winter`), or a synthetic test pattern. It slices
the picture into one 135×240 tile per populated tube, sends only the rows that changed, encodes
(PAL4 · PAL4x2 · RLE565 · RAW565) and streams over **TCP :5555** (one socket per tube) or the
USB serial port, paced to what the node actually draws. Wire format: `docs/ESP1-PROTOCOL.md`.

Requirements: Python 3.9+ with Pillow. `ffmpeg` (+ `yt-dlp` for `url:`) on PATH for media
sources — if missing the bridge says `brew install ffmpeg yt-dlp` and exits 2. Google Chrome
(the arm64 build, see `page:` notes) for `page:`. pyserial only for `--serial` / `shell` (the system python doesn't have it, the
esptool one does: `/opt/homebrew/Cellar/esptool/5.4.0/libexec/bin/python3 tools/bridge/bridge.py …`).
`./bridge.py doctor` prints a ✓/✗ checklist of all of it plus the USB port and the node.

```bash
cd tools/bridge
./bridge.py doctor                                        # what's installed, is the node reachable
./bridge.py play chars:winter                             # one character per tube, pal4 + FS dither, auto fps
./bridge.py play chars:island --chars blue,pink,keiki     # just these three, cast shifts every --rotate-every s
./bridge.py play chars:island --bg '#102040-#000000'      # the cast over a navy→black gradient (quote the #)
./bridge.py play chars:winter --bg 'glow:#60a0ff'         # …over a cool radial glow
./bridge.py play dwarves                                  # = chars:dwarves (the original name still works)
./bridge.py play webcam:0 --layout span --fmt pal4x2      # one wide picture, half the bytes
./bridge.py play screen:0 --crop 0,0,1350,480 --layout span --gap 12   # a region of the Mac screen
./bridge.py play page:https://example.com --page-every 10 # headless-Chrome screenshot of a page
./bridge.py play file:clip.mp4 --loop                     # same clip on every tube
./bridge.py play url:https://youtu.be/… --layout span
./bridge.py play dir:frames/ --tubes 5,4,3,2,1            # numbered PNG/JPG, looped
./bridge.py play test --fps 12 --fmt rle                  # fixed rate, no auto pacing
./bridge.py bench                                         # raw / rle / pal4 / pal4x2 at max rate
./bridge.py serve                                         # control API on 127.0.0.1:8787
./bridge.py shell --serial /dev/cu.usbserial-1220         # UART text shell (needs pyserial)
```

Flags: `--host` `--port` `--conns N` `--transport auto|tcp|serial` `--serial PORT --baud 921600` `--token T` `--tubes 5,4,3,2,1`
`--layout tile|span` `--gap px` · `--fmt pal4|pal4x2|rle|raw` `--dither fs|bayer|none` `--no-band`
`--quant median|octree` · `--fps auto|N|0` `--max-fps 15` · `--loop` `--rotate-every s`
`--chars a,b,c` `--bg black|#rrggbb|#rrggbb-#rrggbb|glow:#rrggbb` `--page-every s` `--crop x,y,w,h` `--chrome PATH`.

## Fewer pixels, fewer bytes

The node's draw path is CPU-bound (~25 ms per full 135×240 panel, ~20 tube-updates/s across
five tubes with PAL4) and bytes still cost (4 KB RLE frames drew at 33/s vs 16 KB PAL4 at
20/s). So:

- **Row-band diffs** (default; `--no-band` to disable). Per tube the bridge keeps the RAW565
  rows it last sent. Each frame it finds the first and last changed row; nothing changed →
  nothing sent; a band shorter than 60 % of the tile → a partial `BLIT` with that `y`/`h`
  (palette + just those rows for PAL4); otherwise the full tile. The memory resets when that
  tube's socket reconnects. The stats line shows the average `band %`. The diff is computed on
  the *source* rows (at RGB565 precision), before dithering, so a dithered static area still
  sends nothing; the price is that rows outside the band keep the palette they were drawn with.
- **PAL4x2** (`--fmt pal4x2`, wire fmt 3): the tile is box-filtered to 135×120 (a real 2:1
  average, not dropped rows), quantised, and sent as `ceil(h/2)` PAL4 rows the node draws
  twice. Half the bytes and half the decode; SPI unchanged. Works with band diffs — the band
  `y` is even, `h` = 2 × payload rows, clamped at the tile bottom.
- **Dithering** (`--dither`, PAL4/PAL4x2 only): `fs` (default) Floyd–Steinberg, `bayer` a 4×4
  ordered dither applied in RGB before quantising, `none`. Pillow's `quantize()` never dithers
  by itself, so the bridge does median-cut for the palette and a second palette-mapped pass for
  the dither. Trade-off: dither noise makes RLE bigger and would enlarge band diffs if they were
  computed on the quantised output (they aren't, see above); on 16 colours it's what makes
  gradients and skin tones look right.
- **Adaptive pacing** (`--fps auto`, default; ceiling `--max-fps 15`, floor 2). Once a second the
  bridge looks at the last ~2 s: if any tick dropped updates (a socket still had two unacked
  frames when the next tick was due) the all-tubes rate is set to 90 % of what the node served
  (acks/s ÷ updates wanted per tick); otherwise it ramps up 15 %/s toward the ceiling. The stats
  line shows `fps <achieved> (auto <target>)`; `--fps N` is a fixed override and `--fps 0` is
  "as fast as the acks allow" (what `bench` uses).

## Connections: one socket per tube

Measured on the node, a single TCP connection tops out at ~280 KB/s — lwIP's fixed ~5.7 KB
receive window with ~10 ms RTT — while the node itself can take ~1.6 MB/s. The firmware
accepts up to six clients on :5555 with independent draw state, so the bridge **defaults to
one TCP connection per populated tube**: tube *k* gets its own socket (own `HELLO`, own
seq/ack bookkeeping, `TCP_NODELAY`, ≤2 frames in flight, own 2 s ack timeout → own reconnect
in the background). `--conns N` caps that (`--conns 1` = a single socket; tubes share sockets
round-robin). Serial is always one link.

Each tick first sends to every socket that has room, then waits only on the sockets that
were busy until the next tick is due, and drops whatever is still blocked (`drop` counts tube
frames). A slow socket costs its own tube frames and never delays the others; tubes that
share a socket get the tick's slack instead of starving, and the send order rotates each
tick so shared-socket drops are spread evenly.

Tube order: native index 0 is the far-**right** tube, 5 the far-left, so physical left→right
is `5,4,3,2,1,0`. The populated set is read from `GET /status` (`populated_mask`, default
`0x3E` = tube 0 empty) unless you pass `--tubes`.

### Over the USB cable (`--transport auto`, the default)

If the node doesn't answer on `--host:--port` within 2 s and a `/dev/cu.usbserial-*` port exists, the
bridge streams over the cable instead (and says so). `--transport serial` forces it, `--transport tcp`
never falls back. The link opens at the node's boot rate (115200 — opening the port resets the clock,
so it waits out the boot log), then asks the shell for `--baud` (921600 by default) and switches; if
the node came up at a one-shot rate (`baud N once`) it finds that rate by probing. Serial is one link for
all tubes with the same ESP/1 frames and acks. **Measured on this clock's USB-UART chip:** 921600 garbles
(the node's rate watchdog reverts it by itself), **460800 works — ~40 KB/s, 5 tube-updates/s, ~350 ms
ack** (vs ~10 KB/s at 115200). `--baud` defaults to 460800; each rung is verified with a `help`
round-trip before it's used. One owner per port: a helper tab with its 🧷 serial panel
connected, `monitor.sh` or `flash.sh` will make the open fail with "Resource busy".

## Sources

| | |
|---|---|
| `webcam:N` | avfoundation camera N (`GET /bridge/sources` lists them), 30 fps in, cover-fit |
| `screen:N` | the Mac's display N ("Capture screen N"), ≤10 fps in; `--crop x,y,w,h` picks a region (macOS asks for screen-recording permission the first time) |
| `page:URL` | headless Chrome (`--headless=new --disable-gpu --hide-scrollbars --force-device-scale-factor=1 --window-size=W,H --screenshot`) every `--page-every` s (default 5), W×H = all the tubes together (135×240 per tube in tile layout); an unchanged page sends nothing — see notes below |
| `file:PATH` | any video / GIF / HEVC, real-time; `--loop` repeats |
| `url:URL` | `yt-dlp -o - -f "best[height<=480]/best"` piped into ffmpeg's stdin, after probing which player client streams (`YT_CLIENTS`); `--loop` restarts at the end — see notes below |
| `dir:PATH` | numbered PNG/JPG frames looped at 12 fps (or `--fps N`); sub-folders = one sequence per tube, rotating every `--rotate-every` s; PNGs with an alpha channel are composited over `--bg` like the characters |
| `chars:SET` | the turntables in `samples/SET/manifest.json` (`dwarves`, `island`, `winter`; `GET /bridge/sources` lists them): one character per tube, the cast shifts one tube along every `--rotate-every` s (default 8); `--chars a,b,c` picks who plays. `dwarves` alone still means `chars:dwarves`. The frames are transparent RGBA (just the figure and its semi-transparent floor shadow); `--bg` picks what goes behind them — see *Behind the characters* below. Render with `tools/render_characters.py` |
| `test` | scrolling colour bars + bouncing ball drawn in Python (no ffmpeg) — every tube changes every frame, for `bench` |

**`url:` (YouTube).** Google gates media URLs per player client (PO tokens), so the old
`yt-dlp -g` → ffmpeg path now gets HTTP 403. The bridge streams `yt-dlp -o -` into ffmpeg's stdin
(yt-dlp keeps its own client headers) and first probes which player client streams today
(`YT_CLIENTS = ("mweb", "default")`, 64 KB each; as of 2026-09 `mweb` streams while the default web
clients 403). When a source ends with no frames the bridge prints ffmpeg's last stderr lines. If it
fails: `brew upgrade yt-dlp` first (the client list moves — 2026.8.19 is out vs 2026.7.4 installed),
or share the YouTube tab from the helper's 📹 Live panel: `getDisplayMedia` needs no yt-dlp at all.
Measured on the clock (5 tubes, PAL4x2 + Bayer, span): 480p → 26–30 tube-updates/s ≈ 5.5 fps per
tube, ~200 KB/s, node busy 68 %; the same pipeline does 75 upd/s (15 fps) against `--fake`, so the
node's draw path is the limit, not the source.

**`page:`.** Chrome's first launch after an update can take > 30 s — the installed Google Chrome is
the x64 build running under Rosetta on this Apple-silicon Mac (Chrome's own log calls that neither
tested nor maintained); install the arm64 build. The bridge waits 60 s and puts Chrome's exit code in
the error. A static page yields exactly one update per tube and then nothing — the band diff sends
nothing for identical screenshots — so 0 upd/s is not a stall; read `(total N)` on the stats line.
`samples/page-demo.html` shows seconds so it visibly ticks every screenshot.

**`--bg` — behind the characters.** The rendered frames have a transparent background (the
renderer draws only the figure and a semi-transparent black floor shadow), so the bridge
composites each frame over a background *before* anything else sees it — once, when the
sequence loads, so the stream loop, band diffs and quantisation still work on opaque RGB
tiles. The grammar, exactly:

| spec | |
|---|---|
| `black` | the default — the look the sets have always had on the tubes |
| `#rrggbb` | one solid colour |
| `#rrggbb-#rrggbb` | a vertical gradient: first colour at the top of the tube, second at the bottom |
| `glow:#rrggbb` | a radial glow of that colour behind the figure, fading to black at the tube edges |

Hex digits are case-insensitive; nothing else is accepted (a bad spec exits with the grammar
printed, or gets a 400 from `POST /bridge/start`). Quote it in the shell — `#` starts a comment.
The floor shadow darkens whatever is behind the figure, so over `black` it is simply invisible.
`dir:` sources honour `--bg` too when their PNGs carry alpha; every other source ignores it.

## Control API (`serve`)

| | |
|---|---|
| `GET /bridge/status` | `{"bridge":"esptube","version":1,"usb":{"port","connected"},"tcp":{"host","connected","conns","conns_total"},"streaming","source","layout","fmt","dither","bg","fps","fps_target","kbps","band_pct","dropped"}` |
| `GET /bridge/sources` | `{"sets":[{"id":"winter","title":"Winter kingdom","characters":[{"id":"icequeen","label":"Ice queen"},…]},…],"webcams":[{"index","name"}],"dwarves":bool,"chrome":bool,"ffmpeg":bool}` — `sets` is read from `samples/*/manifest.json` on every call |
| `POST /bridge/start` | JSON `{"source","layout","fmt","fps","gap","loop"}` plus optional `dither`, `band`, `max_fps`, `conns`, `tubes`, `page_every`, `crop`, and for `chars:` sources `chars` (`"a,b"`, default everyone), `rotate_every` (seconds per shift) and `bg` (what goes behind the transparent frames — same grammar as `--bg`, default `"black"`; `dir:` honours it too; other sources ignore all three) — stops any running stream first. `source` is any CLI spec: `"chars:winter"`, `"dwarves"`, `"webcam:0"`, `"test"`, … |
| `POST /bridge/stop` | |

All responses carry `Access-Control-Allow-Origin: *`; `OPTIONS` is answered, so `helper.html`
can call it from `file://`. `usb.port` is the first `/dev/cu.usbserial-*` (re-checked per call).

## Bench

`bench` streams the test pattern (every tube changes every frame, so band diffs are full
tiles) at max rate for 10 s per format over the same connection layout `play` would use:

```
pal4   x5 conn:   78.0 tube-updates/s =  15.6 fps × 5 ·    1250 KB/s · 16.0 KB/update · ack 41.2 ms
```

Against the real node that is the transport + draw ceiling; `--conns 1` shows the
single-socket cap. For real content sizes, run `play dwarves --fps 12` for a while and read the
`summary:` line (KB/update, KB/frame, band %).

## Testing without the clock

`--fake` (or `ESPTUBE_FAKE=1`) starts an in-process fake node — a threaded TCP server on
127.0.0.1 with one client state per connection — that parses every frame, validates it
(magic, op, window incl. partial bands and even `y` for PAL4x2, payload length for all four
formats, RLE pixel count, PAL4 row alignment and pad nibbles) and acks it like the firmware.
Knobs: `ESPTUBE_FAKE_KBPS=280` caps each connection's throughput (the lwIP window);
`ESPTUBE_FAKE_MS=25` is the draw time per full blit (scaled by rows drawn and serialized
across connections, because the node has one CPU — so 25 ms ≈ 40 tube-updates/s ≈ 8 all-tubes fps
on five tubes; a comma list applies per client in order, cycling — `0,0,150` makes every
third connection's blits slow, `0,0,3000` makes it miss the 2 s ack timeout).

Band diffs on the character turntables stay at `band 100%` — a rotating figure changes rows
23–217 (≈80 % of the tile) between consecutive frames, above the 60 % threshold — so their
saving shows up on content with localized motion (tickers, clocks, mostly-static pages), not
on turntables; `pal4x2` is what halves the turntables.

```bash
./bridge.py play test --fake --fmt pal4x2
./bridge.py play chars:winter --fake --fmt pal4x2 --dither fs
./bridge.py play chars:island --chars blue,pink --fake --rotate-every 4
./bridge.py play chars:island --fake --bg '#102040-#000000'   # the cast over a gradient
ESPTUBE_FAKE_MS=25 ./bridge.py play dwarves --fake        # ~the real node's draw time: watch auto fps settle
./bridge.py bench --fake                                   # Mac encode speed, not the node
./bridge.py doctor --fake
```

Bench numbers against an unthrottled fake node are an upper bound (encode-only); read
`/status.stream` on the device for the truth.

## v2.0 — the cable, the console, the layout
- **USB first.** `--transport auto` takes a free `/dev/cu.usbserial-*` before WiFi (`--transport tcp` forces
  WiFi). With firmware ≥ 2.0 the serial link speaks ESP/1.1 **datagrams**: `00 COBS(frame+crc16) 00`, tiles cut
  into self-contained bands, NOACK except the last, framed acks, the node's epoch adopted from acks (status 4 →
  full tiles). Older firmware: legacy framing, automatically.
- **Baud ladder** `--baud` default **1500000** → 1000000 → 460800 → 230400 → 115200, each rung verified (`help`
  and, at ≥ 1 M, three clean `status` replies). Measured on the CH340: 1.5 M = 110–136 KB/s, 15–18 tile-updates/s.
- **CANCEL** on stop / source end, so nothing of the bridge's is still drawing after it quits.
- **`--geometry FILE|URL|clock`**: the span seam gap from the real tube pitch (`esptube.layout/1`, written by the
  helper's 🧊 Stage; `clock` = `http://<host>/layout.json`).
- **`serve` also starts the console** at `http://localhost:8765` (same as `python3 tools/console.py`): a secure
  context, so camera / screen / tab capture work in the helper. Needs pyserial for the cable
  (PlatformIO ships one: `head -1 $(which pio)`).

