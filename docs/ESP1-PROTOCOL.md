# ESP/1 — the ESPTube stream protocol

*The persistent binary channel that carries frames into the tubes, replacing one HTTP request per
frame. This is the wire format every client implements: the helper's 📹 Live panel, the Mac bridge
(`tools/bridge/bridge.py`), and the test clients in `test/`. Firmware: `src/esp1.*` (parser +
TCP/UDP/WebSocket servers) and `src/shell.*` (the UART side).*

## 1. Transports

| Transport | Port / device | Notes |
|---|---|---|
| **TCP** | `:5555` | Up to **six** clients — one connection per tube is the fast pattern (each connection gets its own lwIP receive window). Reliable; the recommended path for scripts and the bridge. |
| **WebSocket** | `:5557` | For the browser helper. **Binary** frames carry exactly the same bytes as TCP; **text** frames are shell commands and get text replies. |
| **UDP** | `:5556` | Same byte stream in ≤ 1472-byte datagrams, acks back to the sender. Measured 10–60 % datagram loss on WiFi — **not usable** without adding reliability; kept for experiments. |
| **USB UART** | `/dev/cu.usbserial-*` @ 115200 | Same bytes. Text lines that do not start with `0xE5` are shell commands. Opening the port **resets the board** (DTR pulse from the macOS driver) — expect a ~6 s reboot. |

All integers are little-endian unless stated. The node draws rows **as they arrive** (no frame
buffer) and acks each frame once it is fully drawn.

**Auth (optional).** If the node has a token (`POST /config/token {"token":"…"}`, NVS), the client's
first bytes on a TCP/WebSocket connection must be `HELLO <token>\n`; the node answers `OK\n` or
closes. No token = open on the LAN, like the REST API. Physical access is the security model for the
UART.

## 2. Frame header — 20 bytes

| off | size | field |
|---|---|---|
| 0 | 2 | magic `E5 7B` |
| 2 | 1 | op: `1` BLIT · `2` FILL · `3` PING · `4` LED · `5` CLEAR |
| 3 | 1 | tube 0–5 (`0xFF` = all, for FILL/CLEAR) |
| 4 | 2×4 | x, y, w, h (u16) — BLIT/FILL window in panel pixels, clamped by the node |
| 12 | 1 | fmt: `0` RAW565 · `1` RLE565 · `2` PAL4 · `3` PAL4x2 (row-doubled) |
| 13 | 1 | seq (echoed in the ack) |
| 14 | 1 | **gen** (ESP/1.1): the node's content epoch the sender believes in; `0` = legacy / "adopt whatever it is" |
| 15 | 1 | **flags** (ESP/1.1): `0x01` NOACK · `0x04` CLEAR (on CANCEL) · `0x80` V11 ("put the epoch in ack byte 3") |
| 16 | 4 | len — payload bytes (u32) |

## 3. Payloads

- **`RAW565`** (fmt 0): w·h pixels, 2 bytes each, **high byte first**, row-major, top-down — the same
  bytes as `POST /tube/{i}/raw565`.
- **`RLE565`** (fmt 1): runs of `{count u8 (1–255), hi, lo}`; runs may cross rows; total pixels = w·h.
  Best for flat 2–3-colour content (tickers, text).
- **`PAL4`** (fmt 2): 32 bytes = 16 palette entries as RGB565 high-byte-first, then rows of
  `ceil(w/2)` bytes, high nibble = left pixel, **each row byte-aligned**. A per-tube 16-colour palette
  with dithering looks fine on 135×240 glass and is 4× smaller than RAW.
- **`PAL4x2`** (fmt 3): like PAL4 but only `ceil(h/2)` payload rows; the node draws each row **twice**
  (line doubling — half the bytes and decode for the same picture height). Downsample vertically with
  a filter, not by dropping rows. Window `y` should be even. The right default for video.
- **`FILL`**: 2 bytes RGB565 (fills the window; tube `0xFF` = every live tube).
- **`LED`**: 18 bytes — r,g,b for tubes 0..5 (underglow).
- **`PING`**, **`CLEAR`**: no payload.

## 4. Ack — 4 bytes, node → client

Sent after the frame is fully drawn: `A5, seq, status, 0` with status `0` ok · `1` bad tube/window ·
`2` bad fmt · `3` short payload. Clients keep **≤ 2 frames in flight** per connection and pace on
acks. Latency ≈ transfer time + SPI time for that frame.

Connection slots: with all six TCP slots held, a new connection evicts the **least-recently-active**
slot (per-slot last-byte timestamp), so a controller reconnecting all its tubes never evicts its own
fresh sockets.

## 5. Shell (text, `\n`-terminated)

Over the UART, or as WebSocket text frames: `help` · `status` · `preset N|next` · `nixie TEXT` ·
`flash TEXT` · `scroll TEXT` · `countdown N` · `stop` · `led off|solid|rainbow|breathe|comet` ·
`bright N` · `mode clock|off|manual` · `wifi` · `sweep [from to fade]` · `fps` · `debug on|off` ·
`baud N` · `token X` · `reboot`. Replies are single lines; `status` returns the same JSON as
`GET /status`. `tools/bridge/bridge.py shell /dev/cu.usbserial-XXXX` is an interactive client.

## 6. Counters

`GET /status` → `stream`: `port`, `tcp` (a client is connected), `token` (auth on), `fps`, `kbps`,
`frames`, `bad`, `last_ms_ago`, `clients`, `busy_pct` (% of time inside the parser/draw path),
`loops_per_s`. The `fps` shell command prints the same, and the on-device menu's **FPS** item shows
the current rate in nixie letters.

## 7. Clients that speak it

- **Helper 📹 Live** (`tools/helper.html`): camera / screen-or-tab share / video & GIF files /
  animation studio → 16-colour median cut + Bayer dither → PAL4x2 → changed-row bands → one WebSocket
  per tube; clocked by a Web Worker (page timers throttle in background tabs) and by acks.
- **Mac bridge** (`tools/bridge/bridge.py`): ffmpeg / yt-dlp / headless-Chrome sources → per-tube
  PAL4 / PAL4x2 / RLE / RAW → one TCP connection per tube; `--fps auto` follows the node's acks.
- **Tests:** `test/esp1-bench.py` (TCP throughput per format), `test/esp1-ws.py` (WebSocket),
  `test/esp1-serial.py` (UART frames + shell).

## 8. Measured on this clock (ESP32-WROOM-32D, 5 live tubes, 2026-09-17)

| Path | Result | Notes |
|---|---|---|
| WiFi TCP, 1 connection | ~280 KB/s | lwIP's fixed 5.7 KB receive window ÷ ~10 ms RTT (WiFi power save off; with it on, ~100 ms) |
| WiFi TCP, 5 connections (one per tube) | ~300–460 KB/s | the window is no longer the limit; the node's draw path is |
| Node draw cost | **~25 ms per full-panel frame** → ~30 tube-updates/s max | decode + 30 × 8-row SPI pushes at 40 MHz; `busy 82%` on 4 KB RLE frames |
| Real content (dwarves, PAL4 or RLE, 5 tubes) | **~16–20 tube-updates/s ≈ 4 fps per tube** | both limits coincide here |
| UDP | node drains 1.6 MB/s in bursts, but 10–60 % datagram loss on WiFi | not usable without reliability on top |
| UART shell @115200 | works | the 256 B default RX buffer dropped frame bytes → now 16 KB |
| UART frames @115200 | 10.2 KB/s (89 % of line rate), correct | opening the port resets the board (DTR) |
| UART, legacy frames, stop-and-wait | 460800: 33 KB/s · 1 M: 57 KB/s | what 1.x did; every tile waits for its ack |
| **UART, ESP/1.1 datagrams** (§9) | **1 M: 88 KB/s · 1.5 M: 119 KB/s · 2 M: 149 KB/s** | CH340 bridge, RX FIFO threshold 32. 921600 does not work; 2 M carries CRC-checked pixels but drops bytes in ~1 of 5 long text replies, so the verified default is **1.5 M** (14 tile-updates/s, ping 3 ms) |
| DMA blit flushes | node **crashed** (`panic`) with the vendored TFT_eSPI fork | code kept behind `g_dma=false` |
| 80 MHz SPI (`esptube_spi80`) | **no gain** (15.8 vs 16.4 PAL4 updates/s), glass fine | the draw path is CPU/overhead-bound, not clock-bound; kept at 40 MHz |
| 16-row blocks, tube kept selected across blocks, decode straight into the SPI buffer | PAL4 unchanged (16.3/s) | the fixed SPI transfer (13 ms/panel) dominates once overhead is gone |
| **PAL4x2** (line doubling) | **24.2 updates/s (+50 %) at 8 KB/frame** | half the bytes and decode for the same SPI cost |
| RLE (flat art) | 28 updates/s | best for 2–3-colour tickers/text |
| **Helper 📹 Live → WebSocket :5557** (animation studio, PAL4x2 + Bayer, changed-row bands ≈ 56 % of rows) | **35 tube-updates/s ≈ 6.3 fps per tube × 5, 145 KB/s** | the best rate so far — bands + line doubling; JS encode ≈ 1 ms/tile |
| **YouTube 480p via `yt-dlp -o -` → ffmpeg → bridge** (5 tubes, PAL4x2 + Bayer, span, one TCP connection per tube) | **26–30 tube-updates/s ≈ 5.5 fps per tube, ~200 KB/s** | node `busy 68%`, ~1 dropped tick/s, ack 200–400 ms |
| Same pipeline against the bridge's `--fake` node (no draw cost) | 15 fps = 75 updates/s, 600 KB/s | the source/encode side has ~3× headroom: the node's draw path is the ceiling |
| Timer throttling | page timers ran at ~1.5/s in an unfocused pane | the live loop is clocked by a Web Worker metronome + acks |
| Stream task placement | core 1 at loop priority + 1-tick yield | core 0 starved acks; no yield tripped the task watchdog |

**Bottom line for this clock:** ~4 fps per tube across five tubes for arbitrary video (~5.5 with
PAL4x2 + band diffs, ~6.3 from the browser's Live panel), ~7 fps for flat 2-colour content, 100 %
reliable over WiFi-TCP, with the shell and frames also working over USB at 115200. What is left on
the draw path is incremental (leaner decode, DMA if the fork can be fixed — maybe 1.5×). Send fewer
pixels, not faster bits: palette frames, line doubling and changed-row bands are what bought the
frame rate.

## 9. ESP/1.1 — epoch, CANCEL, datagrams (firmware ≥ 2.0)

Backward compatible: a client that leaves bytes 14–15 at zero behaves exactly as before, including ack
byte 3 = 0. Everything below switches on with a non-zero `gen` or `flags`.

**The epoch — "newer content stops older content".** The node owns a content epoch (1…255, never 0).
A clear, a mode/face change or a CANCEL bumps it *before* taking the panel lock, and every blit checks
it **inside its locked 16-row SPI flush** — so a frame that is still arriving stops drawing at the next
block instead of repainting over the clear.

| header `gen` | node |
|---|---|
| `0` | legacy: adopt the current epoch |
| `== epoch` | draw |
| `== succ(epoch)` (255 → 1) | the sender already knows about a supersede: advance, then draw |
| anything else | stale: skip the payload, ack **status 4**, ack byte 3 = the true epoch (adopt it, resend as a full tile) |

The counter is the node's, not the client's, and the compare is equality — two writers (helper + bridge,
legacy + 1.1) never starve each other.

**New ops.** `6` CANCEL (len 0): bump the epoch (stale `gen` → status 4, so a late duplicate cannot
cancel newer content); with flag `0x04` also run the master clear. Send it on its **own** connection —
queued behind 300 KB of pixels it is not a cancel (measured: 985 ms → 57–110 ms). `7` ECHO (≤ 4 KB):
reply `A7 seq crcHi crcLo lenLo lenHi epoch 00`, the CRC16 the node computed over what it received.

**Flags.** `0x01` NOACK — no ack when the frame drew fine (errors are always acked). `0x80` V11 — for a
client that does not know the epoch yet: `PING` with this bit returns it in ack byte 3.

**Ack status.** `0` ok · `1` bad tube/window · `2` bad fmt/op · `3` bad length · `4` superseded · `5` bad CRC
(datagrams). A BLIT length is checked exactly per format; a length no frame can have (> 97 200) is
answered with status 3 at once and the parser hunts for the next magic — it can no longer be wedged. A
socket that stalls mid-frame for 5 s is closed.

**Datagrams on the UART.** One byte stream carries shell text, legacy frames (`E5` at the start of a
line) and datagrams: `00 <COBS(frame + crc16)> 00` (CRC-16/CCITT-FALSE over the frame). `0x00` never
occurs in shell text, so it delimits; an empty frame keeps the framer in datagram state, which makes a
lost opener *or* closer heal within one datagram. A damaged datagram is dropped (status 5 / 3), never
desyncs the stream. Replies to datagrams come back framed the same way, in one write. Senders cut a
tile into self-contained bands of ≤ ~1.15 KB (RAW by rows, PAL4/PAL4x2 16 payload rows + the palette,
RLE at row boundaries), NOACK on all but the last, and keep ≤ ~40 ms of bytes queued — the datagram is
the cancel unit. UDP (port 5556) uses the same rule: one datagram = one complete frame.

Implementation: `firmware/custom-fw/src/esp1_frame.h` (header-only, fuzz-tested natively by
`test/esp1-frame-native.sh`). Tests against a clock: `test/esp1-cancel.py` (WebSocket, no cable),
`test/esp1-dgram.py` and `test/serial-baud-probe.py` (cable).
