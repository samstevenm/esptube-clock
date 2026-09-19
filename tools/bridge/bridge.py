#!/usr/bin/env python3
"""bridge.py — Mac-side bridge that streams video into the ESPTube tubes (ESP/1).

Sources on the Mac (webcam, screen, web page, files, YouTube, frame folders, the rendered
character sets, a synthetic test pattern) -> ffmpeg / Chrome / Pillow -> one 135x240 tile per
tube -> row-band diff against what that tube already shows -> encode (PAL4 · PAL4x2 · RLE565 ·
RAW565, dithered) -> ESP/1 BLITs over TCP :5555 (one socket per tube) or the USB serial port,
paced to what the node actually draws.  Wire format: docs/ESP1-PROTOCOL.md.

The node's draw path is CPU-bound (~25 ms per full panel, and bytes still cost), so the bridge
sends fewer pixels and fewer bytes per update:
  * band diffs — per tube it keeps the RAW565 rows it last sent; only the first..last changed
    rows go out (a partial BLIT with y/h) unless the band is >= 60 % of the tile.
  * PAL4x2 (fmt 3) — the tile is box-filtered to 135x120, sent as PAL4 rows the node draws twice.
  * --dither fs|bayer|none — Floyd-Steinberg (default) or a 4x4 ordered dither before the
    16-colour quantisation. Dither noise makes RLE bigger; band diffs are computed on the
    *source* rows (pre-dither), so a dithered static area still sends nothing.
  * --fps auto (default) — the tick rate follows the node: when a tick drops updates the
    rate is set to 90 % of what the node served, otherwise it ramps up to --max-fps.

Connections: lwIP's ~5.7 KB receive window caps one TCP socket at ~280 KB/s, so the default is
one socket per populated tube (own HELLO, seq, acks, reconnect); --conns N caps that. A tick
sends to every socket with room, waits only on the busy ones until the next tick is due, then
drops what is still blocked — a slow socket costs its own tube frames, never the others'.

Needs: Python 3.9+, Pillow. ffmpeg (+ yt-dlp for url:) on PATH for media sources; Google Chrome
for page:; pyserial only for --serial / shell (the esptool Homebrew python has it:
/opt/homebrew/Cellar/esptool/5.4.0/libexec/bin/python3).  `bridge.py doctor` checks all of it.

Usage:
  bridge.py play SOURCE [conn] [--layout tile|span] [--fps auto|N] [--max-fps 15]
                        [--fmt pal4|pal4x2|rle|raw] [--dither fs|bayer|none] [--no-band]
                        [--gap 0] [--tubes 5,4,3,2,1] [--loop] [--rotate-every 8]
                        [--chars a,b,c] [--bg black|#rrggbb|#rrggbb-#rrggbb|glow:#rrggbb]
                        [--page-every 5] [--crop x,y,w,h]
  bridge.py serve [conn] [--api-port 8787]         # localhost control API for the helper
  bridge.py bench [conn] [--seconds 10]            # raw / rle / pal4 / pal4x2 throughput
  bridge.py doctor [conn]                          # ffmpeg / yt-dlp / Chrome / pyserial / USB / node
  bridge.py shell --serial PORT [--baud 115200]    # text shell on the node's UART
  conn:  [--host esptube.local] [--port 5555] [--conns N] [--serial PORT --baud 921600]
         [--token T] [--chrome PATH] [--fake]

SOURCE:  webcam:N | screen:N | page:URL | file:/path | url:https://... | dir:/path | chars:SET | test
  webcam:N  avfoundation camera N     (list: bridge.py serve -> GET /bridge/sources)
  screen:N  the Mac's display N ("Capture screen N"), low fps; --crop x,y,w,h for a region
  page:URL  headless Chrome screenshot of a web page every --page-every seconds
  file:     any video / GIF / HEVC; --loop repeats it
  url:      resolved with yt-dlp (best <=480p) and fed to ffmpeg
  dir:      a folder of numbered PNG/JPG frames, looped (sub-folders = one sequence per tube)
  chars:SET samples/SET turntables (dwarves | island | winter — GET /bridge/sources lists them):
            one character per tube, the cast shifts one tube every --rotate-every s;
            --chars a,b,c picks who plays.  `dwarves` is short for chars:dwarves.
            The frames are transparent RGBA; --bg puts something behind them: black (default),
            a #rrggbb colour, a #rrggbb-#rrggbb vertical gradient (top-bottom) or glow:#rrggbb,
            a radial glow of that colour over black.  Render with tools/render_characters.py.
  test      colour bars + bouncing ball drawn here (no ffmpeg) — for benchmarks

Tubes: native index 0 = far RIGHT ... 5 = far LEFT, so physical left->right is 5,4,3,2,1,0.
Populated tubes come from GET http://<host>/status (populated_mask) or --tubes.

--fake (or ESPTUBE_FAKE=1) runs an in-process fake node (threaded TCP server on 127.0.0.1,
one client state per connection) that parses and validates every frame (header, window,
lengths, pixel counts, PAL4/PAL4x2 row alignment) and acks it, so the pipeline runs without
hardware.  ESPTUBE_FAKE_MS=25 simulates the node's draw time per full BLIT (scaled by rows,
serialized across connections like the node's one CPU; a per-client comma list works too);
ESPTUBE_FAKE_KBPS=280 caps each connection's throughput like the lwIP window.

Control API (serve, 127.0.0.1:8787, CORS *):
  GET  /bridge/status
  GET  /bridge/sources    {"sets":[{"id","title","characters":[{"id","label"}]}],"webcams":[...],...}
  POST /bridge/start  {"source","layout","fmt","fps","gap","loop"[,"dither","band","max_fps","conns",
                       "chars" (chars: sources only, "a,b"), "rotate_every" (seconds),
                       "bg" (chars:/dir: sources, same grammar as --bg)]}
  POST /bridge/stop
"""
import argparse, collections, glob, json, math, os, re, select, shutil, socket, struct
import subprocess, sys, tempfile, threading, time, urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from PIL import Image, ImageChops, ImageDraw, ImageOps

try:
    import serial                                 # pyserial — optional (only --serial / shell)
except ImportError:
    serial = None

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))     # repo root = two levels up from tools/bridge
SAMPLES = os.path.join(ROOT, "samples")           # samples/<set>/manifest.json = a character set
DWARVES_MANIFEST = os.path.join(SAMPLES, "dwarves", "manifest.json")
CHROME = "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"
SERIAL_HINT = ("pyserial is not installed for this python; serial features are off.\n"
               "  Run with one that has it, e.g. /opt/homebrew/Cellar/esptool/5.4.0/libexec/bin/python3")
FFMPEG_HINT = ("ffmpeg is needed for webcam/file/url sources — install with:  brew install ffmpeg yt-dlp"
               "   (or https://ffmpeg.org/download.html)")
YTDLP_HINT = "yt-dlp is needed for url: sources — install with:  brew install ffmpeg yt-dlp"
CHROME_HINT = "install Google Chrome (or set --chrome /path/to/chrome)"

TW, TH = 135, 240                       # one tube panel
TH2 = (TH + 1) // 2                     # PAL4x2 payload rows
PHYSICAL = [5, 4, 3, 2, 1, 0]           # native tube indices, left -> right as you face the clock
DEFAULT_MASK = 0x3E                     # bit i = tube i populated; default: far-right slot empty
OP_BLIT, OP_FILL, OP_PING, OP_LED, OP_CLEAR, OP_CANCEL, OP_ECHO = 1, 2, 3, 4, 5, 6, 7
F_NOACK, F_CLEAR, F_V11 = 0x01, 0x04, 0x80      # ESP/1.1 header byte 15 (byte 14 = gen, the node's epoch)
FMT_RAW, FMT_RLE, FMT_PAL4, FMT_PAL4X2 = 0, 1, 2, 3
FMTS = {"raw": FMT_RAW, "rle": FMT_RLE, "pal4": FMT_PAL4, "pal4x2": FMT_PAL4X2}
DITHERS = ("fs", "bayer", "none")
HDR = struct.Struct("<BBBBHHHHBBHI")    # E5 7B op tube x y w h fmt seq rsvd len  = 20 bytes
HDR11 = struct.Struct("<BBBBHHHHBBBBI")  # ESP/1.1: ... seq gen flags len
DG_BAND_BYTES = 1150                    # one datagram ~ one band: the cancel unit over the cable (~8 ms at 1.5 Mbaud)

def crc16(b, crc=0xFFFF):
    """CRC-16/CCITT-FALSE, same as firmware esp1_frame.h ("123456789" -> 0x29B1)."""
    for x in b:
        crc ^= x << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc

def cobs_enc(b):
    out = bytearray([0]); code_at = 0; code = 1
    for x in b:
        if x == 0:
            out[code_at] = code; code_at = len(out); out.append(0); code = 1
        else:
            out.append(x); code += 1
            if code == 0xFF:
                out[code_at] = code; code_at = len(out); out.append(0); code = 1
    out[code_at] = code
    return bytes(out)

def cobs_dec(b):
    out = bytearray(); i = 0
    while i < len(b):
        c = b[i]; i += 1
        if c == 0 or i + c - 1 > len(b): return None
        out += b[i:i + c - 1]; i += c - 1
        if c != 0xFF and i < len(b): out.append(0)
    return bytes(out)

def datagram(frame):
    """00 <COBS(frame + crc16)> 00 — self-delimiting and CRC-checked: a damaged one is dropped, never desyncs."""
    return b"\0" + cobs_enc(frame + struct.pack(">H", crc16(frame))) + b"\0"

def split_bands(fmt, payload, y, h):
    """One BLIT window -> [(y, h, payload)] self-contained bands of <= ~DG_BAND_BYTES, each a valid BLIT."""
    if len(payload) <= DG_BAND_BYTES + 250:
        return [(y, h, payload)]
    out = []
    if fmt == FMT_RAW:
        rb = TW * 2; per = max(1, DG_BAND_BYTES // rb)
        for r0 in range(0, h, per):
            n = min(per, h - r0); out.append((y + r0, n, payload[r0 * rb:(r0 + n) * rb]))
    elif fmt in (FMT_PAL4, FMT_PAL4X2):
        pal, body, rb = payload[:32], payload[32:], (TW + 1) // 2
        rows = len(body) // rb; per = 16; mul = 2 if fmt == FMT_PAL4X2 else 1
        for r0 in range(0, rows, per):
            n = min(per, rows - r0)
            out.append((y + r0 * mul, min(n * mul, h - r0 * mul), pal + body[r0 * rb:(r0 + n) * rb]))
    else:   # RLE565: runs may cross rows — cut at row boundaries once a band is big enough (splitting a run if needed)
        cur = bytearray(); px = 0; row0 = 0
        for i in range(0, len(payload) - 2, 3):
            cnt, hi, lo = payload[i] or 1, payload[i + 1], payload[i + 2]
            while cnt:
                room = TW - (px % TW)
                if len(cur) >= DG_BAND_BYTES - 200 and cnt >= room:          # finish this row, then cut
                    cur += bytes((room, hi, lo)); px += room; cnt -= room
                    rows = px // TW; out.append((y + row0, rows, bytes(cur))); row0 += rows; cur = bytearray(); px = 0
                else:
                    cur += bytes((cnt, hi, lo)); px += cnt; cnt = 0
        if px: out.append((y + row0, px // TW, bytes(cur)))
    return out
ACK_TIMEOUT = 2.0
MAX_INFLIGHT = 2                        # per connection
BAND_MAX = 0.60                         # a changed band this tall (or taller) goes as a full tile
BAYER_STRENGTH = 48                     # peak-to-peak RGB offset of the ordered dither
BG_DEFAULT = "black"                    # what transparent frames (chars:, RGBA dir:) are composited over
BG_GRAMMAR = ("--bg wants  black | #rrggbb | #rrggbb-#rrggbb (vertical gradient, top-bottom) | "
              "glow:#rrggbb (radial glow of that colour over black)")


# ---- encoders --------------------------------------------------------------------------------
def rgb565(r, g, b):
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)

_T_RH = bytes(v & 0xF8 for v in range(256)); _T_GH = bytes(v >> 5 for v in range(256))
_T_GL = bytes((v & 0x1C) << 3 for v in range(256)); _T_BL = bytes(v >> 3 for v in range(256))
_T_NIB = bytes((v & 15) << 4 for v in range(256))

def encode_raw(im):
    """RAW565: 2 bytes/pixel, HIGH byte first, row-major top-down. Per-channel LUTs, no pixel loop."""
    r, g, b = (c.tobytes() for c in im.split()); n = len(r)
    hi = (int.from_bytes(r.translate(_T_RH), "big") | int.from_bytes(g.translate(_T_GH), "big")).to_bytes(n, "big")
    lo = (int.from_bytes(g.translate(_T_GL), "big") | int.from_bytes(b.translate(_T_BL), "big")).to_bytes(n, "big")
    return Image.merge("LA", (Image.frombytes("L", im.size, hi), Image.frombytes("L", im.size, lo))).tobytes()

_RUNS = re.compile(rb"(..)\1*", re.S)

def rle_bytes(raw):
    """RLE565 over RAW565 bytes: runs of {count 1..255, hi, lo}; runs may cross rows."""
    out = []
    for m in _RUNS.finditer(raw):
        n, px = len(m.group()) // 2, m.group(1)
        while n > 255:
            out.append(b"\xff" + px); n -= 255
        out.append(bytes((n,)) + px)
    return b"".join(out)

QUANT = {"median": Image.Quantize.MEDIANCUT, "octree": Image.Quantize.FASTOCTREE}
quant_method = QUANT["median"]
_BAYER = [[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]]
_bayer_planes = {}

def bayer_plane(size):
    """L image of 128 + ordered-dither offset, tiled 4x4 (cached per size)."""
    if size not in _bayer_planes:
        w, h = size
        p = Image.new("L", size)
        p.putdata([128 + int((_BAYER[y % 4][x % 4] / 16.0 - 0.5) * BAYER_STRENGTH) for y in range(h) for x in range(w)])
        _bayer_planes[size] = p
    return _bayer_planes[size]

def quantize_tile(im, dither):
    """-> (32-byte RGB565 palette, 'P' image with indices < 16). Pillow's quantize() never dithers
    on its own, so: median cut for the palette, then a palette-mapped pass that does."""
    try:
        base = im.quantize(16, method=quant_method)
    except ValueError:
        base = im.quantize(16, method=Image.Quantize.FASTOCTREE)
    if dither == "fs":
        q = im.quantize(palette=base, dither=Image.Dither.FLOYDSTEINBERG)
    elif dither == "bayer":
        off = bayer_plane(im.size)
        d = Image.merge("RGB", [ImageChops.add(c, off, 1.0, -128) for c in im.split()])
        q = d.quantize(palette=base, dither=Image.Dither.NONE)
    else:
        q = base
    pal = (q.getpalette() or [])[:48]; pal += [0] * (48 - len(pal))
    head = b"".join(struct.pack(">H", rgb565(*pal[i:i + 3])) for i in range(0, 48, 3))
    return head, q

def pack_rows(q, first, last):
    """PAL4 rows first..last of a 'P' image: ceil(w/2) bytes per row, high nibble = left pixel."""
    w = q.width
    q = q.crop((0, first, w + (w % 2), last + 1))     # odd width: pad column, index 0 (out of bounds)
    px = q.tobytes()
    even = int.from_bytes(px[0::2].translate(_T_NIB), "big")
    odd = int.from_bytes(px[1::2], "big")
    return (even | odd).to_bytes(len(px) // 2, "big")

def changed_rows(new, old, rowbytes):
    """Rows [first, last] whose bytes differ between two same-size signatures, None if identical.
    One big-int XOR: the highest / lowest set bit give the first / last differing byte."""
    if old is None or len(old) != len(new):
        return 0, len(new) // rowbytes - 1
    x = int.from_bytes(new, "big") ^ int.from_bytes(old, "big")
    if not x:
        return None
    n = len(new)
    first = (n - (x.bit_length() + 7) // 8) // rowbytes
    last = (n - 1 - ((x & -x).bit_length() - 1) // 8) // rowbytes
    return first, last

def make_update(fmt, tile, prev_sig, dither, band_on, cache):
    """One tube's update against what it shows -> (y, h, payload, sig, band_fraction), or None if
    nothing changed. sig = RAW565 rows of the (half-height for PAL4x2) tile; the band is the
    first..last changed row vs prev_sig. cache is per frame: tiles shared by tubes encode once."""
    c = cache.get(id(tile))
    if c is None:
        img = tile.resize((TW, TH2), Image.BOX) if fmt == FMT_PAL4X2 else tile   # filtered 2:1
        c = cache[id(tile)] = {"img": img, "sig": encode_raw(img), "q": None}
    img, sig = c["img"], c["sig"]
    rows_total, rb = img.height, TW * 2
    band = changed_rows(sig, prev_sig, rb)
    if band is None:
        return None
    first, last = band
    if not band_on or (last - first + 1) >= BAND_MAX * rows_total:
        first, last = 0, rows_total - 1
    rows = last - first + 1
    if fmt == FMT_RAW:
        payload = sig[first * rb:(last + 1) * rb]
    elif fmt == FMT_RLE:
        payload = rle_bytes(sig[first * rb:(last + 1) * rb])
    else:
        if c["q"] is None:
            c["q"] = quantize_tile(img, dither)       # palette from the whole tile, band rows only
        head, q = c["q"]
        payload = head + pack_rows(q, first, last)
    if fmt == FMT_PAL4X2:
        y, h = first * 2, min(rows * 2, TH - first * 2)
    else:
        y, h = first, rows
    return y, h, payload, sig, rows / rows_total


# ---- the USB serial port: open, ride out the reset, step the baud up ------------------------------
BOOT_BAUD = 115200

class RawSerial:
    """A pyserial-shaped wrapper backed by a plain POSIX fd. pyserial's macOS open sets a custom
    baud (IOSSIOSPEED) and exclusive access (TIOCEXCL) which a **PTY rejects** ([Errno 25]); this
    opens the fd directly and puts it in raw mode, so bridge can drive a node over a PTY with no
    hardware (--serial-raw). Baud is nominal (a PTY has none). Implements the read/write/in_waiting/
    reset_input_buffer/close/baudrate/timeout surface the Link uses."""
    def __init__(self, port, baud=BOOT_BAUD):
        self.port = port; self.baudrate = baud or BOOT_BAUD; self.timeout = 0.2; self.write_timeout = None
        self.fd = os.open(port, os.O_RDWR | os.O_NOCTTY | getattr(os, "O_NONBLOCK", 0))
        try:                                                 # raw mode so ESP/1 bytes pass through untouched
            import termios
            a = termios.tcgetattr(self.fd)
            a[0] = a[1] = a[3] = 0                            # iflag / oflag / lflag: no xlate, echo or canon
            a[2] = (a[2] & ~termios.PARENB & ~termios.CSTOPB & ~termios.CSIZE) | termios.CS8 | termios.CREAD | termios.CLOCAL
            a[6][termios.VMIN] = 0; a[6][termios.VTIME] = 0
            termios.tcsetattr(self.fd, termios.TCSANOW, a)
        except Exception:
            pass                                             # a PTY may not support termios fully; the raw fd still works
    def read(self, n=1):
        end = time.monotonic() + (self.timeout or 0); buf = b""
        while len(buf) < n:
            r, _, _ = select.select([self.fd], [], [], max(0, end - time.monotonic()))
            if not r: break
            try: chunk = os.read(self.fd, n - len(buf))
            except (BlockingIOError, InterruptedError): continue
            except OSError: break
            if not chunk: break
            buf += chunk
            if not self.timeout: break
        return buf
    def write(self, b):
        mv = memoryview(b); total = 0
        while total < len(mv):
            try: total += os.write(self.fd, mv[total:])
            except BlockingIOError: select.select([], [self.fd], [], 1.0)
            except OSError: break
        return total
    def fileno(self): return self.fd                     # so select.select([ser], …) works (the Link polls readability)
    def flush(self): pass
    reset_output_buffer = flush
    def reset_input_buffer(self):
        try:
            while True:
                r, _, _ = select.select([self.fd], [], [], 0)
                if not r or not os.read(self.fd, 65536): break
        except OSError: pass
    @property
    def in_waiting(self):
        try:
            import fcntl, termios, array
            b = array.array("i", [0]); fcntl.ioctl(self.fd, termios.FIONREAD, b); return b[0]
        except Exception: return 0
    def close(self):
        try: os.close(self.fd)
        except OSError: pass

def open_serial(port, baud, verbose=True, raw=False):
    """The node boots at 115200 and opening the port resets it (the flasher's DTR auto-reset), so:
    open at 115200 with DTR/RTS low, wait for the boot log to finish, then ask the shell for the
    fast rate (`baud N`) and switch our side. Returns a pyserial port ready for ESP/1 frames.

    raw=True (--serial-raw): open the port as a plain fd (no baud/exclusive ioctls) so a macOS PTY
    works, and SKIP the boot-log wait + baud ladder (a PTY has no baud, and a non-ESP32 node has no
    boot shell) — hand back a channel that speaks ESP/1 datagrams straight away."""
    if raw:
        if verbose: print(f"serial: {port} (raw fd — no baud/exclusive ioctls; PTY bench, skipping boot/baud handshake)")
        return RawSerial(port, baud or BOOT_BAUD)
    ser = serial.Serial(); ser.port = port; ser.baudrate = BOOT_BAUD; ser.timeout = 0.2
    ser.dtr = False; ser.rts = False
    try:
        ser.open()
    except serial.SerialException as e:
        raise LinkError(f"{port}: {e} (is the helper's serial panel, monitor.sh or another bridge holding it?)")
    # boot: the shell starts right after the "[http] REST server started" line. The boot log has
    # multi-second gaps (panel self-test), so "quiet for a moment" is NOT "booted" — wait for that
    # line (up to 15 s); with no output at all for 3 s the node simply wasn't reset (already running).
    t0 = time.monotonic(); seen = b""; quiet_since = None
    while time.monotonic() - t0 < 15:
        chunk = ser.read(4096)
        if chunk:
            seen += chunk; quiet_since = None
            if b"[http] REST server started" in seen:
                time.sleep(0.8); break                                        # Esp1/Shell begin just after
        else:
            if quiet_since is None: quiet_since = time.monotonic()
            elif time.monotonic() - quiet_since > 3.0 and not seen: break     # no reset happened: already running
            elif time.monotonic() - quiet_since > 6.0: break                  # garbled/unknown boot: try anyway
    ser.reset_input_buffer()

    def answers(rate):
        """Does the shell answer `help` at this host rate? At >= 1 Mbaud also demand three clean `status`
        JSON replies: 2 M carries CRC-checked pixels on this CH340 but drops bytes in ~1 of 5 long replies."""
        ser.baudrate = rate; time.sleep(0.05); ser.reset_input_buffer(); ser.write(b"help\n")
        t0 = time.monotonic(); r = b""
        while time.monotonic() - t0 < 1.2 and b"binary:" not in r and not (b"commands:" in r and time.monotonic() - t0 > 0.6):
            r += ser.read(512)
        if b"commands:" not in r: return False
        if rate < 1000000: return True
        time.sleep(0.15); ser.reset_input_buffer()
        for _ in range(3):
            ser.write(b"status\n"); t0 = time.monotonic(); r = b""
            while time.monotonic() - t0 < 1.0 and not r.rstrip().endswith(b"}"):
                r += ser.read(1024)
            try: json.loads(r[r.index(b"{"):r.rindex(b"}") + 1].decode())
            except Exception: return False
        return True

    def step_to(rate):
        """Ask the node (at the current rate) to switch to `rate`, follow it, verify with `help`."""
        ser.write(f"baud {rate}\n".encode()); ser.flush()
        t0 = time.monotonic(); reply = b""
        while time.monotonic() - t0 < 2 and b"OK baud" not in reply and b"ERR" not in reply:
            reply += ser.read(256)
        if b"OK baud" not in reply:
            return False
        time.sleep(0.15)
        return answers(rate)

    # where is the shell right now? normally the boot rate; after a one-shot reset it may be elsewhere
    cur = None
    for rate in (BOOT_BAUD, baud, 1500000, 1000000, 460800, 230400):
        if answers(rate):
            cur = rate; break
    if cur is None:
        raise LinkError("no shell reply at any rate — is this the clock's port, and is the firmware >= 1.9?")
    # best working rate: what was asked for, then down the ladder, verifying each — the USB-UART
    # chip / driver decides what it can really do (this one could not do 921600)
    ladder = []
    for r in (baud, 1500000, 1000000, 460800, 230400, BOOT_BAUD):   # measured on the CH340: 1.5 M clean (119 KB/s); 921600 fails; 2 M garbles long replies
        if r <= baud and r not in ladder: ladder.append(r)
    chosen = cur
    for rate in ladder:
        if rate == cur:
            chosen = cur; break
        if step_to(rate):
            chosen = rate; break
        # the node is at `rate` and we can't talk to it there: its rate watchdog brings it back to `cur`
        # by itself within ~8 s of hearing nothing readable — wait for that, then try the next rung
        if verbose: print(f"usb: {rate} baud does not work on this link — waiting for the node to revert")
        ser.baudrate = cur; t0 = time.monotonic(); back = False
        while time.monotonic() - t0 < 14:
            time.sleep(1.0)
            if answers(cur): back = True; break
        if not back:
            raise LinkError(f"lost the shell while trying {rate} baud — firmware < 1.9.4? power-cycle the clock (it boots at {BOOT_BAUD})")
    ser.reset_input_buffer()
    if verbose:
        print(f"usb: {port} @{chosen}" + (f" (asked for {baud}; that rate did not work on this link)" if chosen < baud else ""))
    return ser

# ---- a link: one ESP/1 connection (TCP socket or the serial port) ----------------------------
class LinkError(Exception):
    pass

class Link:
    """Own seq, <= MAX_INFLIGHT unacked frames, own reconnect. After the handshake a TCP socket
    is non-blocking: writes queue in .tx and drain as the kernel takes them, acks are polled, and
    an ack older than ACK_TIMEOUT raises LinkError — so one slow socket never stalls the others."""

    def __init__(self, a, name):
        self.a, self.name, self.quiet = a, name, False
        self.sock = self.ser = None
        self.ready = self.opening = False; self.last_try = 0.0; self.ping_ms = None
        self.seq = 0; self.inflight = collections.deque()   # (seq, t_sent)
        self.buf = b""; self.tx = bytearray()
        self.on_ack = None                        # callback(latency_s, status)
        self.dgram = False                        # serial + firmware >= 2.0: COBS datagrams, NOACK bands, framed acks
        self.epoch = 0                            # the node's content epoch (ESP/1.1), learned from acks
        self.need_key = False                     # an error / status 4 came back: every tube on this link needs a full tile

    kind = property(lambda s: "serial" if s.a.serial else "tcp")

    def describe(self):
        return f"serial {self.a.serial} @{self.a.baud}" if self.a.serial else f"tcp {self.a.host}:{self.a.port}"

    def open(self):
        """Blocking connect + HELLO + PING (runs in ensure_open's thread)."""
        self.close()
        if self.a.serial:
            raw = getattr(self.a, "serial_raw", False)
            if serial is None and not raw:
                raise SystemExit(SERIAL_HINT)
            self.ser = open_serial(self.a.serial, self.a.baud, verbose=not self.quiet, raw=raw)
            self.ser.timeout = self.ser.write_timeout = ACK_TIMEOUT
            self.a.baud = self.ser.baudrate                      # report the rate we actually got
        else:
            self.sock = socket.create_connection((self.a.host, self.a.port), timeout=ACK_TIMEOUT)
            self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        if self.a.token:
            self._write(f"HELLO {self.a.token}\n".encode())
            if self._read_line().strip() != b"OK":
                raise LinkError("auth failed: HELLO was not answered with OK")
        self.dgram = False; self.epoch = 0
        if self.ser:                              # does the node speak ESP/1.1 datagrams? (firmware >= 2.0)
            self.ser.reset_input_buffer(); self.ser.write(datagram(HDR11.pack(0xE5, 0x7B, OP_PING, 0, 0, 0, 0, 0, 0, 0xEE, 0, F_V11, 0)))
            t0 = time.monotonic(); got = b""
            while time.monotonic() - t0 < 1.0 and not self.dgram:
                got += self.ser.read(self.ser.in_waiting or 1)
                for seg in got.split(b"\0"):
                    d = cobs_dec(seg) if len(seg) >= 7 else None
                    if d and len(d) >= 6 and d[0] == 0xA5 and d[1] == 0xEE and crc16(d[:-2]) == struct.unpack(">H", d[-2:])[0]:
                        self.dgram = True; self.epoch = d[3]
            self.ser.timeout = self.ser.write_timeout = ACK_TIMEOUT
            if not self.quiet: print(f"usb: {'ESP/1.1 datagrams (epoch %d)' % self.epoch if self.dgram else 'legacy framing (firmware < 2.0)'}")
        t = time.monotonic()
        self.send(OP_PING, 0); self.drain()       # proves the channel + first latency sample
        self.ping_ms = (time.monotonic() - t) * 1000
        if self.sock:
            self.sock.setblocking(False)
        self.ready = True

    def ensure_open(self, stop=None):
        """Start a background connect (at most one attempt a second); never blocks the caller."""
        if self.ready or self.opening or time.monotonic() - self.last_try < 1.0:
            return
        self.opening, self.last_try = True, time.monotonic()

        def go():
            try:
                self.open()
                if not self.quiet:
                    print(f"{self.name}: connected to {self.describe()} (ping {self.ping_ms:.1f} ms)")
            except (OSError, LinkError) as e:
                self.close()
                if not (stop and stop.is_set()):
                    print(f"{self.name}: connect to {self.describe()} failed: {e} — retrying")
            finally:
                self.opening = False
        threading.Thread(target=go, daemon=True).start()

    def close(self):
        self.ready = False
        for s in (self.sock, self.ser):
            if s:
                try: s.close()
                except Exception: pass
        self.sock = self.ser = None
        self.inflight.clear(); self.buf = b""; self.tx = bytearray()

    def can_send(self):
        return self.ready and len(self.inflight) < MAX_INFLIGHT

    def _write(self, data):
        if self.ser:
            try: self.ser.write(data)
            except OSError as e: raise LinkError(f"write failed: {e}")
        else:
            self.tx += data; self.flush()

    def flush(self):
        """Hand queued bytes to the kernel without blocking (blocking with timeout during open())."""
        while self.tx and self.sock:
            try:
                n = self.sock.send(self.tx)
            except BlockingIOError:
                return
            except (OSError, socket.timeout) as e:
                raise LinkError(f"write failed: {e}")
            del self.tx[:n]

    def _readable(self, timeout):
        if self.ser and self.ser.in_waiting:
            return True
        try:
            return bool(select.select([self.ser or self.sock], [], [], max(0.0, timeout))[0])
        except (OSError, ValueError) as e:
            raise LinkError(f"link lost: {e}")

    def _recv(self):
        try:
            data = self.ser.read(self.ser.in_waiting or 1) if self.ser else self.sock.recv(4096)
        except BlockingIOError:
            return b""
        except (OSError, socket.timeout) as e:
            raise LinkError(f"read failed: {e}")
        if not data:
            raise LinkError("connection closed by node")
        return data

    def _read_line(self):
        deadline = time.monotonic() + ACK_TIMEOUT
        while b"\n" not in self.buf:
            if not self._readable(deadline - time.monotonic()):
                raise LinkError("no reply to HELLO")
            self.buf += self._recv()
        line, self.buf = self.buf.split(b"\n", 1)
        return line

    def _ack(self, seq, status, epoch, now):
        if epoch and self.dgram: self.epoch = epoch                     # status 4 carries the true epoch: adopt it
        if status: self.need_key = True
        if not any(s == seq for s, _ in self.inflight):
            return
        while self.inflight:                                            # the node acks in order; drop anything older
            s, t0 = self.inflight.popleft()
            if s == seq:
                if self.on_ack: self.on_ack(now - t0, status)
                if status and status != 4:
                    print(f"\n{self.name}: node ack status {status} for seq {seq}", file=sys.stderr)
                break

    def _parse_acks(self):
        """Acks are A5 seq status epoch. Datagram mode: they arrive framed (00 COBS 00, CRC16) and shell
        text never contains 0x00. Legacy: raw 4 bytes; anything before an A5 (shell text) is skipped."""
        n = 0
        if self.dgram:
            while b"\0" in self.buf:
                seg, self.buf = self.buf.split(b"\0", 1)
                d = cobs_dec(seg) if len(seg) >= 7 else None
                if d and len(d) >= 6 and d[0] == 0xA5 and crc16(d[:-2]) == struct.unpack(">H", d[-2:])[0]:
                    n += 1; self._ack(d[1], d[2], d[3], time.monotonic())
            if len(self.buf) > 4096: self.buf = self.buf[-64:]            # shell chatter with no delimiter
            return n
        while True:
            i = self.buf.find(b"\xA5")
            if i < 0:
                self.buf = b""; return n
            self.buf = self.buf[i:]
            if len(self.buf) < 4:
                return n
            _, seq, status, _ = self.buf[:4]; self.buf = self.buf[4:]
            now = time.monotonic(); n += 1
            if not any(s == seq for s, _ in self.inflight):
                continue                          # stale ack (e.g. from before a reconnect)
            while self.inflight:                  # the node acks in order; drop anything older
                s, t0 = self.inflight.popleft()
                if s == seq:
                    if self.on_ack: self.on_ack(now - t0, status)
                    if status:
                        print(f"\n{self.name}: node ack status {status} for seq {seq}", file=sys.stderr)
                    break

    def pump(self, wait=False):
        """Push pending tx, consume acks that arrived. wait=True blocks for at least one ack
        (handshake / drain); otherwise never blocks and raises LinkError once the oldest
        unacked frame is older than ACK_TIMEOUT."""
        if self.sock:
            self.flush()
        got, deadline = 0, time.monotonic() + ACK_TIMEOUT
        while True:
            t = (deadline - time.monotonic()) if (wait and not got) else 0.0
            if (wait and not got and t <= 0) or not self._readable(t):
                if wait and not got:
                    raise LinkError(f"no ack within {ACK_TIMEOUT:.0f}s")
                break
            self.buf += self._recv()
            got += self._parse_acks()
        if not wait and self.inflight and time.monotonic() - self.inflight[0][1] > ACK_TIMEOUT:
            raise LinkError(f"no ack within {ACK_TIMEOUT:.0f}s")
        return got

    def send(self, op, tube, payload=b"", x=0, y=0, w=0, h=0, fmt=0, noack=False, flags=0):
        while not noack and len(self.inflight) >= MAX_INFLIGHT:   # callers check can_send(); this is the backstop
            self.pump(wait=True)
        seq, self.seq = self.seq, (self.seq + 1) & 0xFF
        if not noack: self.inflight.append((seq, time.monotonic()))
        if self.dgram:
            f = HDR11.pack(0xE5, 0x7B, op, tube, x, y, w, h, fmt, seq, self.epoch, F_V11 | flags | (F_NOACK if noack else 0), len(payload)) + payload
            self._write(datagram(f))
        else:
            self._write(HDR.pack(0xE5, 0x7B, op, tube, x, y, w, h, fmt, seq, 0, len(payload)) + payload)

    def blit(self, tube, fmt, payload, y=0, h=TH):
        """Datagram mode: the window goes out as self-contained bands, fire-and-forget (NOACK) except the
        last one, whose ack frees the window — no stop-and-wait bubbles, and a CANCEL never waits for more
        than the bands already on the wire."""
        if not self.dgram:
            self.send(OP_BLIT, tube, payload, 0, y, TW, h, fmt); return
        bands = split_bands(fmt, payload, y, h)
        for k, (by, bh, bp) in enumerate(bands):
            self.send(OP_BLIT, tube, bp, 0, by, TW, bh, fmt, noack=(k < len(bands) - 1))

    def cancel(self, clear=False):
        """Tell the node that whatever this bridge still has in flight is stale (source switch / stop)."""
        if not self.ready: return
        try:
            if self.dgram: self.send(OP_CANCEL, 0xFF, flags=F_CLEAR if clear else 0)
            else:
                seq, self.seq = self.seq, (self.seq + 1) & 0xFF; self.inflight.append((seq, time.monotonic()))
                self._write(HDR11.pack(0xE5, 0x7B, OP_CANCEL, 0xFF, 0, 0, 0, 0, 0, seq, 0, F_CLEAR if clear else 0, 0))
        except LinkError:
            pass

    def drain(self):
        while self.inflight:
            self.pump(wait=True)

def make_links(a, order):
    """-> ({tube: Link}, [unique links]). TCP: one socket per populated tube, capped by --conns."""
    if a.serial:
        link = Link(a, "serial"); return {t: link for t in order}, [link]
    n = len(order) if a.conns <= 0 else max(1, min(a.conns, len(order)))
    links = [Link(a, f"c{i}") for i in range(n)]
    return {t: links[k % n] for k, t in enumerate(order)}, links

def wait_links(links, timeout):
    """Sleep until one of these links can make progress (an ack to read or room to write)."""
    links = list({id(l): l for l in links if l.ready}.values())
    rd = [l.sock or l.ser for l in links]
    wr = [l.sock for l in links if l.sock and l.tx]
    if not rd:
        time.sleep(min(timeout, 0.02)); return
    try:
        select.select(rd, wr, [], max(0.0, timeout))
    except (OSError, ValueError):
        pass


# ---- fake node: threaded TCP server, one client state per connection, validates + acks ------
def fake_check(op, tube, x, y, w, h, fmt, payload):
    """-> (status, reason): 0 ok · 1 bad tube/window · 2 bad fmt/op · 3 payload length/content."""
    n = len(payload)
    if op not in (OP_BLIT, OP_FILL, OP_PING, OP_LED, OP_CLEAR):
        return 2, f"unknown op {op}"
    if op in (OP_BLIT, OP_FILL):
        if not (tube <= 5 or (tube == 0xFF and op == OP_FILL)):
            return 1, f"bad tube {tube}"
        if w < 1 or h < 1 or x + w > TW or y + h > TH:
            return 1, f"bad window {x},{y} {w}x{h}"
    if op == OP_CLEAR and not (tube <= 5 or tube == 0xFF):
        return 1, f"bad tube {tube}"
    if op == OP_FILL and n != 2:  return 3, f"FILL wants 2 bytes, got {n}"
    if op == OP_LED and n != 18:  return 3, f"LED wants 18 bytes, got {n}"
    if op in (OP_PING, OP_CLEAR) and n:  return 3, f"{n} payload bytes on op {op}"
    if op == OP_BLIT:
        if fmt == FMT_RAW:
            if n != w * h * 2: return 3, f"RAW565 wants {w*h*2} bytes, got {n}"
        elif fmt == FMT_RLE:
            if n % 3: return 3, f"RLE565 length {n} not a multiple of 3"
            counts = payload[0::3]
            if counts and min(counts) == 0: return 3, "RLE565 run of length 0"
            if sum(counts) != w * h: return 3, f"RLE565 covers {sum(counts)} px, window is {w*h}"
        elif fmt in (FMT_PAL4, FMT_PAL4X2):
            rb, rows = (w + 1) // 2, (h if fmt == FMT_PAL4 else (h + 1) // 2)
            if fmt == FMT_PAL4X2 and y % 2:
                return 1, f"PAL4x2 window y={y} must be even"
            if n != 32 + rb * rows: return 3, f"fmt {fmt} wants 32+{rb}*{rows}={32+rb*rows} bytes, got {n}"
            if w % 2 and any(b & 0x0F for b in payload[32 + rb - 1::rb]):
                return 3, "PAL4 pad nibble (odd width) not zero — rows not byte-aligned?"
        else:
            return 2, f"bad fmt {fmt}"
    return 0, ""

def fake_log(msg):
    """one write per line (threads interleave otherwise); break out of a \\r stats line on a TTY"""
    sys.stderr.write(("\n" if sys.stdout.isatty() else "") + "fake node: " + msg + "\n"); sys.stderr.flush()

def start_fake_node(token=None):
    """Loopback ESP/1 node accepting any number of clients. Returns (host, port)."""
    draw_ms = [float(v) for v in os.environ.get("ESPTUBE_FAKE_MS", "0").split(",")]
    kbps = float(os.environ.get("ESPTUBE_FAKE_KBPS", "0"))
    srv = socket.socket(); srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", 0)); srv.listen(16); count = [0]
    cpu = threading.Lock()                        # the node has one CPU: draw time is serialized

    def client(conn, cid, draw_s):
        f = conn.makefile("rb"); frames = nbytes = bad = partial = 0
        try:
            if token:
                if f.readline().strip() != f"HELLO {token}".encode():
                    fake_log(f"{cid} bad HELLO, closing"); return
                conn.sendall(b"OK\n")
            while True:
                hdr = f.read(HDR.size)
                if len(hdr) < HDR.size: break
                m0, m1, op, tube, x, y, w, h, fmt, seq, _, ln = HDR.unpack(hdr)
                if (m0, m1) != (0xE5, 0x7B):
                    fake_log(f"{cid} bad magic {m0:02X}{m1:02X}, closing"); return
                payload = f.read(ln) if ln else b""
                if len(payload) < ln:
                    fake_log(f"{cid} short read ({len(payload)}/{ln}), closing"); return
                if kbps:
                    time.sleep((HDR.size + ln) / (kbps * 1024))
                status, why = fake_check(op, tube, x, y, w, h, fmt, payload)
                if status:
                    bad += 1; fake_log(f"{cid} seq {seq} op {op} tube {tube} -> status {status}: {why}")
                elif op == OP_BLIT and draw_s:
                    with cpu:
                        time.sleep(draw_s * h / TH)   # draw time scales with the rows drawn
                if op == OP_BLIT and h < TH: partial += 1
                conn.sendall(bytes((0xA5, seq, status, 0)))
                frames += 1; nbytes += HDR.size + ln
        except OSError:
            pass
        finally:
            conn.close()
            fake_log(f"{cid} done — {frames} frames ({partial} partial), {nbytes/1024:.0f} KB, {bad} rejected")

    def accept():
        while True:
            c, _ = srv.accept(); count[0] += 1
            ms = draw_ms[(count[0] - 1) % len(draw_ms)]
            threading.Thread(target=client, args=(c, f"client#{count[0]}", ms / 1000.0), daemon=True).start()
    threading.Thread(target=accept, daemon=True).start()
    return srv.getsockname()


# ---- stats -----------------------------------------------------------------------------------
class Stats:
    def __init__(self):
        self.lock = threading.Lock(); self.links = []
        self.t0 = self.t_win = time.monotonic()
        self.frames = self.blits = self.bytes = self.dropped = 0; self.band_sum = 0.0
        self.win = {"frames": 0, "blits": 0, "bytes": 0, "tube": {}}
        self.cur = {"acks": 0, "sends": 0, "drops": 0, "ticks": 0}; self.hist = collections.deque(maxlen=2)
        self.fps = self.ups = self.kbps = 0.0; self.tube_rate = {}; self.ack_ms = None
        self.auto, self.target = False, None

    def frame(self, sent, bands):                 # one tick over all tubes; sent = {tube: payload bytes}
        with self.lock:
            self.frames += 1; self.win["frames"] += 1; self.cur["ticks"] += 1
            for t, n in sent.items():
                self.blits += 1; self.bytes += n; self.win["blits"] += 1; self.win["bytes"] += n
                self.win["tube"][t] = self.win["tube"].get(t, 0) + n; self.cur["sends"] += 1
            self.band_sum += sum(bands)
            self._roll()

    def drop(self, n=1):                          # tube frames skipped: their socket was still busy
        with self.lock: self.dropped += n; self.cur["drops"] += n; self._roll()

    def ack(self, dt, status):
        with self.lock:
            self.cur["acks"] += 1
            self.ack_ms = dt * 1000 if self.ack_ms is None else self.ack_ms * 0.8 + dt * 200

    def conns(self):
        return sum(1 for l in self.links if l.ready), len(self.links)

    def window(self):
        """the last ~2 s of completed one-second buckets"""
        with self.lock:
            w = {k: sum(b[k] for b in self.hist) for k in ("acks", "sends", "drops", "ticks")}
            w["seconds"] = sum(b["dt"] for b in self.hist); return w

    def _roll(self):
        dt = time.monotonic() - self.t_win
        if dt >= 1.0:
            w = self.win
            self.fps, self.ups, self.kbps = w["frames"] / dt, w["blits"] / dt, w["bytes"] / dt / 1024
            self.tube_rate = {t: n / dt / 1024 for t, n in w["tube"].items()}
            self.win = {"frames": 0, "blits": 0, "bytes": 0, "tube": {}}
            self.cur["dt"] = dt; self.hist.append(self.cur); self.cur = {"acks": 0, "sends": 0, "drops": 0, "ticks": 0}
            self.t_win += dt

    def band_pct(self):
        return 100.0 * self.band_sum / self.blits if self.blits else 0.0

    def line(self):
        with self.lock:
            tubes = " ".join(f"{t}:{k:.1f}k" for t, k in sorted(self.tube_rate.items(), reverse=True))
            ack = f"{self.ack_ms:.0f}ms" if self.ack_ms is not None else "-"
            r, n = self.conns()
            tgt = f" ({'auto' if self.auto else 'fixed'} {self.target:.1f})" if self.target else ""
            return (f"fps {self.fps:4.1f}{tgt}  upd/s {self.ups:5.1f} (total {self.blits})  {self.kbps:6.1f} KB/s  band {self.band_pct():3.0f}%  "
                    f"tubes[{tubes}]  drop {self.dropped}  ack {ack}  conn {r}/{n}  frames {self.frames}")

    def summary(self):
        el = max(1e-6, time.monotonic() - self.t0)
        with self.lock:
            b = max(1, self.blits)
            tgt = f"{'auto' if self.auto else 'fixed'} target {self.target:.1f}" if self.target else "max rate"
            return (f"summary: {self.frames / el:.1f} fps effective over {el:.0f} s ({tgt}) · {self.blits / el:.1f} updates/s · "
                    f"{self.bytes / b / 1024:.2f} KB/update · {self.bytes / max(1, self.frames) / 1024:.2f} KB/frame · "
                    f"band {self.band_pct():.0f}% · drop {self.dropped} · ack {self.ack_ms or 0:.0f} ms")


# ---- sources: next(stop) -> PIL RGB frame | [one tile per tube] | None at end --------------
class Source:
    def __init__(self, name, nxt, close=None, restart=None):
        self.name, self.next, self.close, self.restart = name, nxt, close or (lambda: None), restart

def need(tool):
    if shutil.which(tool) is None:
        print(FFMPEG_HINT if tool == "ffmpeg" else YTDLP_HINT); sys.exit(2)

def fit(im, w, h):
    """cover-fit (scale to cover, crop centre)"""
    return im if im.size == (w, h) else ImageOps.fit(im, (w, h), method=Image.BILINEAR)

def natural_key(s):
    return [int(t) if t.isdigit() else t.lower() for t in re.split(r"(\d+)", s)]

_HEX6 = re.compile(r"#[0-9a-f]{6}$")

def _hex_rgb(s):
    if not _HEX6.match(s):
        raise ValueError(f"{BG_GRAMMAR}  (got {s!r})")
    return tuple(int(s[i:i + 2], 16) for i in (1, 3, 5))

def parse_bg(spec):
    """--bg spec -> ("solid", rgb) | ("gradient", top_rgb, bottom_rgb) | ("glow", rgb); ValueError if malformed."""
    s = str(spec if spec is not None else BG_DEFAULT).strip().lower()
    if s in ("", BG_DEFAULT):
        return ("solid", (0, 0, 0))
    if s.startswith("glow:"):
        return ("glow", _hex_rgb(s[5:]))
    if s.startswith("#") and s.count("-") == 1:
        top, bottom = s.split("-")
        return ("gradient", _hex_rgb(top), _hex_rgb(bottom))
    return ("solid", _hex_rgb(s))

_BG_CACHE = {}

def bg_image(spec, w, h):
    """The opaque w x h RGBA image a transparent frame is composited over (built once per spec/size)."""
    key = (str(spec if spec is not None else BG_DEFAULT).strip().lower(), w, h)
    im = _BG_CACHE.get(key)
    if im is None:
        kind, *args = parse_bg(spec)
        if kind == "solid":
            im = Image.new("RGBA", (w, h), args[0] + (255,))
        elif kind == "gradient":
            (r0, g0, b0), (r1, g1, b1) = args
            im = Image.new("RGBA", (w, h)); d = ImageDraw.Draw(im)
            for y in range(h):
                t = y / max(1, h - 1)
                d.line([(0, y), (w, y)], fill=(round(r0 + (r1 - r0) * t), round(g0 + (g1 - g0) * t),
                                              round(b0 + (b1 - b0) * t), 255))
        else:                                        # glow: the colour at the centre, fading to black at the edges
            mask = ImageOps.invert(Image.radial_gradient("L")).resize((w, h), Image.BILINEAR)
            mask = mask.point(lambda v: int(255 * (v / 255.0) ** 1.6))   # concentrate it behind the figure
            im = Image.new("RGBA", (w, h), (0, 0, 0, 255))
            im.paste(Image.new("RGBA", (w, h), args[0] + (255,)), mask=mask)
        _BG_CACHE[key] = im
    return im

def _has_alpha(im):
    return im.mode in ("RGBA", "LA", "PA") or (im.mode == "P" and "transparency" in im.info)

def load_sequence(d, w, h, limit=None, bg=None):
    """The numbered frames of a folder, cover-fit to w x h, as RGB.  A frame with an alpha channel (the
    rendered characters are transparent RGBA) is composited over `bg` -- a --bg spec, default black --
    so downstream (tiles, band diffs, quantisation) always sees opaque RGB."""
    names = sorted((f for f in os.listdir(d) if f.lower().endswith((".png", ".jpg", ".jpeg"))), key=natural_key)
    if not names:
        raise SystemExit(f"no PNG/JPG frames in {d}")
    out = []
    for f in names[:limit]:
        im = Image.open(os.path.join(d, f))
        if _has_alpha(im):
            im = Image.alpha_composite(bg_image(bg, w, h), fit(im.convert("RGBA"), w, h)).convert("RGB")
        else:
            im = fit(im.convert("RGB"), w, h)
        out.append(im)
    return out

def sequence_source(name, seqs, rate, rotate_every, n):
    """seqs: list of frame lists played at `rate` fps. One sequence -> shared frame; several ->
    one per tube, the assignment shifting by one every rotate_every seconds."""
    t0 = time.monotonic(); rate = rate or 12.0

    def nxt(stop):
        el = time.monotonic() - t0; i = int(el * rate)
        if len(seqs) == 1:
            return seqs[0][i % len(seqs[0])]
        shift = int(el / rotate_every) % len(seqs) if rotate_every > 0 else 0
        out = []
        for k in range(n):
            s = seqs[(k + shift) % len(seqs)]; out.append(s[i % len(s)])
        return out
    return Source(name, nxt)

def test_source(w, h):
    """Scrolling colour bars + bouncing ball + frame counter, all in Python (no ffmpeg)."""
    bars = [(255, 255, 255), (255, 255, 0), (0, 255, 255), (0, 255, 0), (255, 0, 255), (255, 0, 0), (0, 0, 255), (32, 32, 32)]
    bw = max(8, math.ceil(w / 8)); r = min(20, w // 4, h // 4); i = [0]

    def tri(v, m):                                # triangle wave 0..m
        v %= 2 * m; return v if v < m else 2 * m - v

    def nxt(stop):
        k = i[0]; i[0] += 1
        im = Image.new("RGB", (w, h)); d = ImageDraw.Draw(im)
        off = (k * 3) % (8 * bw)
        for j in range(-8, 9):
            x0 = j * bw + off
            if x0 < w and x0 + bw > 0:
                d.rectangle([x0, 0, x0 + bw - 1, h - 1], fill=bars[j % 8])
        d.rectangle([0, h // 2 - 14, w, h // 2 + 14], fill=(0, 0, 0))
        d.text((4, h // 2 - 6), f"frame {k}", fill=(255, 200, 40))
        bx, by = r + tri(k * 7, w - 2 * r), r + tri(k * 5, h - 2 * r)
        d.ellipse([bx - r, by - r, bx + r, by + r], fill=(255, 128, 0), outline=(0, 0, 0))
        return im
    return Source("test", nxt)

def ffmpeg_cmd(inp, w, h, fps, crop=None):
    vf = (f"crop={crop[2]}:{crop[3]}:{crop[0]}:{crop[1]}," if crop else "") + \
         f"scale={w}:{h}:force_original_aspect_ratio=increase,crop={w}:{h}"
    out = ["-vf", vf, "-s", f"{w}x{h}", "-f", "rawvideo", "-pix_fmt", "rgb24", "-an", "-sn"]
    if fps: out += ["-r", str(fps)]
    return ["ffmpeg", "-hide_banner", "-loglevel", "error", "-nostdin"] + inp + out + ["pipe:1"]

def ffmpeg_source(name, cmd, w, h, stdin=None):
    """Run ffmpeg; a reader thread keeps only the NEWEST rgb24 frame so a slow node drops, not lags."""
    proc = subprocess.Popen(cmd, stdin=stdin, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    errs = collections.deque(maxlen=12)
    threading.Thread(target=lambda: errs.extend(l.decode(errors="replace").rstrip() for l in proc.stderr), daemon=True).start()
    size, cv, slot = w * h * 3, threading.Condition(), {"data": None, "n": 0, "eof": False}

    def reader():
        while True:
            data = proc.stdout.read(size)
            if len(data) < size: break
            with cv:
                slot["data"], slot["n"] = data, slot["n"] + 1; cv.notify_all()
        with cv:
            slot["eof"] = True; cv.notify_all()
    threading.Thread(target=reader, daemon=True).start()
    seen = [0]

    def nxt(stop):
        with cv:
            while slot["n"] == seen[0] and not slot["eof"] and not stop.is_set():
                cv.wait(0.25)
            if slot["n"] == seen[0]:
                return None
            seen[0] = slot["n"]; data = slot["data"]
        return Image.frombytes("RGB", (w, h), data)

    def close():
        if proc.poll() is None:
            proc.kill()
        proc.wait()
    src = Source(name, nxt, close); src.proc, src.errs = proc, errs
    return src

def avfoundation_source(name, device, in_rate, w, h, fps, stop, crop=None):
    """A camera or a display via avfoundation; tries the pixel formats macOS devices usually
    offer until one yields a frame."""
    for pf in ("uyvy422", "nv12", "yuyv422", None):
        inp = ["-f", "avfoundation", "-framerate", str(in_rate)] + (["-pixel_format", pf] if pf else [])
        if name.startswith("screen"): inp += ["-capture_cursor", "1"]
        inp += ["-i", device]
        src = ffmpeg_source(name, ffmpeg_cmd(inp, w, h, fps, crop), w, h)
        first = src.next(stop)
        if first is not None:
            stash, nxt = [first], src.next
            src.next = lambda s: stash.pop() if stash else nxt(s)
            return src
        src.close()
        print(f"{name} pixel_format={pf or 'default'} gave no frames: {' | '.join(src.errs) or 'ffmpeg exited'}")
    raise SystemExit(f"{name}: ffmpeg could not open the device")

YT_FMT = "best[height<=480][vcodec!=none]/best"
YT_CLIENTS = ("mweb", "default")

def yt_client(url):
    """Which YouTube player client actually streams today. Google gates the media URLs per client (PO
    tokens); as of 2026-09 the default web clients 403 while `mweb` streams. Probe each for 64 KB."""
    for c in YT_CLIENTS:
        try:
            r = subprocess.run(["yt-dlp", "-q", "--no-warnings", "--extractor-args", f"youtube:player_client={c}",
                                "-f", YT_FMT, "--downloader-args", "ffmpeg:-t 1", "-o", "-", url],
                               capture_output=True, timeout=45)
        except subprocess.TimeoutExpired as e:
            r = e; r.stdout = e.stdout or b""; r.stderr = e.stderr or b""
        if len(r.stdout) >= 65536:
            print(f"url: yt-dlp player_client={c}"); return c
        err = (r.stderr or b"").decode(errors="replace").strip().splitlines()
        print(f"url: player_client={c} gave no data ({err[-1][:120] if err else 'no output'})")
    raise SystemExit("yt-dlp could not stream this URL with any client — try:  brew upgrade yt-dlp\n"
                     "  (or share the YouTube tab from the helper's Live panel, which needs no yt-dlp)")

def page_source(url, w, h, every, chrome):
    """Headless Chrome screenshot of a web page every `every` seconds, at exactly w x h."""
    if not chrome or not os.path.exists(chrome):
        print(f"page: {CHROME_HINT}"); sys.exit(2)
    tmp = tempfile.mkdtemp(prefix="esptube-page-"); shot = os.path.join(tmp, "page.png")
    cmd = [chrome, "--headless=new", "--disable-gpu", "--hide-scrollbars", "--force-device-scale-factor=1",
           f"--window-size={w},{h}", f"--screenshot={shot}", "--no-first-run", "--no-default-browser-check",
           "--disable-background-networking", "--disable-component-update",
           f"--user-data-dir={os.path.join(tmp, 'profile')}", url]
    cv, slot, done = threading.Condition(), {"im": None, "err": None}, threading.Event()

    def snap():
        """Run Chrome until the PNG lands, then kill it — headless Chrome writes the screenshot in
        a few seconds but often lingers (updater, GPU teardown) long after."""
        if os.path.exists(shot):
            os.remove(shot)
        proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        errs = collections.deque(maxlen=6)
        threading.Thread(target=lambda: errs.extend(l.decode(errors="replace").rstrip() for l in proc.stderr), daemon=True).start()
        im, deadline = None, time.monotonic() + 60       # Rosetta Chrome's first launch can take 30 s+
        while im is None and time.monotonic() < deadline and not done.is_set():
            if os.path.exists(shot):
                try:
                    im = Image.open(shot); im.load()
                except OSError:
                    im = None                     # still being written
            elif proc.poll() is not None:
                break                             # exited without a screenshot
            if im is None:
                time.sleep(0.1)
        if proc.poll() is None:
            proc.kill(); proc.wait()
        if im is None:
            raise OSError(f"Chrome wrote no screenshot (exit {proc.returncode}): " + (" | ".join(errs) or "no output"))
        return im.convert("RGB")

    def worker():
        while not done.is_set():
            try:
                im = snap()
                with cv:
                    slot["im"], slot["err"] = fit(im, w, h), None; cv.notify_all()
            except OSError as e:
                with cv:
                    slot["err"] = f"page: {e}"; cv.notify_all()
            done.wait(every)
    threading.Thread(target=worker, daemon=True).start()
    told = [None]

    def nxt(stop):
        with cv:
            while slot["im"] is None and slot["err"] is None and not stop.is_set():
                cv.wait(0.25)
            if slot["err"] and slot["err"] != told[0]:
                print(slot["err"]); told[0] = slot["err"]
            return slot["im"]                     # unchanged screenshot -> band diff sends nothing

    def close():
        done.set(); shutil.rmtree(tmp, ignore_errors=True)
    return Source(f"page:{url}", nxt, close)

def set_manifest(set_id):
    """samples/<set>/manifest.json as a dict, or None. Character entries carry name (== id), label, dir."""
    p = os.path.join(SAMPLES, set_id, "manifest.json")
    if not re.fullmatch(r"[A-Za-z0-9_-]+", set_id or "") or not os.path.exists(p):
        return None
    with open(p) as f:
        man = json.load(f)
    for c in man.get("characters", []):
        c.setdefault("name", c.get("id")); c.setdefault("label", str(c["name"]).capitalize())
    return man

def list_sets():
    """Every rendered character set: [{"id","title","characters":[{"id","label"}]}] (for /bridge/sources)."""
    out = []
    for p in sorted(glob.glob(os.path.join(SAMPLES, "*", "manifest.json"))):
        sid = os.path.basename(os.path.dirname(p)); man = set_manifest(sid)
        if man and man.get("characters"):
            out.append({"id": man.get("set", sid), "title": man.get("title", sid),
                        "characters": [{"id": c["name"], "label": c["label"]} for c in man["characters"]]})
    return out

def chars_sequences(set_id, names=None, bg=None):
    """Frame sequences for a character set (optionally just `names`), composited over `bg` -> (seqs, native fps)."""
    man = set_manifest(set_id)
    if man is None:
        have = ", ".join(s["id"] for s in list_sets()) or "none rendered"
        raise SystemExit(f"no character set {set_id!r} under samples/ (have: {have}) — render it first:\n"
                         f"  python3 tools/render_characters.py --set {set_id}")
    chars = man.get("characters", [])
    if names:
        by_id = {c["name"]: c for c in chars}
        missing = [n for n in names if n not in by_id]
        if missing:
            raise SystemExit(f"chars:{set_id} has no {', '.join(missing)} (has {', '.join(by_id)})")
        chars = [by_id[n] for n in names]
    print(f"chars:{set_id}: loading {len(chars)} characters ({man.get('fps', 12)} fps turntables) over bg {bg or BG_DEFAULT} …")
    seqs = [load_sequence(os.path.join(ROOT, c["dir"]), TW, TH, c.get("frames"), bg) for c in chars]
    if not seqs:
        raise SystemExit(f"chars:{set_id} manifest has no characters")
    return seqs, float(man.get("fps", 12))

def dwarves_sequences(n=None):
    return chars_sequences("dwarves")

def parse_chars(v):
    return [s.strip() for s in str(v or "").split(",") if s.strip()]

def open_source(a, n, fw, fh, stop):
    spec = a.source
    kind, _, arg = spec.partition(":")
    out_fps = a.fps or a.max_fps                  # ffmpeg output rate (auto: up to the ceiling)
    bg = getattr(a, "bg", BG_DEFAULT) or BG_DEFAULT   # behind transparent frames (chars:, RGBA dir:)
    try:
        parse_bg(bg)
    except ValueError as e:
        raise SystemExit(str(e))
    if spec == "test":
        return test_source(fw, fh)
    if spec == "dwarves":
        spec, kind, arg = "chars:dwarves", "chars", "dwarves"    # the original name still works
    if kind == "chars":
        if a.layout == "span": print(f"note: {spec} is one character per tube; span layout ignored")
        seqs, native = chars_sequences(arg, parse_chars(getattr(a, "chars", "")), bg)
        return sequence_source(spec, seqs, a.fps or native, a.rotate_every, n)
    if kind == "dir":
        d = os.path.expanduser(arg)
        subs = sorted((os.path.join(d, s) for s in os.listdir(d) if os.path.isdir(os.path.join(d, s))), key=natural_key)
        subs = [s for s in subs if any(f.lower().endswith((".png", ".jpg", ".jpeg")) for f in os.listdir(s))]
        if subs:
            print(f"dir: {len(subs)} sequences, one per tube" + (" (span layout ignored)" if a.layout == "span" else ""))
            return sequence_source(spec, [load_sequence(s, TW, TH, bg=bg) for s in subs], a.fps, a.rotate_every, n)
        return sequence_source(spec, [load_sequence(d, fw, fh, bg=bg)], a.fps, a.rotate_every, n)
    if kind == "page":
        return page_source(arg, fw, fh, a.page_every, a.chrome)
    if kind in ("webcam", "screen", "file", "url"):
        need("ffmpeg")
    if kind == "webcam":
        return avfoundation_source(spec, str(int(arg or 0)), 30, fw, fh, out_fps, stop)
    if kind == "screen":
        crop = tuple(int(v) for v in a.crop.split(",")) if a.crop else None
        if crop and len(crop) != 4: raise SystemExit("--crop wants x,y,w,h")
        return avfoundation_source(spec, f"Capture screen {int(arg or 0)}", min(10, out_fps), fw, fh, out_fps, stop, crop)
    if kind == "file":
        path = os.path.expanduser(arg)
        if not os.path.exists(path): raise SystemExit(f"no such file: {path}")
        inp = (["-stream_loop", "-1"] if a.loop else []) + ["-re", "-i", path]
        return ffmpeg_source(spec, ffmpeg_cmd(inp, fw, fh, out_fps), fw, fh)
    if kind == "url":
        # yt-dlp downloads (it carries the client headers/tokens Google wants — a bare `yt-dlp -g` URL
        # handed to ffmpeg gets 403) and pipes a progressive <=480p stream into ffmpeg's stdin.
        need("yt-dlp")
        client = yt_client(arg)
        def make():
            yt = subprocess.Popen(["yt-dlp", "-q", "--no-warnings", "--extractor-args", f"youtube:player_client={client}",
                                   "-f", YT_FMT, "-o", "-", arg], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            src = ffmpeg_source(spec, ffmpeg_cmd(["-re", "-i", "pipe:0"], fw, fh, out_fps), fw, fh, stdin=yt.stdout)
            yt.stdout.close()                    # ffmpeg owns the read end now
            threading.Thread(target=lambda: src.errs.extend("yt-dlp: " + l.decode(errors="replace").rstrip()
                                                              for l in yt.stderr), daemon=True).start()
            old_close = src.close
            src.close = lambda: (yt.kill(), old_close())
            return src
        src = make(); src.restart = make if a.loop else None
        return src
    raise SystemExit(f"unknown source {spec!r} — see --help")

def tiles_for(frame, layout, n, gap):
    """-> n tiles (135x240 RGB) in physical left->right order"""
    if isinstance(frame, list):
        return [fit(f, TW, TH) for f in frame]
    if layout == "span":
        frame = fit(frame, n * TW + (n - 1) * gap, TH)
        return [frame.crop((k * (TW + gap), 0, k * (TW + gap) + TW, TH)) for k in range(n)]
    return [fit(frame, TW, TH)] * n


# ---- the stream loop -------------------------------------------------------------------------
PANEL_MM = (14.864, 24.912)             # 1.14" 135x240 IPS active area (datasheet) — same constant as the helper's Layout module

def geometry_gap(spec, host):
    """--geometry FILE|URL|clock -> the seam gap in panel pixels from an esptube.layout/1 document (the
    helper's 🧊 Stage writes it; `clock` fetches http://<host>/layout.json). Uniform rows only, for now."""
    try:
        if spec == "clock": spec = f"http://{host}/layout.json"
        raw = urllib.request.urlopen(spec, timeout=5).read() if spec.startswith("http") else open(spec, "rb").read()
        L = json.loads(raw)
        if L.get("row"): pitch = float(L["row"]["pitch"])
        else:
            xs = sorted(d["pos"][0] for d in L["displays"]); pitch = (xs[-1] - xs[0]) / max(1, len(xs) - 1)
        gap = max(0, round(pitch * TW / PANEL_MM[0] - TW))
        print(f"geometry: pitch {pitch:g} mm -> seam gap {gap} px" + ("" if L.get("calibrated") else "  (layout not calibrated yet)"))
        return gap
    except Exception as e:
        print(f"geometry: {spec}: {e} — keeping --gap"); return None

def node_reachable(host, port, timeout=2.0):
    try:
        with socket.create_connection((host, port), timeout=timeout):
            return True
    except OSError:
        return False

def pick_transport(a):
    """--transport auto (default): TCP to --host if it answers, otherwise the USB serial port if one is
    plugged in. `tcp` / `serial` force one. Sets a.serial (the port) when USB is chosen."""
    mode = getattr(a, "transport", "auto") or "auto"
    if a.fake:
        a.serial = None; return "tcp"
    if mode == "serial" or (a.serial and mode == "auto"):
        a.serial = a.serial or usb_port(a)
        if not a.serial:
            raise SystemExit("no USB serial port (/dev/cu.usbserial-*) — plug the clock into the powered hub")
        return "serial"
    if mode == "tcp":
        a.serial = None; return "tcp"
    port = usb_port(a)
    if port:                                     # auto = the cable first: 1.5 Mbaud datagrams are as fast as good WiFi and never at -76 dBm
        holder = subprocess.run(["lsof", "-t", port], capture_output=True, text=True).stdout.split()
        if not holder:
            print(f"USB cable {port} found — using it (up to {a.baud} baud; --transport tcp forces WiFi)")
            a.serial = port; return "serial"
        print(f"USB cable {port} is held by pid {','.join(holder)} (the helper's serial panel?) — using WiFi")
    if node_reachable(a.host, a.port):
        a.serial = None; return "tcp"
    print(f"node not reachable at {a.host}:{a.port} and no USB cable — will keep trying the network")
    return "tcp"

def tube_order(a):
    """Populated tubes, left -> right, as native indices."""
    if a.tubes:
        tubes = [int(t) for t in a.tubes.split(",") if t.strip()]
        if not tubes or any(t not in PHYSICAL for t in tubes):
            raise SystemExit(f"--tubes must list indices 0..5 left->right, e.g. 5,4,3,2,1 (got {a.tubes!r})")
        return tubes
    mask = DEFAULT_MASK
    if a.serial:
        return [t for t in PHYSICAL if (mask >> t) & 1]      # over USB: the default layout (use --tubes to override)
    if not a.fake:
        try:
            with urllib.request.urlopen(f"http://{a.host}/status", timeout=3) as r:
                mask = int(json.load(r).get("populated_mask", DEFAULT_MASK))
        except Exception as e:
            print(f"note: GET http://{a.host}/status failed ({e}); assuming populated mask 0x{DEFAULT_MASK:02X}")
    return [t for t in PHYSICAL if (mask >> t) & 1]

def run_stream(a, stats, stop, quiet=False):
    if getattr(a, "geometry", None):
        g = geometry_gap(a.geometry, a.host)
        if g is not None: a.gap = g
    pick_transport(a)
    order = tube_order(a); n = len(order)
    if not n:
        raise SystemExit("no populated tubes")
    fmt = FMTS[a.fmt]
    fw, fh = (n * TW + (n - 1) * a.gap, TH) if a.layout == "span" else (TW, TH)
    src = open_source(a, n, fw, fh, stop)
    by_tube, links = make_links(a, order)
    for l in links: l.on_ack, l.quiet = stats.ack, quiet
    stats.links = links
    auto = a.fps is None
    target = min(6.0, a.max_fps) if auto else a.fps          # auto starts low and ramps
    stats.auto, stats.target = auto, (target or None)
    print(f"stream {src.name} -> tubes {order} (left->right)  layout={a.layout} fmt={a.fmt} dither={a.dither} "
          f"band={'on' if a.band else 'off'} fps={'auto (max %g)' % a.max_fps if auto else (a.fps or 'max')} gap={a.gap}"
          + (f" bg={a.bg}" if getattr(a, "bg", BG_DEFAULT) not in (None, BG_DEFAULT) else ""))
    print(f"  {len(links)} {'serial link' if a.serial else 'TCP connection(s)'} to {links[0].describe()}: "
          + ", ".join(f"tube {t}->{by_tube[t].name}" for t in order))
    tty = sys.stdout.isatty() and not quiet
    period = 1.0 / target if target > 0 else 0.0
    slot = {t: k for k, t in enumerate(order)}    # tube -> tile index (left->right)
    prev, t_next, t_report, t_ctl, tick = {}, time.monotonic(), 0.0, time.monotonic(), 0
    for l in links:
        l.ensure_open(stop)                       # connect while a slow source (page:, webcam:) starts
    try:
        while not stop.is_set():
            frame = src.next(stop)
            if frame is None:
                if src.restart:
                    src.close(); src = src.restart(); continue
                errs = [e for e in getattr(src, "errs", []) if e]
                print("\nsource ended" + (" — last ffmpeg output:\n  " + "\n  ".join(errs[-6:]) if errs else "")); break
            if period:                            # pace all tubes together on a monotonic clock
                now = time.monotonic()
                if now < t_next:
                    time.sleep(t_next - now)
                elif now - t_next > 2 * period:   # fell behind: resync instead of bursting
                    t_next = now
                t_next += period
            # One tick: pass 1 sends to every socket that has room right now; then we wait only on
            # the sockets that were busy, until the next tick is due (max-rate: until the ack
            # timeout), and drop whatever is still blocked. A slow socket never delays the others.
            deadline = t_next if period else time.monotonic() + ACK_TIMEOUT
            tiles, cache, sent, bands = None, {}, {}, []
            pending = order[tick % n:] + order[:tick % n]; tick += 1   # rotate: fair on shared sockets
            while pending:
                busy = []
                for tube in pending:
                    link = by_tube[tube]
                    if not link.ready:
                        link.ensure_open(stop); continue
                    try:
                        link.pump()
                        if link.need_key:         # status 4 (superseded) or an error: what we think is on the glass is not
                            link.need_key = False
                            for t2, l2 in by_tube.items():
                                if l2 is link: prev.pop(t2, None)
                        if not link.can_send():   # 2 unacked on this socket
                            busy.append(tube); continue
                        if tiles is None:
                            tiles = tiles_for(frame, a.layout, n, a.gap)
                        upd = make_update(fmt, tiles[slot[tube]], prev.get(tube), a.dither, a.band, cache)
                        if upd is None:
                            continue              # this tube already shows exactly this
                        y, h, payload, sig, frac = upd
                        link.blit(tube, fmt, payload, y, h)
                        prev[tube] = sig; sent[tube] = len(payload); bands.append(frac)
                    except LinkError as e:
                        print(f"\n{link.name}: {e} — reconnecting")
                        link.close()
                        for t2, l2 in by_tube.items():
                            if l2 is link: prev.pop(t2, None)   # it will need a full tile again
                remaining = deadline - time.monotonic()
                if not busy or remaining <= 0:
                    if busy and period:
                        stats.drop(len(busy))     # still blocked when the next tick is due
                    break
                wait_links([by_tube[t] for t in busy], remaining)
                pending = busy
            stats.frame(sent, bands)
            if not period and not sent:
                time.sleep(0.02)                  # max-rate mode with nothing connected yet
            now = time.monotonic()
            if auto and now - t_ctl >= 1.0:       # adaptive pacing, once a second over a ~2 s window
                t_ctl = now; w = stats.window()
                if w["seconds"] >= 1.0:
                    if w["drops"]:
                        demand = (w["sends"] + w["drops"]) / max(1, w["ticks"])   # updates wanted per tick
                        served = w["acks"] / w["seconds"]                          # updates the node drew /s
                        target = max(2.0, min(a.max_fps, 0.9 * served / max(demand, 1e-6)))
                    else:
                        target = min(a.max_fps, target * 1.15)
                    period = 1.0 / target; stats.target = target
            if tty and now - t_report >= 0.5:
                sys.stdout.write("\r" + stats.line().ljust(130)); sys.stdout.flush(); t_report = now
            elif not tty and not quiet and now - t_report >= 2.0:
                print(stats.line()); t_report = now
    finally:
        src.close()
        for l in links:
            try:
                if l.ready: l.cancel(); l.drain()      # anything of ours still arriving is stale now
            except LinkError:
                pass
            l.close()
        if not quiet:
            print(("\r" if tty else "") + stats.line())
            print(stats.summary())
        stats.links = []


# ---- serve: localhost control API ------------------------------------------------------------
SERVE = {"lock": threading.Lock(), "base": None, "thread": None, "stop": None, "stats": Stats(), "opts": None}

def usb_port(base):
    ports = sorted(glob.glob("/dev/cu.usbserial-*"))
    return base.serial or (ports[0] if ports else None)

def list_webcams():
    if shutil.which("ffmpeg") is None:
        return []
    r = subprocess.run(["ffmpeg", "-hide_banner", "-f", "avfoundation", "-list_devices", "true", "-i", ""],
                       capture_output=True, text=True)
    cams, video = [], False
    for line in r.stderr.splitlines():
        if "video devices" in line: video = True
        elif "audio devices" in line: video = False
        elif video:
            m = re.search(r"\[(\d+)\] (.+)$", line)
            if m: cams.append({"index": int(m.group(1)), "name": m.group(2).strip()})
    return cams

def serve_status():
    base, stats, th, o = SERVE["base"], SERVE["stats"], SERVE["thread"], SERVE["opts"]
    ready, total = stats.conns(); port = usb_port(base)
    cur = o or base
    return {"bridge": "esptube", "version": 1,
            "transport": ("serial" if cur.serial else "tcp") if (th and th.is_alive()) else None,
            "transport_mode": getattr(cur, "transport", "auto"), "baud": cur.baud,
            "usb": {"port": port, "connected": bool(port and os.path.exists(port))},
            "tcp": {"host": base.host, "connected": bool(not base.serial and ready), "conns": ready, "conns_total": total},
            "streaming": bool(th and th.is_alive()), "source": o.source if o else None,
            "layout": (o or base).layout, "fmt": (o or base).fmt, "dither": (o or base).dither,
            "bg": getattr(o or base, "bg", BG_DEFAULT) or BG_DEFAULT,
            "fps": round(stats.fps, 2), "fps_target": round(stats.target, 2) if stats.target else None,
            "kbps": round(stats.kbps, 1), "band_pct": round(stats.band_pct(), 1), "dropped": stats.dropped}

def serve_stop():
    th, stop = SERVE["thread"], SERVE["stop"]
    if th and th.is_alive():
        stop.set(); th.join(5)
    SERVE["thread"] = None

def parse_fps(v):
    return None if str(v).strip().lower() == "auto" else float(v)

def serve_start(body):
    serve_stop()
    a = argparse.Namespace(**vars(SERVE["base"]))
    a.source = str(body.get("source", "test"))
    a.layout = body.get("layout", a.layout); a.fmt = body.get("fmt", a.fmt); a.dither = body.get("dither", a.dither)
    a.fps = parse_fps(body.get("fps", "auto" if a.fps is None else a.fps)); a.max_fps = float(body.get("max_fps", a.max_fps))
    a.gap = int(body.get("gap", a.gap)); a.loop = bool(body.get("loop", a.loop)); a.band = bool(body.get("band", a.band))
    a.rotate_every = float(body.get("rotate_every", a.rotate_every)); a.tubes = body.get("tubes", a.tubes)
    a.chars = ",".join(parse_chars(body.get("chars", a.chars)))   # chars: sources only; ignored elsewhere
    a.bg = str(body.get("bg", getattr(a, "bg", BG_DEFAULT)) or BG_DEFAULT); parse_bg(a.bg)   # bad spec -> ValueError -> 400
    a.conns = int(body.get("conns", a.conns)); a.page_every = float(body.get("page_every", a.page_every))
    a.crop = body.get("crop", a.crop)
    a.transport = str(body.get("transport", getattr(a, "transport", "auto") or "auto")).lower()
    if a.transport not in ("auto", "tcp", "serial"):
        raise ValueError("transport must be auto|tcp|serial")
    if a.transport != "serial": a.serial = SERVE["base"].serial          # a forced port on the CLI still wins
    if a.layout not in ("tile", "span") or a.fmt not in FMTS or a.dither not in DITHERS:
        raise ValueError("layout must be tile|span, fmt raw|rle|pal4|pal4x2, dither fs|bayer|none")
    stats, stop = Stats(), threading.Event()

    def worker():
        try:
            run_stream(a, stats, stop, quiet=True)
        except SystemExit as e:
            print(f"stream stopped: {e}")
        except Exception as e:
            print(f"stream crashed: {e!r}")
    th = threading.Thread(target=worker, daemon=True); th.start()
    SERVE.update(thread=th, stop=stop, stats=stats, opts=a)

class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        pass

    def _send(self, code, obj):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")
        self.send_header("Access-Control-Allow-Private-Network", "true")     # a page on the LAN (the clock-hosted helper) may call loopback
        self.end_headers(); self.wfile.write(body)

    def do_OPTIONS(self):
        self.send_response(204)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")
        self.send_header("Access-Control-Allow-Private-Network", "true")
        self.send_header("Access-Control-Max-Age", "600"); self.end_headers()

    def do_GET(self):
        p = self.path.split("?")[0]
        if p == "/bridge/status":
            with SERVE["lock"]: self._send(200, serve_status())
        elif p == "/bridge/sources":
            self._send(200, {"sets": list_sets(), "webcams": list_webcams(), "dwarves": os.path.exists(DWARVES_MANIFEST),
                             "chrome": os.path.exists(SERVE["base"].chrome), "ffmpeg": shutil.which("ffmpeg") is not None})
        else:
            self._send(404, {"error": "try /bridge/status, /bridge/sources, POST /bridge/start, /bridge/stop"})

    def do_POST(self):
        p = self.path.split("?")[0]
        raw = self.rfile.read(int(self.headers.get("Content-Length") or 0))
        try:
            body = json.loads(raw) if raw else {}
        except ValueError:
            return self._send(400, {"error": "body must be JSON"})
        with SERVE["lock"]:
            if p == "/bridge/start":
                try:
                    serve_start(body)
                except (ValueError, TypeError) as e:
                    return self._send(400, {"error": str(e)})
                time.sleep(0.2)                   # let the worker fail fast on a bad source
                self._send(200, {"ok": True, **serve_status()})
            elif p == "/bridge/stop":
                serve_stop(); self._send(200, {"ok": True, **serve_status()})
            else:
                self._send(404, {"error": "unknown endpoint"})


# ---- commands --------------------------------------------------------------------------------
def cmd_play(a):
    stop = threading.Event()
    try:
        run_stream(a, Stats(), stop)
    except KeyboardInterrupt:
        stop.set(); print("\nstopped")

def start_console():
    """The console at http://localhost:8765 — a SECURE context, so camera / screen capture work (they do
    not on the copy the clock hosts over plain http). Same server as tools/console.py."""
    try:
        sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
        import console
        if console.serve_in_background(): print(f"console: http://localhost:{console.PORT}/   (open THIS one for camera / screen / tab capture)")
        else: print(f"console: http://localhost:{console.PORT}/ is already being served")
    except Exception as e:
        print(f"console: not started ({e}) — run  python3 tools/console.py")

def cmd_serve(a):
    start_console()
    SERVE["base"] = a
    srv = ThreadingHTTPServer(("127.0.0.1", a.api_port), Handler); srv.daemon_threads = True
    print(f"bridge API on http://127.0.0.1:{a.api_port}/bridge/status  (node: "
          f"{'fake' if a.fake else (a.serial or a.host)}; usb port: {usb_port(a) or 'none'})  Ctrl-C to quit")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print("\nshutting down")
    finally:
        with SERVE["lock"]: serve_stop()
        srv.server_close()

def cmd_shell(a):
    if serial is None:
        raise SystemExit(SERIAL_HINT)
    if not a.serial:
        raise SystemExit("shell needs --serial PORT (e.g. /dev/cu.usbserial-1220); ls /dev/cu.usbserial-*")
    ser = serial.Serial(a.serial, a.baud, timeout=0.2)
    print(f"shell on {a.serial} @{a.baud} — type a command (help), Ctrl-C to exit")

    def rx():
        try:
            while True:
                data = ser.read(ser.in_waiting or 1)
                if data:
                    sys.stdout.write(data.decode("utf-8", "replace")); sys.stdout.flush()
        except (OSError, TypeError):
            pass
    threading.Thread(target=rx, daemon=True).start()
    try:
        while True:
            ser.write((input().strip() + "\n").encode())
    except (KeyboardInterrupt, EOFError):
        print()
    finally:
        ser.close()

def cmd_bench(a):
    order = tube_order(a); ntubes = len(order)
    nconn = 1 if a.serial else (ntubes if a.conns <= 0 else max(1, min(a.conns, ntubes)))
    rows = []
    for name in ("raw", "rle", "pal4", "pal4x2"):
        b = argparse.Namespace(**vars(a)); b.source, b.fmt, b.fps, b.layout, b.loop = "test", name, 0.0, "span", False
        print(f"bench {name}: {a.seconds:.0f} s of the test pattern at max rate over {nconn} connection(s) …")
        stats, stop = Stats(), threading.Event()
        timer = threading.Timer(a.seconds, stop.set); timer.start()
        try:
            run_stream(b, stats, stop, quiet=True)
        finally:
            timer.cancel()
        el = max(1e-6, time.monotonic() - stats.t0); ups = stats.blits / el
        rows.append(f"{name:6} x{nconn} conn: {ups:7.1f} tube-updates/s = {ups / ntubes:5.1f} fps × {ntubes} · "
                    f"{stats.bytes / el / 1024:7.0f} KB/s · {stats.bytes / max(1, stats.blits) / 1024:.1f} KB/update · "
                    f"ack {stats.ack_ms or 0:.1f} ms")
    where = "fake node (Mac encode speed only)" if a.fake else (a.serial or f"{a.host}:{a.port}")
    print(f"\nbench — {where}, {a.seconds:.0f} s per format, tubes {order}, span layout, dither={a.dither}")
    for r in rows:
        print("  " + r)

def cmd_doctor(a):
    def row(ok, text):
        print(("✓ " if ok else "✗ ") + text)
    ff = shutil.which("ffmpeg")
    if ff:
        v = subprocess.run([ff, "-version"], capture_output=True, text=True).stdout.split("\n")[0]
        row(True, f"ffmpeg: {v.split(' Copyright')[0]}  ({ff})")
    else:
        row(False, "ffmpeg: not found — needed for webcam/screen/file/url sources: brew install ffmpeg yt-dlp")
    yt = shutil.which("yt-dlp")
    row(bool(yt), f"yt-dlp: {yt}" if yt else "yt-dlp: not found — needed for url: sources: brew install yt-dlp")
    row(os.path.exists(a.chrome), f"Google Chrome (page: sources): {a.chrome}" if os.path.exists(a.chrome)
        else f"Google Chrome (page: sources): not at {a.chrome} — {CHROME_HINT}")
    row(True, f"Pillow {Image.__version__}, python {sys.version.split()[0]}")
    row(serial is not None, f"pyserial: {serial.__version__} (serial streaming + shell available)" if serial
        else "pyserial: not installed for this python — " + SERIAL_HINT.split("\n")[1].strip())
    ports = sorted(glob.glob("/dev/cu.usbserial-*"))
    row(bool(ports), f"USB serial port: {', '.join(ports)}" if ports
        else "USB serial port: none (/dev/cu.usbserial-*) — clock not on the powered hub? (only needed for serial/shell)")
    if a.fake:
        row(True, f"node: fake node on {a.host}:{a.port} (--fake)")
    else:
        try:
            with urllib.request.urlopen(f"http://{a.host}/status", timeout=3) as r:
                st = json.load(r)
            row(True, f"REST http://{a.host}/status: fw {st.get('version', '?')}, populated_mask 0x{int(st.get('populated_mask', 0)):02X}"
                      f", tubes {[t for t in PHYSICAL if int(st.get('populated_mask', 0)) >> t & 1]} left->right")
        except Exception as e:
            row(False, f"REST http://{a.host}/status: {e} — is the clock on WiFi? (esptube.local / ESPTUBE_IP)")
    try:
        s = socket.create_connection((a.host, a.port), timeout=2); s.close()
        row(True, f"ESP/1 stream port tcp {a.host}:{a.port}: accepting connections")
    except OSError as e:
        row(False, f"ESP/1 stream port tcp {a.host}:{a.port}: {e} — firmware with the stream server flashed?")

def main():
    conn = argparse.ArgumentParser(add_help=False)
    conn.add_argument("--host", default=os.environ.get("ESPTUBE_IP", "esptube.local"), help="node host for TCP + REST (default esptube.local)")
    conn.add_argument("--port", type=int, default=5555, help="ESP/1 TCP port (default 5555)")
    conn.add_argument("--conns", type=int, default=0, help="TCP connections to the node (default 0 = one per populated tube; 1 = a single socket)")
    conn.add_argument("--serial", help="stream over this serial port instead of TCP (/dev/cu.usbserial-*)")
    conn.add_argument("--serial-raw", action="store_true", help="open --serial as a raw fd (no pyserial baud/exclusive ioctls) so a macOS PTY works — bench a node without hardware; skips the ESP32 boot/baud handshake and speaks ESP/1 datagrams straight away")
    conn.add_argument("--transport", choices=["auto", "tcp", "serial"], default="auto",
                      help="auto (default): the network if the node answers, else the USB cable; or force one")
    conn.add_argument("--baud", type=int, default=None, help="fastest serial rate to try (default 1500000: measured clean on this clock's CH340, 119 KB/s; every rung is verified and the ladder falls 1000000 → 460800 → 230400 → 115200; 921600 does not work; shell 115200)")
    conn.add_argument("--token", help="node stream token (HELLO <token>, sent on every connection)")
    conn.add_argument("--tubes", help="populated tubes left->right, e.g. 5,4,3,2,1 (default: ask /status)")
    conn.add_argument("--chrome", default=CHROME, help="Chrome binary for page: sources")
    conn.add_argument("--fake", action="store_true", help="talk to an in-process fake node (also ESPTUBE_FAKE=1)")
    stream = argparse.ArgumentParser(add_help=False)
    stream.add_argument("--layout", choices=("tile", "span"), default="tile")
    stream.add_argument("--geometry", default=None, metavar="FILE|URL|clock", help="physical layout (esptube.layout/1 from the helper's 🧊 Stage): sets the span seam gap from the real tube pitch; `clock` reads http://<host>/layout.json")
    stream.add_argument("--fps", default="auto", help="frames/s across all tubes: auto (default, follows the node), N, or 0 = as fast as acks allow")
    stream.add_argument("--max-fps", type=float, default=15, help="ceiling for --fps auto (default 15)")
    stream.add_argument("--fmt", choices=tuple(FMTS), default="pal4", help="pal4 (default) · pal4x2 row-doubled · rle · raw")
    stream.add_argument("--dither", choices=DITHERS, default="fs", help="PAL4/PAL4x2 dithering: fs (Floyd-Steinberg, default) · bayer 4x4 · none")
    stream.add_argument("--no-band", dest="band", action="store_false", help="always send full tiles (no row-band diffs)")
    stream.add_argument("--quant", choices=tuple(QUANT), default="median", help="PAL4 palette: median (MEDIANCUT) or octree (FASTOCTREE)")
    stream.add_argument("--gap", type=int, default=0, help="seam gap in pixels between tubes for span")
    stream.add_argument("--loop", action="store_true", help="loop file:/url: sources")
    stream.add_argument("--rotate-every", type=float, default=8, help="chars:/dir: shift sequences one tube every N s")
    stream.add_argument("--chars", default="", help="chars:SET: comma-separated character ids to play (default: everyone)")
    stream.add_argument("--bg", default=BG_DEFAULT, help="chars:/dir: what goes behind transparent frames: black (default) · #rrggbb · "
                        "#rrggbb-#rrggbb vertical gradient (top-bottom) · glow:#rrggbb radial glow over black (quote it: # starts a shell comment)")
    stream.add_argument("--page-every", type=float, default=5, help="page: re-screenshot every N s")
    stream.add_argument("--crop", help="screen: capture region x,y,w,h")

    p = argparse.ArgumentParser(prog="bridge.py", description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    sp = sub.add_parser("play", parents=[conn, stream], help="stream SOURCE to the tubes"); sp.add_argument("source")
    ss = sub.add_parser("serve", parents=[conn, stream], help="localhost control API for the helper")
    ss.add_argument("--api-port", type=int, default=8787)
    sub.add_parser("shell", parents=[conn], help="interactive text shell over the node's UART")
    sb = sub.add_parser("bench", parents=[conn, stream], help="test pattern at max rate per format, report tube-updates/s and KB/s")
    sb.add_argument("--seconds", type=float, default=10)
    sub.add_parser("doctor", parents=[conn], help="check ffmpeg / yt-dlp / Chrome / pyserial / USB port / node")
    a = p.parse_args()
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(line_buffering=True)   # keep log lines in order when piped

    global quant_method
    quant_method = QUANT[getattr(a, "quant", "median")]
    if hasattr(a, "fps"):
        try:
            a.fps = parse_fps(a.fps)
        except ValueError:
            raise SystemExit(f"--fps wants auto or a number (got {a.fps!r})")
    if a.baud is None:
        a.baud = 115200 if a.cmd == "shell" else 1500000         # measured on the CH340: 1.5 M clean; the ladder verifies every rung anyway
    if a.serial and serial is None and a.cmd != "shell" and not getattr(a, "serial_raw", False):
        raise SystemExit(SERIAL_HINT)   # --serial-raw uses a plain fd (RawSerial), so pyserial isn't required
    a.fake = a.fake or os.environ.get("ESPTUBE_FAKE") == "1"
    if a.fake:
        a.host, a.port = start_fake_node(a.token); a.serial = None
        print(f"fake node listening on {a.host}:{a.port}")
    {"play": cmd_play, "serve": cmd_serve, "shell": cmd_shell, "bench": cmd_bench, "doctor": cmd_doctor}[a.cmd](a)


if __name__ == "__main__":
    main()
