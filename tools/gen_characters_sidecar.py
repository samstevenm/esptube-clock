#!/usr/bin/env python3
"""
gen_characters_sidecar.py -- pack every rendered character set into
samples/characters.js, a classic script the helper page can load from a
file:// URL with a plain <script src> (which is why it is JS, not JSON):

    window.ESPTUBE_CHARACTERS = {"sets": {
        "<set>": {"title", "fps", "frames", "w": 135, "h": 240, "alpha": true,
                  "characters": [{"id", "label", "strip": "data:image/png;base64,..."}, ...]},
        ...}};

`strip` is one vertical PNG of all frames (frame k at y = k*h).  The frames
are transparent RGBA (tools/render_characters.py draws only the figure and a
semi-transparent floor shadow) and the strip keeps that alpha: it is an 8-bit
palette PNG of --colors entries (default 64), each entry carrying its own
alpha (a tRNS chunk), so the whole sidecar stays small.  The palette comes
from a median cut done here in RGBA (`median_cut_rgba`): Pillow's MEDIANCUT
only takes RGB, and FASTOCTREE -- the one Pillow quantiser that accepts RGBA --
measured almost twice this median cut's mean colour error on these strips
(3.3 vs 1.8 levels over black; the old opaque RGB strips got 1.1).  Index 0
is the fully transparent pixel, and opaque and translucent pixels never share
an entry, so the figure stays exactly opaque and only the anti-aliased edges
and the shadow are approximated.  `--colors 0` keeps the strips as lossless
8-bit RGBA instead (about 7x bigger).  `"alpha": true` at the set level tells
the helper the strips are transparent and must be drawn over a background of
its own.  Reads samples/*/manifest.json (written by
tools/render_characters.py, which also runs this after a render).

Usage:  python3 tools/gen_characters_sidecar.py [--colors 64] [--samples DIR] [--out FILE]
"""

import argparse
import array
import base64
import glob
import io
import json
import os
import struct
import sys

from PIL import Image


def _key(c):
    return c[0] | c[1] << 8 | c[2] << 16 | c[3] << 24        # RGBA bytes as one little-endian int


def _mean(box):
    cnt = float(sum(n for n, _ in box))
    return tuple(int(round(sum(n * c[i] for n, c in box) / cnt)) for i in range(4))


def _nearest_opaque(colours, entries):
    """Nearest entry (RGB distance) for each opaque colour, via Pillow's C palette matcher."""
    n = len(entries)
    pal = Image.new("P", (1, 1))
    flat = []
    for c in entries:
        flat.extend(c[:3])
    flat.extend(entries[0][:3] * (256 - n))                  # pad with copies of entry 0 ...
    pal.putpalette(flat)
    row = Image.new("RGB", (len(colours), 1))
    row.putdata([c[:3] for c in colours])
    return [i if i < n else 0 for i in row.quantize(palette=pal, dither=Image.Dither.NONE).tobytes()]  # ... remapped


def _nearest_trans(colours, entries):
    """Nearest entry (RGBA distance) for each translucent colour.  Pure Python, but only over the
    distinct colours (tens of thousands) x the handful of translucent entries."""
    ents = [(c[0], c[1], c[2], c[3], k) for k, c in enumerate(entries)]
    out = []
    for r, g, b, a in colours:
        best, bd = 0, 1 << 30
        for er, eg, eb, ea, k in ents:
            d = (r - er) ** 2 + (g - eg) ** 2 + (b - eb) ** 2 + (a - ea) ** 2
            if d < bd:
                bd, best = d, k
        out.append(best)
    return out


def median_cut_rgba(im, colors, split="product", refine=2):
    """Median cut in RGBA -> "P" image with an RGBA palette of <= `colors` entries.

    Entry 0 is (0,0,0,0) for every fully transparent pixel.  The visible colours start as two
    boxes -- opaque (alpha 255) and translucent -- so no palette entry mixes the two and the
    figure keeps alpha 255 exactly.  Each round splits one box at its count-weighted median
    along its longest axis; `split` picks the box: "product" (pixel count x range, default),
    "count" (Heckbert's original) or "range".  Every colour then goes to its *nearest* entry
    (not just its box's), and `refine` k-means passes move the entries to the mean of what
    they attracted.  Pure Python apart from Pillow's palette matcher, and it only ever
    touches the distinct colours (tens of thousands), not the pixels."""
    w, h = im.size
    allc = im.getcolors(w * h)
    opaque = [(n, c) for n, c in allc if c[3] == 255]
    trans = [(n, c) for n, c in allc if 0 < c[3] < 255]

    def stats(box):                       # -> (count, longest axis, its range)
        cnt = 0
        mn = [255, 255, 255, 255]
        mx = [0, 0, 0, 0]
        for n, c in box:
            cnt += n
            for i in range(4):
                if c[i] < mn[i]:
                    mn[i] = c[i]
                if c[i] > mx[i]:
                    mx[i] = c[i]
        axis = max(range(4), key=lambda i: mx[i] - mn[i])
        return cnt, axis, mx[axis] - mn[axis]

    def score(b):
        cnt, rng = b[1], b[3]
        return cnt * rng if split == "product" else (cnt if split == "count" else rng)

    boxes = [(b, *stats(b)) for b in (opaque, trans) if b]
    while len(boxes) < colors - 1:
        cand = [(score(b), k) for k, b in enumerate(boxes) if b[3] > 0 and len(b[0]) > 1]
        if not cand:
            break
        _, k = max(cand)
        box, cnt, axis, _ = boxes.pop(k)
        box.sort(key=lambda e: e[1][axis])
        acc, cut = 0, 0
        for cut, (n, _) in enumerate(box):
            acc += n
            if acc >= cnt / 2.0:
                break
        cut = min(cut + 1, len(box) - 1)                       # both halves non-empty
        lo, hi = box[:cut], box[cut:]
        boxes.append((lo, *stats(lo)))
        boxes.append((hi, *stats(hi)))

    # entries per class, then nearest-entry assignment (+ k-means refinement) within the class
    classes = []
    for cols, nearest in ((opaque, _nearest_opaque), (trans, _nearest_trans)):
        if not cols:
            continue
        entries = [_mean(b[0]) for b in boxes if b[0][0][1][3] == cols[0][1][3] or
                   (cols is trans and 0 < b[0][0][1][3] < 255)]
        colours = [c for _, c in cols]
        idx = None
        for _ in range(refine + 1):
            idx = nearest(colours, entries)
            if _ == refine:
                break
            groups = {}
            for (n, c), k in zip(cols, idx):
                groups.setdefault(k, []).append((n, c))
            entries = [_mean(groups[k]) for k in sorted(groups)]  # drop entries nothing chose
        classes.append((cols, entries, idx))

    pal, lut = [(0, 0, 0, 0)], {}
    for n, c in allc:
        if c[3] == 0:
            lut[_key(c)] = 0
    for cols, entries, idx in classes:
        base = len(pal)
        pal.extend(entries)
        for (_, c), k in zip(cols, idx):
            lut[_key(c)] = base + k
    raw = im.tobytes()
    if sys.byteorder == "little" and array.array("I").itemsize == 4:
        keys = array.array("I", raw)
    else:
        keys = (v for (v,) in struct.iter_unpack("<I", raw))
    out = Image.frombytes("P", (w, h), bytes(map(lut.__getitem__, keys)))
    flat = []
    for c in pal:
        flat.extend(c)
    out.putpalette(flat, "RGBA")
    return out


def strip_for(cdir, nframes, w, h, colors):
    """One vertical RGBA strip of a character's frames as PNG bytes (palette+alpha if colors > 0)."""
    strip = Image.new("RGBA", (w, h * nframes), (0, 0, 0, 0))
    for k in range(nframes):
        p = os.path.join(cdir, "f%02d.png" % k)
        if not os.path.exists(p):
            raise SystemExit("missing frame %s" % p)
        im = Image.open(p).convert("RGBA")
        if im.size != (w, h):
            im = im.resize((w, h), Image.LANCZOS)
        strip.paste(im, (0, k * h))
    out = median_cut_rgba(strip, colors) if colors > 0 else strip
    buf = io.BytesIO()
    out.save(buf, format="PNG", optimize=True, compress_level=9)
    return buf.getvalue()


def main(argv=None):
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--samples", default=os.path.join(here, "..", "samples"), help="samples/ directory")
    ap.add_argument("--out", default=None, help="output file (default samples/characters.js)")
    ap.add_argument("--colors", type=int, default=64,
                    help="palette size per strip, alpha included (default 64); 0 = lossless RGBA, no palette")
    ap.add_argument("--sets", default="", help="comma-separated subset of sets (default: every manifest found)")
    args = ap.parse_args(argv)

    samples = os.path.normpath(args.samples)
    out = args.out or os.path.join(samples, "characters.js")
    only = [s.strip() for s in args.sets.split(",") if s.strip()]
    sets = {}
    sizes = []
    for mp in sorted(glob.glob(os.path.join(samples, "*", "manifest.json"))):
        set_dir = os.path.dirname(mp)
        with open(mp) as fh:
            man = json.load(fh)
        set_id = man.get("set") or os.path.basename(set_dir)
        if only and set_id not in only:
            continue
        w, h = man.get("w"), man.get("h")
        if not (w and h):
            w, h = man.get("size", [135, 240])
        nframes = int(man.get("frames") or (man["characters"][0].get("frames") if man.get("characters") else 36))
        chars, total = [], 0
        for c in man.get("characters", []):
            cid = c.get("name") or c.get("id")
            cdir = os.path.join(set_dir, cid)
            png = strip_for(cdir, int(c.get("frames", nframes)), w, h, args.colors)
            total += len(png)
            chars.append({"id": cid, "label": c.get("label", cid.capitalize()),
                          "strip": "data:image/png;base64," + base64.b64encode(png).decode("ascii")})
        sets[set_id] = {"title": man.get("title", set_id), "fps": man.get("fps", 12), "frames": nframes,
                        "w": w, "h": h, "alpha": True, "characters": chars}
        sizes.append((set_id, len(chars), total))
    if not sets:
        raise SystemExit("no samples/*/manifest.json found under %s" % samples)
    body = json.dumps({"sets": sets}, separators=(",", ":"))
    with open(out, "w") as fh:
        fh.write("// generated by tools/gen_characters_sidecar.py -- do not edit\n")
        fh.write("window.ESPTUBE_CHARACTERS = %s;\n" % body)
    for set_id, n, total in sizes:
        print("sidecar: %-8s %d characters, strips %5.0f KB png (RGBA)" % (set_id, n, total / 1024.0))
    print("sidecar: %s  %.2f MB total (%s)" % (os.path.relpath(out, os.getcwd()), os.path.getsize(out) / 1048576.0,
                                              "%d colours/strip incl. alpha" % args.colors if args.colors > 0 else "lossless RGBA"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
