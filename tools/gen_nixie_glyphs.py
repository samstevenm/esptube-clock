#!/usr/bin/env python3
"""
gen_nixie_glyphs.py — bake the on-device Nixie glyph set into firmware.

Renders digits 0-9, letters A-Z and a few symbols as 135x240 8-bit INTENSITY
plates that look like a real nixie: a thin cathode-wire glyph (Nixie One, OFL)
with neon bloom. The "glass" — the other cathodes faintly stacked behind (ghost)
and a warm lit-glass vignette — is ONE shared BASE plate the firmware adds under
every glyph at draw time, so glyph plates are mostly zero and pack small.

The firmware colorizes intensity -> RGB565 through a brightness-scaled amber LUT
(tubes.cpp); cross-fades are a per-pixel blend of two glyph plates over the base.

Output: firmware/custom-fw/src/nixie_glyphs.h  (PROGMEM, RLE-packed)
  NIXIE_CHARS  — the characters in plate order (index = plate); ' ' is the blank
  NIXIE_BASE   — index of the shared ghost+glass plate (not a character)

Usage:  python3 tools/gen_nixie_glyphs.py          (needs Pillow; downloads the
        font into a scratch dir on first run — the TTF is NOT committed)

Font: "Nixie One" by Jovanny Lemonad, SIL Open Font License 1.1
      https://github.com/google/fonts/tree/main/ofl/nixieone
"""
import math, os, urllib.request
from PIL import Image, ImageDraw, ImageFont, ImageFilter, ImageChops

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT  = os.path.join(ROOT, "firmware", "custom-fw", "src", "nixie_glyphs.h")
FONT_URL = "https://github.com/google/fonts/raw/main/ofl/nixieone/NixieOne-Regular.ttf"
FONT = os.path.join(os.environ.get("TMPDIR", "/tmp"), "NixieOne-Regular.ttf")

W, H = 135, 240
MAX_W, MAX_H = 120, 168          # glyph box limits inside the 135x240 panel
CY = int(H * 0.50)               # visual centre line
GHOST = 12                       # unlit-cathode ghost intensity (0-255)
HALO1, HALO2 = 0.60, 0.32        # tight / wide bloom weights
VIG = 14                         # peak lit-glass vignette intensity
QUANT = 4                        # intensity quantisation step (RLE-friendlier)

DIGITS  = "0123456789"
LETTERS = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
SYMBOLS = "-:.!?°%+/"
CHARS   = DIGITS + LETTERS + SYMBOLS + " "     # ' ' = blank glyph (all zero), always last
WIDE    = "MW"                                 # letters wider than the box: squeezed to fit

if not os.path.exists(FONT):
    print("downloading", FONT_URL)
    urllib.request.urlretrieve(FONT_URL, FONT)

_meas = ImageDraw.Draw(Image.new("L", (1, 1)))
def box(ch, size):
    return _meas.textbbox((0, 0), ch, font=ImageFont.truetype(FONT, size))

def fit_size(chars, start=240):
    """largest font size whose widest/tallest glyph in `chars` fits the box"""
    size = start
    while size > 20:
        bs = [box(c, size) for c in chars]
        if max(b[2]-b[0] for b in bs) <= MAX_W and max(b[3]-b[1] for b in bs) <= MAX_H:
            return size
        size -= 2
    return size

size_d = fit_size(DIGITS)                                   # digits as big as the box allows
size_l = fit_size([c for c in LETTERS if c not in WIDE])    # letters: one size for all (M/W squeezed)
print(f"digit size {size_d}, letter size {size_l}")
font_d = ImageFont.truetype(FONT, size_d)
font_l = ImageFont.truetype(FONT, size_l)
bb0_d = box("0", size_d); gh_d = bb0_d[3] - bb0_d[1]     # digit cap box → common baseline
bb0_l = box("H", size_l); gh_l = bb0_l[3] - bb0_l[1]

def stroke(ch):
    """thin-wire glyph mask, centred on the common baseline for its class"""
    if ch == " ":
        return Image.new("L", (W, H), 0)
    letter = ch in LETTERS
    font, bb0, gh = (font_l, bb0_l, gh_l) if letter else (font_d, bb0_d, gh_d)
    bb = _meas.textbbox((0, 0), ch, font=font)
    gw, ghh = bb[2]-bb[0], bb[3]-bb[1]
    # render into a loose canvas, then (for wide letters) squeeze horizontally to MAX_W
    tmp = Image.new("L", (gw + 8, ghh + 8), 0)
    ImageDraw.Draw(tmp).text((4 - bb[0], 4 - bb[1]), ch, fill=255, font=font)
    if gw > MAX_W:
        tmp = tmp.resize((int(tmp.width * MAX_W / gw), tmp.height), Image.LANCZOS)
    im = Image.new("L", (W, H), 0)
    x = (W - tmp.width) // 2
    y = CY - gh // 2 - bb0[1] + bb[1] - 4        # keep this class's baseline
    im.paste(tmp, (x, y))
    return im

strokes = {c: stroke(c) for c in CHARS}

# shared BASE: ghost = union of the ten digit cathodes (what a real tube stacks), + glass vignette
ghost = Image.new("L", (W, H), 0)
for c in DIGITS: ghost = ImageChops.lighter(ghost, strokes[c])
base = Image.new("L", (W, H), 0); bp = base.load(); gp = ghost.load()
for y in range(H):
    for x in range(W):
        d = math.hypot((x - W/2) / (W*0.62), (y - CY) / (H*0.55))
        v = max(0.0, 1 - d)
        bp[x, y] = int(min(255, VIG * v * v + gp[x, y] * GHOST / 255.0))

def glyph_plate(ch):
    """bloom + core only (no ghost/glass) — the firmware adds BASE underneath"""
    s = strokes[ch]
    if ch == " ":
        return s
    h1 = s.filter(ImageFilter.GaussianBlur(4)).load()
    h2 = s.filter(ImageFilter.GaussianBlur(12)).load()
    sp = s.load()
    out = Image.new("L", (W, H), 0); o = out.load()
    for y in range(H):
        for x in range(W):
            o[x, y] = int(min(255, h2[x, y]*HALO2 + h1[x, y]*HALO1 + sp[x, y]))
    return out

def rle(data):
    out = bytearray(); i = 0; n = len(data)
    while i < n:
        v = data[i]; run = 1
        while i + run < n and data[i+run] == v and run < 255: run += 1
        out += bytes((run, v)); i += run
    return bytes(out)

plates = [glyph_plate(c) for c in CHARS] + [base]
plates = [p.point(lambda v: (v // QUANT) * QUANT) for p in plates]
packed = [rle(p.tobytes()) for p in plates]
BASE_IDX = len(CHARS)

def c_str(s):  # C string literal, escaping non-ASCII as UTF-8 bytes
    return '"' + "".join(c if 32 <= ord(c) < 127 and c not in '"\\' else
                         "".join(f"\\x{b:02x}" for b in c.encode("utf-8")) for c in s) + '"'

with open(OUT, "w") as f:
    f.write("// nixie_glyphs.h — GENERATED by tools/gen_nixie_glyphs.py. Do not edit.\n")
    f.write("// 135x240 8-bit intensity plates, RLE (count,value) pairs, PROGMEM.\n")
    f.write("// Glyph plates hold bloom+core only; NIXIE_BASE (ghost cathodes + glass) is added under them.\n")
    f.write("// Font: Nixie One (Jovanny Lemonad, SIL OFL 1.1) — rendered bitmaps only.\n")
    f.write("#pragma once\n#include <stdint.h>\n#ifdef ARDUINO\n#include <pgmspace.h>\n#else\n#ifndef PROGMEM\n#define PROGMEM\n#endif\n#endif\n\n")
    f.write(f"#define NIXIE_W {W}\n#define NIXIE_H {H}\n")
    f.write(f"#define NIXIE_PLATES {len(packed)}\n#define NIXIE_BASE {BASE_IDX}\n")
    f.write(f"#define NIXIE_BLANK {CHARS.index(' ')}\n#define NIXIE_NCHARS {len(CHARS)}\n")
    f.write(f"// plate index = position in NIXIE_CHARS (UTF-8; '°' is 2 bytes, see firmware plateOf)\n")
    f.write(f"static const char NIXIE_CHARS[] PROGMEM = {c_str(CHARS)};\n\n")
    for i, data in enumerate(packed):
        f.write(f"static const uint8_t NIXIE_RLE_{i}[{len(data)}] PROGMEM = {{\n")
        for k in range(0, len(data), 24):
            f.write("  " + ",".join(str(b) for b in data[k:k+24]) + ",\n")
        f.write("};\n")
    f.write("\nstatic const uint8_t* const NIXIE_RLE[NIXIE_PLATES] PROGMEM = {\n  ")
    f.write(", ".join(f"NIXIE_RLE_{i}" for i in range(len(packed))) + "\n};\n")
    f.write("static const uint32_t NIXIE_RLE_LEN[NIXIE_PLATES] PROGMEM = {\n  ")
    f.write(", ".join(str(len(d)) for d in packed) + "\n};\n")

total = sum(len(d) for d in packed)
print(f"wrote {OUT}: {len(packed)} plates ({len(CHARS)} chars + base), {total} bytes RLE ({total/1024:.0f} KB)")
print("largest:", sorted(((len(d), CHARS[i] if i < len(CHARS) else 'BASE') for i, d in enumerate(packed)), reverse=True)[:5])

# preview (not committed): colorised contact sheet next to the header
def lut(i):
    stops = [(0,(0,0,0)),(24,(38,7,0)),(80,(168,44,4)),(150,(255,108,18)),(215,(255,168,72)),(255,(255,228,175))]
    for (a, ca), (b, cb) in zip(stops, stops[1:]):
        if a <= i <= b:
            t = (i-a)/(b-a); return tuple(int(ca[j] + (cb[j]-ca[j])*t) for j in range(3))
    return stops[-1][1]
LUT = [lut(i) for i in range(256)]
cols = 16; rows = (len(CHARS) + cols - 1) // cols
sheet = Image.new("RGB", ((W+2)*cols+2, (H+2)*rows+2), (12, 12, 12))
basepx = plates[BASE_IDX].load()
for n, c in enumerate(CHARS):
    rgb = Image.new("RGB", (W, H)); rp = rgb.load(); ip = plates[n].load()
    for y in range(H):
        for x in range(W): rp[x, y] = LUT[min(255, ip[x, y] + basepx[x, y])]
    sheet.paste(rgb, (2 + (n % cols)*(W+2), 2 + (n // cols)*(H+2)))
prev = os.path.join(os.environ.get("TMPDIR", "/tmp"), "nixie_preview.png")
sheet.save(prev); print("preview:", prev)
