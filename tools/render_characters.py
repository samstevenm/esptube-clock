#!/usr/bin/env python3
"""
render_characters.py -- tiny pure-Python software 3D renderer + three SETS of
eight original low-poly characters each, rendered as seamless turntable loops
for the 135x240 ESPTube IPS display.

Sets (all designs, names, colours and props are our own -- evocative homages,
nothing traced or named after a film; released CC0):

  dwarves  the public-domain Grimm tale "Snow White": a princess and seven
           short bearded fellows -- princess, miner, baker, fiddler, snoozer,
           grouch, shy, giggler.
  island   a Hawaiian island family and their alien friends -- keiki, blue,
           sister, surfer, professor, noodle, pink, agent.
  winter   a snowy kingdom -- icequeen, sunny, snowman, iceman, reindeer,
           prince, troll, snowgiant.

Renderer (stdlib + Pillow only, no numpy):
  * characters are assembled from primitives -- lat/long spheres (ellipsoids,
    optionally clipped with a `keep` predicate for hair/decals), frusta
    (cylinders and cones between two arbitrary points) and boxes;
  * each polygon is oriented outward at build time, so a per-frame back-face
    test halves the draw list and reduces painter's-algorithm artefacts;
  * turntable = rotate the mesh about the vertical (Y) axis, 360/N deg/frame;
  * every builder takes a phase (frame/N) and bakes a subtle idle motion into
    the loop -- a prop sway or head tilt -- and the renderer adds a gentle
    "breathing" bob (about +-2 px, feet planted) so the loop is alive yet
    seamless (all motion is sin(2*pi*k*phase) with integer k);
  * simple look-at perspective camera, pitched ~10 deg down so the floor
    ellipse is visible;
  * painter's algorithm: polygons sorted by distance of their centroid from
    the eye, far to near;
  * flat Lambert shading: ambient + key light + weak fill + rim, lights fixed
    in camera space so shading changes as the figure turns;
  * a two-tone floor shadow under the feet -- a soft disc and a denser contact
    shadow, both semi-transparent black, so it darkens whatever the frame is
    composited over;
  * `ImageDraw.polygon` on a transparent RGBA canvas, drawn at `--ss` x
    supersampling (default 3) and downscaled with LANCZOS (Pillow resizes RGBA
    premultiplied, so the anti-aliased edges carry alpha and blend cleanly
    over any background instead of being baked against black).

Outputs (under samples/<set>/):
  <name>/f00.png .. f35.png   individual frames: RGBA, transparent background
  <name>.gif                  looping animation (12 fps), matted over black
  sheet.png                   contact sheet, front view of everyone, matted over dark grey
  manifest.json, README.md
and samples/characters.js (all sets as base64 PNG strips, for the helper page;
see tools/gen_characters_sidecar.py).

Usage:
  python3 tools/render_characters.py                 # every set
  python3 tools/render_characters.py --set winter    # one set
  python3 tools/render_characters.py --set island --only blue,pink   # iterate
  Options: --frames 36 --size 135x240 --ss 3 --fps 12 --out DIR --no-sidecar
"""

import argparse
import json
import math
import os
import sys
import time

from PIL import Image, ImageDraw, ImageFont

TAU = 2 * math.pi


# --------------------------------------------------------------------------
# vector / matrix helpers (tuples; no numpy)
# --------------------------------------------------------------------------

def v_add(a, b):
    return (a[0] + b[0], a[1] + b[1], a[2] + b[2])


def v_sub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def v_mul(a, s):
    return (a[0] * s, a[1] * s, a[2] * s)


def v_dot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def v_cross(a, b):
    return (a[1] * b[2] - a[2] * b[1],
            a[2] * b[0] - a[0] * b[2],
            a[0] * b[1] - a[1] * b[0])


def v_len(a):
    return math.sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2])


def v_norm(a):
    length = v_len(a)
    if length < 1e-12:
        return (0.0, 0.0, 0.0)
    return (a[0] / length, a[1] / length, a[2] / length)


def v_lerp(a, b, t):
    return (a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t)


def centroid(pts):
    n = float(len(pts))
    return (sum(p[0] for p in pts) / n,
            sum(p[1] for p in pts) / n,
            sum(p[2] for p in pts) / n)


def rot_matrix(rx, ry, rz):
    """3x3 rotation matrix (row tuples) for Euler angles in degrees,
    applied in the order X, then Y, then Z."""
    ax, ay, az = math.radians(rx), math.radians(ry), math.radians(rz)
    cx, sx = math.cos(ax), math.sin(ax)
    cy, sy = math.cos(ay), math.sin(ay)
    cz, sz = math.cos(az), math.sin(az)
    mx = ((1, 0, 0), (0, cx, -sx), (0, sx, cx))
    my = ((cy, 0, sy), (0, 1, 0), (-sy, 0, cy))
    mz = ((cz, -sz, 0), (sz, cz, 0), (0, 0, 1))
    return m_mul(mz, m_mul(my, mx))


def m_mul(a, b):
    return tuple(tuple(sum(a[i][k] * b[k][j] for k in range(3)) for j in range(3))
                 for i in range(3))


def m_apply(m, p):
    return (m[0][0] * p[0] + m[0][1] * p[1] + m[0][2] * p[2],
            m[1][0] * p[0] + m[1][1] * p[1] + m[1][2] * p[2],
            m[2][0] * p[0] + m[2][1] * p[1] + m[2][2] * p[2])


def osc(ph, k=1, phase=0.0):
    """Seamless idle oscillation: sin of k whole cycles per loop."""
    return math.sin(TAU * (k * ph + phase))


# --------------------------------------------------------------------------
# materials
# --------------------------------------------------------------------------

class Mat(object):
    """Flat colour + ambient floor.  Dark materials (hair) get a higher
    ambient so they still separate from a black background (the default matte
    in the bridge, the GIFs and the helper)."""
    __slots__ = ("rgb", "amb")

    def __init__(self, rgb, amb=0.30):
        self.rgb = rgb
        self.amb = amb


SKIN = Mat((238, 194, 152))
PALE = Mat((250, 232, 220))
TAN = Mat((196, 140, 92))
NOSE = Mat((236, 140, 110))
NOSE_RED = Mat((222, 110, 96))
WHITE = Mat((250, 250, 250))
EYE_WHITE = Mat((252, 252, 252), amb=0.55)
BLACK = Mat((16, 16, 24), amb=0.2)
INK = Mat((28, 26, 34), amb=0.45)          # dark hair / suits: reads on black
MOUTH = Mat((120, 30, 40), amb=0.5)
LIPS = Mat((215, 60, 80), amb=0.5)
BEARD_WHITE = Mat((244, 242, 234))
BEARD_GREY = Mat((172, 176, 186))
BEARD_GINGER = Mat((218, 112, 38))
BOOT = Mat((92, 58, 32))
BELT = Mat((66, 42, 26))
GOLD = Mat((242, 198, 58), amb=0.45)
HAIR = Mat((74, 68, 112), amb=0.70)
WOOD = Mat((150, 96, 52))
IRON = Mat((150, 156, 168), amb=0.4)
BLUSH = Mat((240, 150, 150))
GREEN = Mat((58, 156, 84))
DRESS_WHITE = Mat((246, 246, 240))
RED = Mat((222, 40, 48), amb=0.4)
FLOOR_RGBA = (0, 0, 0, 56)       # soft floor disc: semi-transparent black, darkens whatever is behind
SHADOW_RGBA = (0, 0, 0, 110)     # denser contact shadow under the feet
SHEET_BG = (40, 40, 40)          # sheet.png mattes the transparent frames over this dark grey


# --------------------------------------------------------------------------
# primitives -> list of (points, Mat) faces in world space
# --------------------------------------------------------------------------

def _orient(faces, mat, center):
    """Make every (convex-primitive) face wind so its normal points away from
    `center`; the renderer relies on this for back-face culling."""
    out = []
    for pts in faces:
        c = centroid(pts)
        n = v_cross(v_sub(pts[1], pts[0]), v_sub(pts[2], pts[0]))
        if v_dot(n, v_sub(c, center)) < 0:
            pts = list(reversed(pts))
        out.append((pts, mat))
    return out


def _radii(r):
    if isinstance(r, (int, float)):
        return (float(r), float(r), float(r))
    return r


def sphere(center, radii, mat, slices=12, stacks=8, rot=None, keep=None):
    """Lat/long sphere or ellipsoid.  `keep(p_local)` may drop faces (used
    for hair caps and decals); p_local is the scaled, un-rotated centroid."""
    rx, ry, rz = _radii(radii)
    rings = []
    for i in range(stacks + 1):
        phi = math.pi * i / stacks
        y = math.cos(phi)
        r = math.sin(phi)
        ring = []
        for j in range(slices):
            th = 2 * math.pi * j / slices
            ring.append((r * math.cos(th) * rx, y * ry, r * math.sin(th) * rz))
        rings.append(ring)
    faces = []
    for i in range(stacks):
        for j in range(slices):
            j2 = (j + 1) % slices
            if i == 0:
                pts = [rings[0][0], rings[1][j], rings[1][j2]]
            elif i == stacks - 1:
                pts = [rings[i][j], rings[i + 1][0], rings[i][j2]]
            else:
                pts = [rings[i][j], rings[i + 1][j], rings[i + 1][j2], rings[i][j2]]
            if keep is not None and not keep(centroid(pts)):
                continue
            faces.append(pts)
    m = rot_matrix(*rot) if rot else None
    placed = []
    for pts in faces:
        if m:
            pts = [m_apply(m, p) for p in pts]
        placed.append([v_add(p, center) for p in pts])
    return _orient(placed, mat, center)


def frustum(p0, p1, r0, r1, mat, n=12, caps=True):
    """Truncated cone between two points (r1=0 -> cone, r0=r1 -> cylinder)."""
    axis = v_norm(v_sub(p1, p0))
    helper = (1.0, 0.0, 0.0) if abs(axis[0]) < 0.9 else (0.0, 1.0, 0.0)
    e1 = v_norm(v_cross(axis, helper))
    e2 = v_cross(axis, e1)

    def ring(p, r):
        out = []
        for j in range(n):
            th = 2 * math.pi * j / n
            out.append(v_add(p, v_add(v_mul(e1, r * math.cos(th)),
                                      v_mul(e2, r * math.sin(th)))))
        return out

    ring0 = ring(p0, r0) if r0 > 0 else None
    ring1 = ring(p1, r1) if r1 > 0 else None
    faces = []
    for j in range(n):
        j2 = (j + 1) % n
        if ring1 is None and ring0 is None:
            break
        if ring1 is None:
            faces.append([ring0[j], ring0[j2], p1])
        elif ring0 is None:
            faces.append([p0, ring1[j], ring1[j2]])
        else:
            faces.append([ring0[j], ring1[j], ring1[j2], ring0[j2]])
    if caps:
        if ring0 is not None:
            faces.append(list(ring0))
        if ring1 is not None:
            faces.append(list(ring1))
    center = v_mul(v_add(p0, p1), 0.5)
    return _orient(faces, mat, center)


def box(center, size, mat, rot=None):
    sx, sy, sz = size[0] / 2.0, size[1] / 2.0, size[2] / 2.0
    c = [(x, y, z) for x in (-sx, sx) for y in (-sy, sy) for z in (-sz, sz)]
    # index = x*4 + y*2 + z
    quads = [
        [0, 1, 3, 2], [4, 6, 7, 5],   # -x, +x
        [0, 4, 5, 1], [2, 3, 7, 6],   # -y, +y
        [0, 2, 6, 4], [1, 5, 7, 3],   # -z, +z
    ]
    m = rot_matrix(*rot) if rot else None
    faces = []
    for q in quads:
        pts = [c[i] for i in q]
        if m:
            pts = [m_apply(m, p) for p in pts]
        faces.append([v_add(p, center) for p in pts])
    return _orient(faces, mat, center)


def xform(faces, rot=(0, 0, 0), pivot=(0, 0, 0), offset=(0, 0, 0)):
    """Rigidly move a group of faces: rotate about `pivot`, then translate."""
    m = rot_matrix(*rot)
    out = []
    for pts, mat in faces:
        out.append(([v_add(v_add(m_apply(m, v_sub(p, pivot)), pivot), offset)
                     for p in pts], mat))
    return out


def tube(p0, p1, r, mat, n=10):
    return frustum(p0, p1, r, r, mat, n)


def polyline(pts, r, mat, n=6, joints=True):
    """A bent tube through `pts` (smiles, twigs, antlers, braids)."""
    F = []
    for a, b in zip(pts, pts[1:]):
        F += frustum(a, b, r, r, mat, n, caps=False)
    if joints:
        for p in pts:
            F += sphere(p, r, mat, n, 4)
    return F


def bob_mesh(faces, dy, y_lo, y_hi):
    """The idle 'breath': vertices above y_hi rise by dy, below y_lo stay
    put (feet planted), smoothstep in between."""
    if abs(dy) < 1e-9:
        return faces
    span = max(1e-6, y_hi - y_lo)
    out = []
    for pts, mat in faces:
        q = []
        for x, y, z in pts:
            t = (y - y_lo) / span
            t = 0.0 if t < 0 else (1.0 if t > 1 else t)
            q.append((x, y + dy * t * t * (3 - 2 * t), z))
        out.append((q, mat))
    return out


# --------------------------------------------------------------------------
# surface helpers: put things on a (possibly ellipsoidal) head
# --------------------------------------------------------------------------

def on_sphere(c, radii, yaw, pitch, out=0.0):
    """Point on an ellipsoid at (yaw, pitch) degrees; +z is the face,
    +yaw is the figure's left (screen right in the front view)."""
    rx, ry, rz = _radii(radii)
    ya, pa = math.radians(yaw), math.radians(pitch)
    d = (math.sin(ya) * math.cos(pa), math.sin(pa), math.cos(ya) * math.cos(pa))
    return (c[0] + d[0] * (rx + out), c[1] + d[1] * (ry + out), c[2] + d[2] * (rz + out))


def stick(faces, hc, hr, yaw, pitch, out=0.0):
    """Faces built around the origin with +z outward, rotated to face
    (yaw, pitch) and placed on the surface."""
    return xform(faces, rot=(-pitch, yaw, 0), offset=on_sphere(hc, hr, yaw, pitch, out))


def arc(hc, hr, pitch, half_yaw, curve, r, mat, n=7, out=0.0, tilt=0.0, yaw0=0.0):
    """A thin bent tube on the surface: a mouth.  curve > 0 lifts the corners
    (smile), < 0 drops them (frown); tilt raises one corner (smug)."""
    pts = []
    for k in range(n + 1):
        t = -1.0 + 2.0 * k / n
        pts.append(on_sphere(hc, hr, yaw0 + t * half_yaw, pitch + curve * t * t + tilt * t, out))
    return polyline(pts, r, mat, 6)


def decal(hc, hr, mat, spots, slices=24, stacks=16, lift=None):
    """Patches of colour lying on the head: faces of a slightly larger sphere
    within `ang` degrees of each (yaw, pitch, ang) spot -- blush, freckles.
    The lift grows with the radius so the painter's sort keeps the patch in
    front of the coarser body faces underneath."""
    rx, ry, rz = _radii(hr)
    if lift is None:
        lift = 0.003 + 0.025 * max(rx, ry, rz)
    dirs = []
    for yaw, pitch, ang in spots:
        ya, pa = math.radians(yaw), math.radians(pitch)
        dirs.append(((math.sin(ya) * math.cos(pa), math.sin(pa), math.cos(ya) * math.cos(pa)),
                     math.cos(math.radians(ang))))

    def keep(p):
        d = v_norm((p[0] / rx, p[1] / ry, p[2] / rz))
        return any(v_dot(d, sd) > c for sd, c in dirs)
    return sphere(hc, (rx + lift, ry + lift, rz + lift), mat, slices, stacks, keep=keep)


def hair_cap(hc, hr, mat, bottom=-0.15, front=0.30, fringe=0.45, grow=0.03,
             slices=18, stacks=11, back_only=False):
    """A shell over the head with the face cut out: keep everything above
    `bottom`, plus the front only above `fringe` (fractions of the radius)."""
    rx, ry, rz = _radii(hr)
    rr = (rx + grow, ry + grow, rz + grow)

    def keep(p):
        y, z = p[1] / rr[1], p[2] / rr[2]
        if back_only:
            return z < front and y > bottom
        return y > bottom and (z < front or y > fringe)
    return sphere(hc, rr, mat, slices, stacks, keep=keep)


def beads(path, r, mat, n_beads, taper=0.0, wobble=0.12):
    """Spheres along a polyline path: braids, ponytails, tails."""
    F = []
    # total length
    segs = list(zip(path, path[1:]))
    lens = [v_len(v_sub(b, a)) for a, b in segs]
    total = sum(lens) or 1e-6
    for k in range(n_beads):
        s = total * k / max(1, n_beads - 1)
        acc = 0.0
        p = path[-1]
        for (a, b), L in zip(segs, lens):
            if s <= acc + L or L == 0:
                p = v_lerp(a, b, (s - acc) / L if L else 0)
                break
            acc += L
        rr = r * (1 - taper * k / max(1, n_beads - 1)) * (1 + wobble * (k % 2))
        F += sphere(p, rr, mat, 10, 7)
    return F


# --------------------------------------------------------------------------
# body parts shared by the sets
# --------------------------------------------------------------------------

def arm(shoulder, hand, sleeve, elbow=None, r=0.058, hand_r=0.07,
        hand_mat=SKIN, n=10):
    F = []
    if elbow is None:
        F += tube(shoulder, hand, r, sleeve, n)
    else:
        F += tube(shoulder, elbow, r, sleeve, n)
        F += sphere(elbow, r, sleeve, 8, 5)
        F += tube(elbow, hand, r, sleeve, n)
    F += sphere(hand, hand_r, hand_mat, 12, 8)
    return F


def leg(hip, foot, mat, r=0.075, shoe=BOOT, shoe_size=(0.15, 0.09, 0.22), shoe_z=0.03):
    F = tube(hip, foot, r, mat, 10)
    if shoe is not None:
        F += box((foot[0], shoe_size[1] / 2.0, foot[2] + shoe_z), shoe_size, shoe)
    return F


def eye(hc, hr, yaw, pitch, r, style="open", white=EYE_WHITE, pupil=BLACK,
        pr=None, look=(0.0, 0.0), lash=None, shine=True):
    """An eye on the head at (yaw, pitch).  Styles: open, dark (big alien
    eye), closed (a line), happy (an upward arc)."""
    F = []
    if style == "closed":
        f = sphere((0, 0, 0), (r, r * 0.22, r * 0.35), BLACK, 8, 4)
        return stick(f, hc, hr, yaw, pitch, 0.005)
    if style == "happy":
        return arc(hc, hr, pitch, 6, -4, r * 0.24, BLACK, 4, 0.006, yaw0=yaw)
    pr = pr if pr is not None else r * 0.5
    if style == "dark":
        c = on_sphere(hc, hr, yaw, pitch, -r * 0.4)
        F += sphere(c, (r, r * 1.2, r * 0.8), pupil, 14, 9, rot=(-pitch, yaw, 0))
        if shine:
            F += sphere(on_sphere(c, (r, r * 1.2, r * 0.8), yaw * 0.4 - 12, pitch + 28, 0.002),
                        r * 0.26, EYE_WHITE, 8, 5)
        return F
    c = on_sphere(hc, hr, yaw, pitch, -r * 0.45)
    F += sphere(c, r, white, 12, 8)
    F += sphere(on_sphere(c, r, yaw * 0.35 + look[0], pitch * 0.3 + look[1], -pr * 0.55), pr, pupil, 10, 7)
    if lash is not None:
        F += arc(hc, hr, pitch + 7, 7, -3, r * 0.2, lash, 4, r * 0.55 + 0.004, yaw0=yaw)
    return F


def brow(hc, hr, yaw, pitch, mat, tilt=0.0, size=(0.06, 0.02, 0.025), out=0.004):
    f = sphere((0, 0, 0), size, mat, 8, 5, rot=(0, 0, tilt))
    return stick(f, hc, hr, yaw, pitch, out)


def mouth(hc, hr, kind, mat=MOUTH, pitch=-27, half_yaw=13, r=0.012, tilt=0.0, out=0.003):
    """smile | frown | line | smug | o | grin (dark with teeth) | wide"""
    if kind == "smile":
        return arc(hc, hr, pitch, half_yaw, 6, r, mat, 7, out, tilt)
    if kind == "frown":
        return arc(hc, hr, pitch, half_yaw, -5, r, mat, 7, out, tilt)
    if kind == "line":
        return arc(hc, hr, pitch, half_yaw, 0, r, mat, 3, out, tilt)
    if kind == "smug":
        return arc(hc, hr, pitch, half_yaw, 3, r, mat, 7, out, 6)
    if kind == "o":
        return stick(sphere((0, 0, 0), (r * 2.4, r * 3.0, r * 1.5), mat, 8, 5), hc, hr, 0, pitch, out)
    rx = _radii(hr)[0]
    if kind == "grin":
        w = rx * math.sin(math.radians(half_yaw))
        F = stick(sphere((0, 0, 0), (w, w * 0.42, 0.02), mat, 12, 7), hc, hr, 0, pitch, out)
        F += stick(sphere((0, 0.010, 0), (w * 0.82, w * 0.16, 0.024), WHITE, 10, 5), hc, hr, 0, pitch + 4, out)
        return F
    if kind == "wide":     # big open smile: dark ellipse + tongue
        w = rx * math.sin(math.radians(half_yaw))
        F = stick(sphere((0, 0, 0), (w, w * 0.55, 0.02), mat, 12, 7), hc, hr, 0, pitch, out)
        F += stick(sphere((0, -0.012, 0), (w * 0.5, w * 0.2, 0.024), LIPS, 8, 5), hc, hr, 0, pitch - 5, out)
        return F
    return []


def nose(hc, hr, r, mat=NOSE, pitch=-6, shape=None):
    shape = shape or (r, r * 0.85, r)
    return sphere(on_sphere(hc, hr, 0, pitch, -r * 0.35), shape, mat, 10, 7)


def ears(hc, hr, mat, size=(0.04, 0.052, 0.036), pitch=-3):
    F = []
    for sx in (-1, 1):
        F += sphere(on_sphere(hc, hr, sx * 90, pitch, -size[0] * 0.5), size, mat, 8, 5)
    return F


# --------------------------------------------------------------------------
# a plain two-legged body (the new sets); the dwarves keep their own builder
# --------------------------------------------------------------------------

def biped(cfg):
    """Legs + shoes, torso (frustum hip->shoulder with a rounded shoulder
    cap), neck and a bare head; face/hair/outfit/props are added by the
    character.  All keys optional except skin."""
    skin = cfg["skin"]
    F = []
    hip_y = cfg.get("hip_y", 0.80)
    sh_y = cfg.get("shoulder_y", 1.30)
    leg_dx = cfg.get("leg_dx", 0.11)
    leg_r = cfg.get("leg_r", 0.075)
    if cfg.get("legs", True):
        for sx in (-1, 1):
            F += leg((sx * leg_dx, hip_y, 0.0), (sx * leg_dx, 0.05, 0.0), cfg.get("leg_mat", skin),
                     leg_r, cfg.get("shoe", BOOT), cfg.get("shoe_size", (0.15, 0.09, 0.22)))
    torso = cfg.get("torso", skin)
    rh, rs = cfg.get("hip_r", 0.24), cfg.get("shoulder_r", 0.22)
    F += frustum((0, hip_y - 0.02, 0), (0, sh_y, 0), rh, rs, torso, cfg.get("torso_n", 18))
    F += sphere((0, sh_y, 0), (rs * 1.12, cfg.get("shoulder_h", 0.07), rs * 0.78),
                cfg.get("shoulder_mat", torso), 16, 6)
    if cfg.get("neck", True):
        F += tube((0, sh_y - 0.02, 0), (0, cfg["head_c"][1], 0), cfg.get("neck_r", 0.065), skin, 10)
    F += sphere(cfg["head_c"], cfg["head_r"], skin, cfg.get("head_slices", 20), cfg.get("head_stacks", 14))
    for sh, el, hd, mat, r, hm in cfg.get("arms", []):
        F += arm(sh, hd, mat, elbow=el, r=r, hand_r=r * 1.15, hand_mat=hm)
    return F


# ==========================================================================
# SET 1: dwarves -- a princess and seven short bearded fellows (Grimm)
# ==========================================================================

def pointy_hat(mat, base=(0, 0.94, 0.02), r=0.235, tip=(0, 1.34, 0.02),
               mid=None, brim=True, brim_r=0.275, band=None):
    F = []
    if brim:
        F += frustum(base, v_add(base, (0, 0.03, 0)), brim_r, brim_r, mat, 20)
    cone_base = v_add(base, (0, 0.015, 0))
    if mid is None:
        F += frustum(cone_base, tip, r, 0.0, mat, 16, caps=False)
    else:
        mid_pt, mid_r = mid
        F += frustum(cone_base, mid_pt, r, mid_r, mat, 16, caps=False)
        F += frustum(mid_pt, tip, mid_r, 0.0, mat, 16, caps=False)
    if band is not None:
        F += frustum(v_add(base, (0, 0.03, 0)), v_add(base, (0, 0.09, 0)),
                     r + 0.012, r * 0.86 + 0.012, band, 18, caps=False)
    return F


DWARF_HEAD = (0.0, 0.80, 0.02)
DWARF_HEAD_R = 0.22
DWARF_NECK = (0.0, 0.62, 0.02)
DWARF_SHOULDER = 0.52


def build_dwarf(cfg, ph):
    """Generic short bearded fellow; `cfg` picks colours, pose and props.
    The head tilts gently with the phase (k=1 -> one nod per turn)."""
    tunic = cfg["tunic"]
    trousers = cfg.get("trousers", Mat((70, 66, 96)))
    beard = cfg["beard"]
    F = []

    # legs and boots
    for sx in (-1, 1):
        F += frustum((sx * 0.10, 0.05, 0.0), (sx * 0.10, 0.27, 0.0),
                     0.063, 0.063, trousers, 10)
        F += box((sx * 0.10, 0.045, 0.04), (0.14, 0.09, 0.19), BOOT)

    # body, belt, buckle
    F += sphere((0, 0.42, 0), (0.28, 0.27, 0.25), tunic, 20, 12)
    F += frustum((0, 0.325, 0), (0, 0.39, 0), 0.29, 0.29, BELT, 20)
    F += box((0, 0.357, 0.278), (0.075, 0.06, 0.04), GOLD)
    for extra in cfg.get("body_extras", []):
        F += extra

    # arms
    arms = cfg.get("arms")
    if arms is None:
        arms = [((-0.24, DWARF_SHOULDER, 0.02), None, (-0.31, 0.27, 0.10)),
                ((0.24, DWARF_SHOULDER, 0.02), None, (0.31, 0.27, 0.10))]
    for sh, el, hd in arms:
        F += arm(sh, hd, tunic, elbow=el)

    # ---- head group (built in place, then tilted about the neck) ----
    H = []
    hc, hr = DWARF_HEAD, DWARF_HEAD_R
    H += sphere(hc, hr, SKIN, 20, 14)
    if cfg.get("ears", True):
        H += ears(hc, hr, SKIN)
    # big round nose
    H += sphere((0, 0.77, 0.235), 0.085, cfg.get("nose", NOSE), 12, 9)
    # eyes
    eyes = cfg.get("eyes", "open")
    for sx in (-1, 1):
        if eyes == "open":
            H += sphere((sx * 0.085, 0.865, 0.195), 0.052, EYE_WHITE, 12, 8)
            H += sphere((sx * 0.085, 0.86, 0.236), 0.026, BLACK, 10, 7)
        else:  # closed: a dark line
            H += sphere((sx * 0.085, 0.862, 0.216), (0.05, 0.012, 0.02), BLACK, 8, 4)
    # bushy brows (tilt > 0 = angry, < 0 = raised/surprised)
    tilt = cfg.get("brow_tilt", 0)
    for sx in (-1, 1):
        H += sphere((sx * 0.09, 0.925, 0.20), (0.065, 0.026, 0.03), beard, 8, 5,
                    rot=(0, 0, sx * tilt))
    # cheeks
    if cfg.get("blush", True):
        H += decal(hc, hr, BLUSH, [(-42, -14, 13), (42, -14, 13)])
    # beard: a wide ellipsoid around the jaw + a long point down the chest
    by = cfg.get("beard_y", 0.62)
    H += sphere((0, by, 0.07), (0.235, 0.24, 0.19), beard, 16, 10)
    H += frustum((0, by - 0.07, 0.25), (0, by - 0.42, 0.33), 0.12, 0.02,
                 beard, 12, caps=False)
    if cfg.get("moustache", True):
        for sx in (-1, 1):
            H += sphere((sx * 0.075, 0.705, 0.27), (0.085, 0.034, 0.04), beard, 8, 5,
                        rot=(0, 0, -sx * 18))
    m = cfg.get("mouth")
    if m == "open":
        H += sphere((0, 0.735, 0.25), (0.07, 0.045, 0.03), MOUTH, 10, 6)
        H += sphere((0, 0.745, 0.262), (0.05, 0.012, 0.02), WHITE, 8, 4)
    elif m == "o":
        H += sphere((0, 0.725, 0.262), 0.028, MOUTH, 8, 5)
    elif m == "smile":
        H += polyline([(-0.05, 0.705, 0.245), (-0.025, 0.69, 0.255), (0.025, 0.69, 0.255), (0.05, 0.705, 0.245)],
                      0.011, MOUTH, 6)
    for extra in cfg.get("head_extras", []):
        H += extra
    H += cfg["hat"]
    hx, hy, hz = cfg.get("head_rot", (0, 0, 0))
    nod = cfg.get("nod", (2.5, 0, 2.0))
    H = xform(H, rot=(hx + nod[0] * osc(ph, 1), hy, hz + nod[2] * osc(ph, 1, 0.25)), pivot=DWARF_NECK)
    F += H

    for extra in cfg.get("extras", []):
        F += extra
    return F


def z_letter(center, s, mat):
    """A block letter 'Z' from three thin boxes (for the sleeper)."""
    cx, cy, cz = center
    t = s * 0.24
    F = []
    F += box((cx, cy + s / 2 - t / 2, cz), (s, t, 0.02), mat)
    F += box((cx, cy - s / 2 + t / 2, cz), (s, t, 0.02), mat)
    F += box((cx, cy, cz), (s * 1.25, t, 0.02), mat, rot=(0, 0, 52))
    return F


def flower(base, top, mat_center, mat_petal, stem_mat=GREEN):
    F = tube(base, top, 0.016, stem_mat, 6)
    F += sphere(v_add(top, (0, 0.04, 0.01)), 0.046, mat_center, 8, 5)
    for k in range(6):
        a = 2 * math.pi * k / 6
        F += sphere(v_add(top, (0.072 * math.cos(a), 0.04 + 0.072 * math.sin(a), 0.0)),
                    0.04, mat_petal, 8, 5)
    # a leaf
    mid = v_mul(v_add(base, top), 0.5)
    F += sphere(v_add(mid, (0.05, 0.0, 0.0)), (0.06, 0.02, 0.03), stem_mat, 8, 4,
                rot=(0, 0, 30))
    return F


def build_princess(ph):
    F = []
    # skirt (white) with green hem stripes, gold belt
    F += frustum((0, 0.02, 0), (0, 0.95, 0), 0.42, 0.17, DRESS_WHITE, 22)
    F += frustum((0, 0.02, 0), (0, 0.15, 0), 0.43, 0.395, GREEN, 22, caps=False)
    F += frustum((0, 0.40, 0), (0, 0.47, 0), 0.326, 0.308, GREEN, 22, caps=False)
    F += frustum((0, 0.925, 0), (0, 0.985, 0), 0.19, 0.185, GOLD, 18, caps=False)
    # bodice (green), shoulders, white collar and puff sleeves
    F += frustum((0, 0.95, 0), (0, 1.30, 0), 0.17, 0.19, GREEN, 18)
    F += sphere((0, 1.30, 0), (0.25, 0.075, 0.16), GREEN, 16, 6)
    F += frustum((0, 1.30, 0), (0, 1.36, 0), 0.19, 0.145, DRESS_WHITE, 18, caps=False)
    sway = 0.02 * osc(ph, 1)
    for sx in (-1, 1):
        F += sphere((sx * 0.22, 1.29, 0.01), 0.10, DRESS_WHITE, 12, 8)
        F += arm((sx * 0.24, 1.25, 0.03), (sx * 0.055, 1.02 + sway, 0.21), PALE,
                 elbow=(sx * 0.26, 1.07, 0.11), r=0.045, hand_r=0.055, hand_mat=PALE)
    # neck and head (a little oversized so the face reads at 135 px wide)
    F += tube((0, 1.30, 0), (0, 1.48, 0), 0.065, PALE, 10)
    H = []
    hc = (0.0, 1.60, 0.0)
    hr = 0.235
    H += sphere(hc, hr, PALE, 20, 14)
    # black bob: sphere shell with the face region and under-jaw removed
    H += sphere((0, 1.61, -0.01), (0.262, 0.272, 0.262), HAIR, 20, 12,
                keep=lambda p: p[1] > -0.21 and (p[2] < 0.08 or p[1] > 0.125))
    # face
    for sx in (-1, 1):
        H += eye(hc, hr, sx * 21, 4, 0.05, lash=HAIR)
        H += brow(hc, hr, sx * 22, 22, HAIR, tilt=-sx * 8, size=(0.05, 0.012, 0.02))
    H += sphere((0, 1.555, 0.225), 0.035, PALE, 8, 5)
    H += mouth(hc, hr, "smile", LIPS, pitch=-24, half_yaw=11, r=0.013)
    H += decal(hc, hr, BLUSH, [(-40, -12, 12), (40, -12, 12)])
    # red bow on the right side of the head
    H += sphere((0.17, 1.775, 0.07), (0.075, 0.046, 0.038), RED, 8, 5, rot=(0, 0, 32))
    H += sphere((0.29, 1.74, 0.07), (0.075, 0.046, 0.038), RED, 8, 5, rot=(0, 0, -32))
    H += sphere((0.23, 1.76, 0.085), 0.034, RED, 8, 5)
    # small gold crown
    H += frustum((0, 1.84, -0.01), (0, 1.915, -0.01), 0.105, 0.115, GOLD, 12)
    for k in range(6):
        a = 2 * math.pi * k / 6
        p = (0.105 * math.cos(a), 1.915, -0.01 + 0.105 * math.sin(a))
        H += frustum(p, v_add(p, (0, 0.085, 0)), 0.03, 0.0, GOLD, 6, caps=False)
    F += xform(H, rot=(1.5 * osc(ph, 1), 0, 2.5 * osc(ph, 1, 0.25)), pivot=(0, 1.40, 0))
    return F


def build_miner(ph):
    hat_mat = Mat((142, 86, 44))
    hat = pointy_hat(hat_mat, band=Mat((96, 56, 28)))
    hat += sphere((0, 1.015, 0.225), 0.048, Mat((255, 236, 120), amb=0.7), 8, 5)
    pick = tube((0.31, 0.16, 0.12), (0.31, 1.15, 0.12), 0.028, WOOD, 8)
    pick += frustum((0.31, 1.13, 0.12), (0.31, 1.05, 0.34), 0.05, 0.0, IRON, 8)
    pick += frustum((0.31, 1.13, 0.12), (0.31, 1.06, -0.09), 0.05, 0.0, IRON, 8)
    pick = xform(pick, rot=(0, 0, -5 * osc(ph, 1)), pivot=(0.31, 0.44, 0.12))
    return build_dwarf({
        "hat": hat, "tunic": Mat((206, 160, 44)), "beard": BEARD_GREY,
        "trousers": Mat((74, 60, 50)),
        "arms": [((-0.24, DWARF_SHOULDER, 0.02), None, (-0.31, 0.27, 0.10)),
                 ((0.24, DWARF_SHOULDER, 0.02), None, (0.31, 0.44, 0.12))],
        "extras": [pick], "mouth": "smile",
    }, ph)


def build_baker(ph):
    hat = pointy_hat(Mat((246, 246, 244)), band=Mat((214, 50, 60)))
    apron = sphere((0, 0.42, 0), (0.292, 0.282, 0.262), Mat((248, 246, 238)), 20, 12,
                   keep=lambda p: p[2] > 0.13 and p[1] < 0.16 and p[1] > -0.22)
    loaf = sphere((0, 0.40, 0.33), (0.23, 0.09, 0.11), Mat((208, 142, 66)), 12, 7)
    for k in (-1, 0, 1):
        loaf += sphere((k * 0.075, 0.475, 0.33), (0.012, 0.02, 0.09), Mat((150, 90, 40)), 6, 4)
    # a curl of steam: three puffs that rise, swell and vanish (seamless)
    steam = []
    for k in range(3):
        t = (ph + k / 3.0) % 1.0
        steam += sphere((-0.06 + 0.05 * math.sin(TAU * t), 0.52 + 0.16 * t, 0.34),
                        0.004 + 0.03 * math.sin(math.pi * t), Mat((235, 235, 240), amb=0.6), 6, 4)
    return build_dwarf({
        "hat": hat, "tunic": Mat((104, 158, 222)), "beard": BEARD_GINGER,
        "trousers": Mat((60, 70, 110)),
        "body_extras": [apron], "mouth": "smile",
        "arms": [((-0.24, DWARF_SHOULDER, 0.02), (-0.34, 0.42, 0.15), (-0.25, 0.38, 0.30)),
                 ((0.24, DWARF_SHOULDER, 0.02), (0.34, 0.42, 0.15), (0.25, 0.38, 0.30))],
        "extras": [loaf, steam],
    }, ph)


def build_fiddler(ph):
    hat = pointy_hat(Mat((132, 60, 176)), band=Mat((236, 200, 70)))
    # fiddle held against the chest (below the beard), bow across it
    fmat = Mat((176, 104, 46))
    body_c = (-0.13, 0.50, 0.31)
    fiddle = sphere(body_c, (0.15, 0.10, 0.045), fmat, 12, 7, rot=(0, 0, -39))
    fiddle += sphere(body_c, (0.075, 0.115, 0.05), fmat, 10, 6, rot=(0, 0, -39))
    neck_end = (-0.34, 0.67, 0.21)
    fiddle += tube(body_c, neck_end, 0.022, Mat((60, 40, 30)), 6)
    fiddle += sphere(neck_end, 0.036, fmat, 8, 5)
    s = 0.045 * osc(ph, 2)             # the bow strokes twice per turn
    bow_dir = v_norm((-0.32, -0.26, 0.01))
    b0, b1 = v_add((0.10, 0.62, 0.38), v_mul(bow_dir, s)), v_add((-0.22, 0.36, 0.39), v_mul(bow_dir, s))
    fiddle += tube(b0, b1, 0.013, Mat((236, 224, 200)), 6)
    hand_r = v_add((0.10, 0.60, 0.36), v_mul(bow_dir, s))
    return build_dwarf({
        "hat": hat, "tunic": Mat((52, 64, 140)), "beard": BEARD_WHITE,
        "trousers": Mat((80, 60, 90)),
        "head_rot": (4, 0, 10), "mouth": "smile",
        "arms": [((-0.24, DWARF_SHOULDER, 0.02), (-0.36, 0.46, 0.10), (-0.31, 0.64, 0.20)),
                 ((0.24, DWARF_SHOULDER, 0.02), (0.35, 0.42, 0.18), hand_r)],
        "extras": [fiddle],
    }, ph)


def build_snoozer(ph):
    cap_mat = Mat((56, 104, 226))
    hat = pointy_hat(cap_mat, mid=((0.10, 1.25, 0.02), 0.09), tip=(0.30, 1.12, 0.02),
                     brim=False, band=Mat((240, 240, 250)))
    hat += frustum((0, 0.925, 0.02), (0, 0.985, 0.02), 0.25, 0.24, Mat((240, 240, 250)), 18)
    hat += sphere((0.30, 1.12, 0.02), 0.06, Mat((240, 240, 250)), 8, 5)
    pillow = box((0, 0.44, 0.30), (0.34, 0.23, 0.09), Mat((238, 236, 250)))
    zmat = Mat((176, 206, 255), amb=0.75)
    # two Zs drift up and to the side, growing in and fading out (seamless)
    zees = []
    for k, (x, y, dy, s) in enumerate(((-0.25, 0.96, 0.14, 0.10), (-0.31, 1.06, 0.18, 0.12))):
        t = (ph + 0.5 * k) % 1.0
        sz = s * math.sin(math.pi * t)
        if sz > 0.012:
            zees += z_letter((x - 0.05 * t, y + dy * t, 0.24 - 0.10 * k), sz, zmat)
    return build_dwarf({
        "hat": hat, "tunic": Mat((190, 170, 232)), "beard": BEARD_WHITE,
        "trousers": Mat((150, 130, 190)),
        "eyes": "closed", "moustache": False, "mouth": "o", "brow_tilt": -8,
        "head_rot": (6, 0, -6), "nod": (4.0, 0, 1.0),
        "arms": [((-0.24, DWARF_SHOULDER, 0.02), (-0.32, 0.40, 0.16), (-0.09, 0.33, 0.36)),
                 ((0.24, DWARF_SHOULDER, 0.02), (0.32, 0.40, 0.16), (0.09, 0.33, 0.36))],
        "extras": [pillow, zees],
    }, ph)


def build_grouch(ph):
    hat = pointy_hat(Mat((128, 130, 142)), band=Mat((60, 60, 70)), tip=(0.0, 1.32, 0.02))
    return build_dwarf({
        "hat": hat, "tunic": Mat((170, 46, 50)), "beard": BEARD_GREY,
        "trousers": Mat((56, 50, 60)), "nose": NOSE_RED,
        "brow_tilt": 22, "head_rot": (8, 0, 0), "nod": (1.0, 0, 3.5), "blush": False,
        "arms": [((-0.24, DWARF_SHOULDER, 0.02), (-0.31, 0.40, 0.22), (0.15, 0.43, 0.29)),
                 ((0.24, DWARF_SHOULDER, 0.02), (0.31, 0.40, 0.26), (-0.15, 0.45, 0.37))],
    }, ph)


def build_shy(ph):
    hat = pointy_hat(Mat((240, 130, 182)), base=(0, 0.905, 0.02), tip=(0.06, 1.31, 0.02),
                     band=Mat((250, 200, 220)))
    blush = decal(DWARF_HEAD, DWARF_HEAD_R, Mat((240, 130, 140)), [(-42, -12, 17), (42, -12, 17)])
    # a blue flower held in front of the chest, swaying in the hand
    fl = flower((0.20, 0.38, 0.27), (0.20, 0.64, 0.29), Mat((250, 210, 60), amb=0.5),
                Mat((120, 160, 255), amb=0.5))
    fl = xform(fl, rot=(0, 0, 7 * osc(ph, 1)), pivot=(0.20, 0.40, 0.27))
    return build_dwarf({
        "hat": hat, "tunic": Mat((60, 170, 160)), "beard": BEARD_WHITE,
        "trousers": Mat((60, 90, 100)),
        "head_rot": (9, 0, 7), "brow_tilt": -10, "blush": False, "mouth": "smile",
        "head_extras": [blush],
        "arms": [((-0.24, DWARF_SHOULDER, 0.02), None, (-0.29, 0.27, 0.06)),
                 ((0.24, DWARF_SHOULDER, 0.02), (0.34, 0.36, 0.14), (0.20, 0.40, 0.27))],
        "extras": [fl],
    }, ph)


def build_giggler(ph):
    hat = pointy_hat(Mat((246, 132, 30)), band=Mat((250, 220, 60)), tip=(-0.05, 1.33, 0.06))
    w = 0.06 * osc(ph, 2)              # hands wave up and down, alternating
    return build_dwarf({
        "hat": hat, "tunic": Mat((84, 172, 92)), "beard": BEARD_GINGER,
        "trousers": Mat((60, 80, 60)),
        "beard_y": 0.585, "moustache": False, "mouth": "open", "brow_tilt": -14,
        "head_rot": (-12, 0, 0), "ears": False, "nod": (3.0, 0, 3.0),
        "arms": [((-0.24, DWARF_SHOULDER, 0.02), (-0.40, 0.72, 0.06), (-0.35, 0.98 + w, 0.10)),
                 ((0.24, DWARF_SHOULDER, 0.02), (0.40, 0.72, 0.06), (0.35, 0.98 - w, 0.10))],
    }, ph)


DWARVES = {
    "title": "Snow White & the dwarves",
    "blurb": "a princess and seven bearded dwarves from the public-domain Grimm tale \"Snow White\"",
    "characters": [
        ("princess", "Princess", build_princess, 0.80),
        ("miner", "Miner", build_miner, 0.80),
        ("baker", "Baker", build_baker, 0.80),
        ("fiddler", "Fiddler", build_fiddler, 0.80),
        ("snoozer", "Snoozer", build_snoozer, 0.80),
        ("grouch", "Grouch", build_grouch, 0.80),
        ("shy", "Shy", build_shy, 0.80),
        ("giggler", "Giggler", build_giggler, 0.80),
    ],
}


# ==========================================================================
# SET 2: island -- a Hawaiian island family and their alien friends
# ==========================================================================

ISLAND_SKIN = Mat((222, 172, 128))
DENIM = Mat((58, 78, 138))
TEAL = Mat((40, 170, 170))
BLUE_FUR = Mat((66, 108, 222), amb=0.36)
BLUE_BELLY = Mat((150, 190, 244), amb=0.4)
PINK_FUR = Mat((242, 122, 182), amb=0.36)
PINK_BELLY = Mat((252, 200, 226), amb=0.45)
PURPLE = Mat((124, 72, 176), amb=0.36)
PURPLE_LIGHT = Mat((178, 132, 220), amb=0.4)
LIME = Mat((104, 192, 92))
SUN_YELLOW = Mat((250, 212, 62), amb=0.4)
SAND = Mat((236, 224, 196))
LENS = Mat((22, 24, 34), amb=0.35)
GLASS = Mat((150, 220, 232), amb=0.6)
SUIT = Mat((60, 60, 74), amb=0.55)          # charcoal, not black: must separate from the background
LAPEL = Mat((84, 84, 100), amb=0.55)
SHOE_DARK = Mat((44, 44, 54), amb=0.5)
STRAW = Mat((216, 172, 92))


def dress_dots(y0, r0, y1, r1, mat, spots, size=(0.03, 0.03, 0.012)):
    """Flat spots stuck on the surface of a vertical frustum (dress prints)."""
    F = []
    for a, t in spots:
        y = y0 + (y1 - y0) * t
        r = r0 + (r1 - r0) * t
        F += stick(sphere((0, 0, 0), size, mat, 8, 5), (0, y, 0), r, a, 0, 0.004)
    return F


def hibiscus(c, petal, core, r=0.045, yaw=0, pitch=0):
    """A flat five-petal flower facing (yaw, pitch)."""
    F = []
    for k in range(5):
        a = TAU * k / 5
        F += sphere((r * math.cos(a), r * math.sin(a), 0), (r * 0.7, r * 0.55, 0.012), petal, 8, 5,
                    rot=(0, 0, math.degrees(a)))
    F += sphere((0, 0, 0.006), r * 0.45, core, 8, 5)
    return xform(F, rot=(-pitch, yaw, 0), offset=c)


def build_keiki(ph):
    """Little island girl: black hair, red flower-print dress, waving."""
    skin, red = ISLAND_SKIN, Mat((214, 40, 52), amb=0.4)
    wave = 0.04 * osc(ph, 2)
    hc, hr = (0.0, 1.30, 0.0), 0.26
    F = biped({
        "skin": skin, "hip_y": 0.58, "shoulder_y": 1.03, "hip_r": 0.19, "shoulder_r": 0.165,
        "leg_dx": 0.09, "leg_r": 0.062, "leg_mat": skin, "shoe": Mat((150, 96, 52)),
        "shoe_size": (0.12, 0.05, 0.18), "torso": red, "shoulder_mat": skin, "shoulder_h": 0.05,
        "neck_r": 0.055, "head_c": hc, "head_r": hr,
        "arms": [((-0.19, 1.00, 0.0), (-0.25, 0.80, 0.05), (-0.22, 0.62, 0.11), skin, 0.045, skin),
                 ((0.19, 1.00, 0.0), (0.31, 1.10, 0.03), (0.30, 1.32 + wave, 0.06), skin, 0.045, skin)],
    })
    # A-line skirt + white flower print
    F += frustum((0, 0.44, 0), (0, 0.86, 0), 0.31, 0.195, red, 20)
    F += dress_dots(0.44, 0.31, 0.86, 0.195, WHITE,
                    [(20, 0.25), (95, 0.6), (160, 0.3), (230, 0.65), (300, 0.2), (340, 0.75), (60, 0.85), (200, 0.9)])
    F += dress_dots(0.86, 0.19, 1.03, 0.165, WHITE, [(30, 0.5), (150, 0.4), (270, 0.6)])
    # long black hair: back curtain + cap, and a pink hibiscus behind the ear
    H = sphere((0, 1.12, -0.05), (0.31, 0.36, 0.27), INK, 18, 12,
               keep=lambda p: p[2] < 0.02 and p[1] < 0.55)
    H += hair_cap(hc, hr, INK, bottom=-0.30, front=0.30, fringe=0.42)
    H += hibiscus(on_sphere(hc, hr, 62, 26, 0.03), Mat((250, 120, 150)), SUN_YELLOW, 0.05, 62, 26)
    # face: big eyes, little nose, a wide happy smile, rosy cheeks
    for sx in (-1, 1):
        H += eye(hc, hr, sx * 20, 4, 0.05, pr=0.031, lash=INK)
        H += brow(hc, hr, sx * 20, 21, INK, tilt=-sx * 6, size=(0.045, 0.011, 0.02))
    H += nose(hc, hr, 0.026, skin, -8)
    H += mouth(hc, hr, "wide", pitch=-24, half_yaw=12)
    H += decal(hc, hr, BLUSH, [(-40, -12, 12), (40, -12, 12)])
    F += xform(H, rot=(0, 0, 3 * osc(ph, 1)), pivot=(0, 1.06, 0))
    return F


def furball(cfg, ph):
    """Small round furry alien: egg body, wide head, big ears, stubby limbs."""
    fur, belly = cfg["fur"], cfg["belly"]
    F = []
    bc, br = (0.0, 0.52, 0.0), (0.40, 0.42, 0.36)
    F += sphere(bc, br, fur, 20, 13)
    F += decal(bc, br, belly, [(0, -12, 34)], lift=0.005)
    hc, hr = (0.0, 1.06, 0.02), (0.50, 0.42, 0.44)
    F += sphere(hc, hr, fur, 22, 14)
    # ears
    es = cfg.get("ear_size", (0.11, 0.28, 0.05))
    for sx in (-1, 1):
        wig = sx * (cfg.get("ear_tilt", 35) + 4 * osc(ph, 1))
        E = sphere((0, es[1] * 0.8, 0), es, fur, 10, 7)
        E += sphere((0, es[1] * 0.8, es[2] * 0.7), (es[0] * 0.55, es[1] * 0.65, es[2] * 0.5), belly, 8, 5)
        F += xform(E, rot=(0, 0, -wig), offset=(sx * 0.47, 1.22, -0.02))
    # a tuft on top
    for k, (dx, dz) in enumerate(((-0.06, 0.0), (0.02, 0.05), (0.07, -0.04))):
        F += frustum((dx, 1.44, dz), (dx * 1.6, 1.58 + 0.03 * k, dz * 1.4), 0.035, 0.0, fur, 6)
    # limbs
    for sx in (-1, 1):
        F += arm((sx * 0.36, 0.70, 0.05), (sx * 0.50, 0.44 + 0.02 * sx * osc(ph, 1), 0.16), fur,
                 elbow=None, r=0.07, hand_r=0.085, hand_mat=fur)
        F += tube((sx * 0.17, 0.28, 0.0), (sx * 0.17, 0.06, 0.02), 0.08, fur, 10)
        F += sphere((sx * 0.17, 0.06, 0.07), (0.11, 0.06, 0.16), fur, 10, 6)
    F += cfg["face"](hc, hr)
    for extra in cfg.get("extras", []):
        F += extra
    return F


def build_blue(ph):
    """Small round blue furry alien: big ears, big dark eyes, a wide grin."""
    def face(hc, hr):
        H = []
        for sx in (-1, 1):
            H += eye(hc, hr, sx * 25, 4, 0.115, style="dark")
        H += stick(sphere((0, 0, 0), (0.06, 0.04, 0.03), BLACK, 8, 5), hc, hr, 0, -13, 0.0)
        H += mouth(hc, hr, "grin", pitch=-31, half_yaw=36, out=0.004)
        return H
    return furball({"fur": BLUE_FUR, "belly": BLUE_BELLY, "face": face}, ph)


def build_pink(ph):
    """Pink furry alien with antennae, long lashes and a sweet smile."""
    def face(hc, hr):
        H = []
        for sx in (-1, 1):
            H += eye(hc, hr, sx * 24, 4, 0.10, pr=0.062, lash=BLACK)
        H += stick(sphere((0, 0, 0), (0.05, 0.035, 0.03), Mat((200, 60, 120), amb=0.4), 8, 5), hc, hr, 0, -13, 0.0)
        H += mouth(hc, hr, "smile", pitch=-30, half_yaw=22, r=0.016)
        H += decal(hc, hr, Mat((252, 170, 200)), [(-48, -16, 12), (48, -16, 12)])
        return H
    ant = []
    for sx in (-1, 1):
        A = polyline([(0, 0, 0), (sx * 0.06, 0.18, -0.02), (sx * 0.12, 0.34, -0.03)], 0.022, PINK_FUR, 6)
        A += sphere((sx * 0.12, 0.36, -0.03), 0.055, PINK_BELLY, 10, 7)
        ant += xform(A, rot=(0, 0, -sx * 8 * osc(ph, 1, 0.25)), offset=(sx * 0.16, 1.44, -0.03))
    return furball({"fur": PINK_FUR, "belly": PINK_BELLY, "face": face, "extras": [ant],
                    "ear_size": (0.09, 0.19, 0.05), "ear_tilt": 30}, ph)


def build_sister(ph):
    """Older sister: dark ponytail, teal top, jeans, hand on hip."""
    skin = ISLAND_SKIN
    hc, hr = (0.0, 1.66, 0.0), 0.22
    F = biped({
        "skin": skin, "hip_y": 0.90, "shoulder_y": 1.40, "hip_r": 0.20, "shoulder_r": 0.19,
        "leg_dx": 0.10, "leg_r": 0.075, "leg_mat": DENIM, "shoe": WHITE, "shoe_size": (0.13, 0.07, 0.21),
        "torso": TEAL, "shoulder_mat": TEAL, "neck_r": 0.06, "head_c": hc, "head_r": hr,
        "arms": [((-0.22, 1.36, 0.0), (-0.36, 1.12, 0.02), (-0.21, 0.94, 0.09), skin, 0.05, skin),
                 ((0.22, 1.36, 0.0), (0.28, 1.10, 0.04), (0.27, 0.86 + 0.015 * osc(ph, 1), 0.10), skin, 0.05, skin)],
    })
    F += frustum((0, 0.86, 0), (0, 0.94, 0), 0.215, 0.205, Mat((40, 36, 48), amb=0.5), 18, caps=False)  # belt
    H = hair_cap(hc, hr, INK, bottom=-0.20, front=0.32, fringe=0.48)
    tail = beads([(0, 0.02, -0.02), (0.02, -0.22, -0.10), (0.05, -0.55, -0.08)], 0.07, INK, 6, taper=0.45)
    tail += sphere((0, 0.02, -0.02), 0.05, RED, 8, 5)
    H += xform(tail, rot=(0, 0, 5 * osc(ph, 1)), offset=(0, 1.76, -0.20))
    for sx in (-1, 1):
        H += eye(hc, hr, sx * 20, 5, 0.043, lash=INK)
        H += brow(hc, hr, sx * 20, 22, INK, tilt=-sx * 4, size=(0.048, 0.011, 0.02))
    H += nose(hc, hr, 0.028, skin, -7)
    H += mouth(hc, hr, "smile", LIPS, pitch=-25, half_yaw=11)
    H += decal(hc, hr, Mat((238, 160, 140)), [(-40, -12, 10), (40, -12, 10)])
    F += xform(H, rot=(0, 0, 2.5 * osc(ph, 1, 0.25)), pivot=(0, 1.45, 0))
    return F


def build_surfer(ph):
    """Tanned young man in red shorts, surfboard under one arm, shaka with the other."""
    skin, hair = TAN, Mat((214, 172, 92))
    hc, hr = (0.0, 1.82, 0.0), 0.22
    sh = 0.02 * osc(ph, 1)
    F = biped({
        "skin": skin, "hip_y": 1.0, "shoulder_y": 1.48, "hip_r": 0.21, "shoulder_r": 0.25, "shoulder_h": 0.08,
        "leg_dx": 0.11, "leg_r": 0.08, "leg_mat": skin, "shoe": skin, "shoe_size": (0.13, 0.05, 0.22),
        "torso": skin, "neck_r": 0.07, "head_c": hc, "head_r": hr,
        "arms": [((-0.27, 1.44, 0.0), (-0.44, 1.22, 0.10), (-0.42, 1.04, 0.16), skin, 0.06, skin),
                 ((0.27, 1.44, 0.0), (0.42, 1.22, 0.04), (0.46, 1.46 + sh, 0.12), skin, 0.06, skin)],
    })
    F += frustum((0, 0.72, 0), (0, 1.02, 0), 0.245, 0.225, RED, 20)                     # shorts
    F += frustum((0, 0.72, 0), (0, 0.76, 0), 0.25, 0.248, WHITE, 20, caps=False)         # hem stripe
    # surfboard standing at his left side, slightly tilted
    B = sphere((0, 0, 0), (0.17, 0.86, 0.045), SAND, 14, 12)
    B += box((0, 0, 0.046), (0.05, 1.45, 0.008), Mat((40, 140, 210)))
    B += box((0, -0.86, -0.04), (0.02, 0.1, 0.06), Mat((40, 140, 210)))               # fin
    F += xform(B, rot=(0, 10, 7), offset=(-0.46, 0.88, 0.0))
    # puka-shell necklace
    for k in range(9):
        a = TAU * (k + 0.5) / 18 + math.pi * 0.5
        F += sphere((0.13 * math.cos(a), 1.56 - 0.02 * math.sin(a), 0.12 * math.sin(a) + 0.02), 0.02, WHITE, 6, 4)
    H = hair_cap(hc, hr, hair, bottom=0.02, front=0.42, fringe=0.55, grow=0.04)
    for k in range(4):                                   # messy tufts
        H += frustum(on_sphere(hc, hr, -60 + 40 * k, 60, 0.0), on_sphere(hc, hr, -70 + 45 * k, 72, 0.10), 0.04, 0.0, hair, 6)
    for sx in (-1, 1):
        H += eye(hc, hr, sx * 20, 5, 0.043)
        H += brow(hc, hr, sx * 20, 22, hair, tilt=-sx * 5, size=(0.05, 0.012, 0.02))
    H += nose(hc, hr, 0.03, Mat((190, 130, 90)), -7)
    H += mouth(hc, hr, "wide", pitch=-25, half_yaw=13)
    F += xform(H, rot=(0, 0, 3 * osc(ph, 1, 0.25)), pivot=(0, 1.60, 0))
    return F


def build_professor(ph):
    """Large rotund purple alien scientist: four arms, lab coat, goggles, flask."""
    F = []
    bc, br = (0.0, 0.72, 0.0), (0.52, 0.58, 0.46)
    F += sphere(bc, br, PURPLE, 22, 14)
    F += decal(bc, br, PURPLE_LIGHT, [(0, -8, 30)], lift=0.005)
    coat = (0.56, 0.61, 0.50)
    F += sphere(bc, coat, WHITE, 22, 14,
                keep=lambda p: p[2] / coat[2] < 0.42 and -0.80 < p[1] / coat[1] < 0.72
                or (p[1] / coat[1] > 0.45 and abs(p[0]) / coat[0] > 0.45 and p[1] / coat[1] < 0.72))
    for sx in (-1, 1):                                           # collar flaps
        F += box((sx * 0.19, 1.16, 0.40), (0.14, 0.18, 0.02), WHITE, rot=(-20, 0, sx * 25))
    hc, hr = (0.0, 1.44, 0.05), 0.30
    F += sphere(hc, hr, PURPLE, 20, 14)
    # goggles pushed up on the forehead + strap
    for sx in (-1, 1):
        G = frustum((0, 0, -0.02), (0, 0, 0.05), 0.085, 0.085, GOLD, 14)
        G += frustum((0, 0, 0.05), (0, 0, 0.062), 0.07, 0.07, LENS, 14)
        F += stick(G, hc, hr, sx * 17, 40, -0.01)
    sy = hc[1] + hr * math.sin(math.radians(40))
    F += frustum((0, sy - 0.02, hc[2]), (0, sy + 0.02, hc[2]), hr * math.cos(math.radians(40)) + 0.012,
                 hr * math.cos(math.radians(40)) + 0.012, INK, 20, caps=False)
    # face: small clever eyes, one brow cocked, a knowing smile, no nose
    for sx in (-1, 1):
        F += eye(hc, hr, sx * 17, 4, 0.05, pr=0.028, look=(0, 0))
        F += brow(hc, hr, sx * 18, 18, Mat((70, 36, 110), amb=0.5), tilt=(10 if sx > 0 else -2) * sx,
                  size=(0.06, 0.016, 0.022))
    F += mouth(hc, hr, "smug", pitch=-24, half_yaw=12)
    F += decal(hc, hr, PURPLE_LIGHT, [(-44, -14, 11), (44, -14, 11)])
    # arms: an upper pair in white sleeves, a lower pair in purple
    flask_hand = (0.55, 0.84, 0.30)
    F += arm((-0.48, 1.02, 0.05), (-0.52, 0.72, 0.32), WHITE, elbow=(-0.66, 0.88, 0.14), r=0.07, hand_r=0.08, hand_mat=PURPLE)
    F += arm((0.48, 1.02, 0.05), flask_hand, WHITE, elbow=(0.66, 0.90, 0.12), r=0.07, hand_r=0.08, hand_mat=PURPLE)
    F += arm((-0.52, 0.66, 0.05), (-0.56, 0.36, 0.30), PURPLE, elbow=(-0.70, 0.50, 0.12), r=0.06, hand_r=0.07, hand_mat=PURPLE)
    F += arm((0.52, 0.66, 0.05), (0.58, 0.38, 0.30), PURPLE, elbow=(0.70, 0.50, 0.12), r=0.06, hand_r=0.07, hand_mat=PURPLE)
    # a flask with a bubble rising (one cycle per turn)
    fb = v_add(flask_hand, (0, 0.05, 0))
    F += frustum(fb, v_add(fb, (0, 0.17, 0)), 0.075, 0.03, GLASS, 12)
    F += frustum(v_add(fb, (0, 0.17, 0)), v_add(fb, (0, 0.23, 0)), 0.03, 0.03, GLASS, 10)
    F += frustum(v_add(fb, (0, 0.005, 0)), v_add(fb, (0, 0.07, 0)), 0.07, 0.052, LIME, 12, caps=False)
    F += sphere(v_add(fb, (0.02 * osc(ph, 1), 0.05 + 0.16 * ph, 0.0)), 0.004 + 0.018 * math.sin(math.pi * ph), WHITE, 6, 4)
    # legs
    for sx in (-1, 1):
        F += tube((sx * 0.20, 0.30, 0.0), (sx * 0.20, 0.06, 0.02), 0.09, PURPLE, 10)
        F += sphere((sx * 0.20, 0.06, 0.08), (0.13, 0.06, 0.18), PURPLE, 10, 6)
    return F


def build_noodle(ph):
    """Tall thin green one-eyed alien in a yellow polka-dot sundress."""
    green = LIME
    hc, hr = (0.0, 2.08, 0.0), (0.19, 0.24, 0.18)
    F = biped({
        "skin": green, "hip_y": 0.86, "shoulder_y": 1.62, "hip_r": 0.13, "shoulder_r": 0.115, "shoulder_h": 0.04,
        "leg_dx": 0.08, "leg_r": 0.045, "leg_mat": green, "shoe": None,
        "torso": SUN_YELLOW, "shoulder_mat": green, "neck_r": 0.045, "head_c": hc, "head_r": hr,
        "head_slices": 18, "head_stacks": 12,
        "arms": [((-0.15, 1.58, 0.0), (-0.27, 1.30, 0.04), (-0.23, 1.02 + 0.02 * osc(ph, 1), 0.12), green, 0.035, green),
                 ((0.15, 1.58, 0.0), (0.27, 1.30, 0.04), (0.23, 1.02 - 0.02 * osc(ph, 1), 0.12), green, 0.035, green)],
    })
    for sx in (-1, 1):                                           # big flat feet
        F += sphere((sx * 0.08, 0.04, 0.05), (0.075, 0.04, 0.15), green, 10, 6)
    F += frustum((0, 0.70, 0), (0, 1.56, 0), 0.34, 0.125, SUN_YELLOW, 20)
    F += dress_dots(0.70, 0.34, 1.56, 0.125, WHITE,
                    [(15, 0.15), (80, 0.45), (140, 0.2), (200, 0.6), (260, 0.3), (320, 0.7), (50, 0.8), (230, 0.9), (110, 0.05), (290, 0.05)],
                    size=(0.026, 0.026, 0.01))
    for sx in (-1, 1):
        F += tube((sx * 0.10, 1.56, 0.07), (sx * 0.10, 1.66, -0.02), 0.012, SUN_YELLOW, 6)
    # the one big eye, a raised brow, a shy smile, antennae
    F += eye(hc, hr, 0, 8, 0.12, pr=0.066, pupil=Mat((60, 120, 200)))
    F += sphere(on_sphere(on_sphere(hc, hr, 0, 8, -0.12 * 0.45), 0.12, 0, 2, -0.02), 0.04, BLACK, 8, 5)
    F += brow(hc, hr, 0, 44, Mat((60, 130, 60)), tilt=-6, size=(0.09, 0.014, 0.02))
    F += mouth(hc, hr, "smile", pitch=-36, half_yaw=12, r=0.011)
    F += decal(hc, hr, Mat((170, 220, 120)), [(-40, -20, 12), (40, -20, 12)])
    for sx in (-1, 1):
        A = polyline([(0, 0, 0), (sx * 0.04, 0.10, 0.0), (sx * 0.09, 0.18, 0.0)], 0.014, green, 6)
        A += sphere((sx * 0.09, 0.19, 0.0), 0.03, SUN_YELLOW, 8, 5)
        F += xform(A, rot=(0, 0, -sx * 9 * osc(ph, 1)), offset=(sx * 0.07, 2.30, 0.0))
    return F


def build_agent(ph):
    """A very large man in a black suit: sunglasses, gold earring, no nonsense."""
    skin = Mat((200, 150, 110))
    hc, hr = (0.0, 1.82, 0.0), 0.25
    F = biped({
        "skin": skin, "hip_y": 0.86, "shoulder_y": 1.50, "hip_r": 0.36, "shoulder_r": 0.42, "shoulder_h": 0.10,
        "leg_dx": 0.16, "leg_r": 0.12, "leg_mat": SUIT, "shoe": SHOE_DARK, "shoe_size": (0.18, 0.09, 0.27),
        "torso": SUIT, "torso_n": 22, "neck_r": 0.10, "head_c": hc, "head_r": hr,
        "arms": [((-0.42, 1.44, 0.0), (-0.58, 1.14, 0.02), (-0.54, 0.86 + 0.012 * osc(ph, 1), 0.10), SUIT, 0.085, skin),
                 ((0.42, 1.44, 0.0), (0.58, 1.14, 0.02), (0.54, 0.86 - 0.012 * osc(ph, 1), 0.10), SUIT, 0.085, skin)],
    })
    # shirt, tie and lapels laid on the chest
    F += box((0, 1.27, 0.385), (0.17, 0.40, 0.02), WHITE, rot=(-5, 0, 0))
    F += box((0, 1.22, 0.405), (0.055, 0.32, 0.012), Mat((150, 30, 40), amb=0.45), rot=(-5, 0, 0))
    F += sphere((0, 1.42, 0.41), (0.04, 0.03, 0.02), Mat((150, 30, 40), amb=0.45), 8, 5)
    for sx in (-1, 1):
        F += box((sx * 0.135, 1.28, 0.395), (0.08, 0.38, 0.012), LAPEL, rot=(-5, 0, sx * 9))
    F += hair_cap(hc, hr, INK, bottom=0.10, front=0.55, fringe=0.75, grow=0.012)
    # sunglasses: two lenses, a bridge and arms back to the ears
    for sx in (-1, 1):
        F += stick(sphere((0, 0, 0), (0.08, 0.05, 0.02), LENS, 10, 6), hc, hr, sx * 22, 6, 0.012)
        F += tube(on_sphere(hc, hr, sx * 45, 8, 0.02), on_sphere(hc, hr, sx * 88, 4, 0.01), 0.008, BLACK, 5)
    F += tube(on_sphere(hc, hr, -6, 8, 0.02), on_sphere(hc, hr, 6, 8, 0.02), 0.008, BLACK, 5)
    F += ears(hc, hr, skin, (0.045, 0.06, 0.04))
    F += sphere(on_sphere(hc, hr, -92, -18, 0.0), 0.024, GOLD, 8, 5)                 # earring
    for sx in (-1, 1):
        F += brow(hc, hr, sx * 22, 20, INK, tilt=sx * 8, size=(0.07, 0.016, 0.022), out=0.02)
    F += nose(hc, hr, 0.04, Mat((190, 138, 100)), -8)
    F += mouth(hc, hr, "line", pitch=-27, half_yaw=9, r=0.012)
    F += stick(sphere((0, 0, 0), (0.05, 0.035, 0.02), Mat((186, 136, 98)), 8, 5), hc, hr, 0, -42, 0.0)  # chin
    return F


ISLAND = {
    "title": "Island family & friends",
    "blurb": "a Hawaiian island family and their alien friends",
    "characters": [
        ("keiki", "Keiki", build_keiki, 0.72),
        ("blue", "Blue", build_blue, 0.70),
        ("sister", "Sister", build_sister, 0.82),
        ("surfer", "Surfer", build_surfer, 0.84),
        ("professor", "Professor", build_professor, 0.84),
        ("noodle", "Noodle", build_noodle, 0.88),
        ("pink", "Pink", build_pink, 0.70),
        ("agent", "Agent", build_agent, 0.88),
    ],
}


# ==========================================================================
# SET 3: winter -- a snowy kingdom
# ==========================================================================

ICE_BLUE = Mat((132, 196, 242), amb=0.36)
ICE_GOWN = Mat((92, 168, 232), amb=0.34)
CAPE_ICE = Mat((208, 234, 252), amb=0.60)      # pale + luminous: reads translucent
PLATINUM = Mat((242, 238, 220), amb=0.36)
PLATINUM_DK = Mat((196, 180, 150))
AUBURN = Mat((164, 76, 36), amb=0.4)
MAGENTA = Mat((196, 42, 124), amb=0.4)
TEAL_DRESS = Mat((30, 124, 134))
TEAL_BODICE = Mat((22, 82, 94), amb=0.45)
NAVY = Mat((38, 52, 104), amb=0.45)
CREAM = Mat((242, 230, 202))
SNOW = Mat((248, 250, 255), amb=0.36)
SNOW_SHADE = Mat((206, 222, 240))
CARROT = Mat((240, 130, 30), amb=0.4)
TWIG = Mat((112, 72, 40), amb=0.4)
FUR_TRIM = Mat((238, 234, 226))
TUNIC = Mat((64, 74, 114), amb=0.4)
BROWN = Mat((134, 88, 50))
BROWN_LIGHT = Mat((196, 154, 104))
ANTLER = Mat((214, 178, 124))
MOSS = Mat((92, 146, 62))
ROCK = Mat((128, 130, 118))
ROCK_DARK = Mat((96, 98, 90))
CRYSTAL_PINK = Mat((236, 130, 226), amb=0.55)
CRYSTAL_VIOLET = Mat((160, 116, 244), amb=0.55)
ICE_SPIKE = Mat((196, 228, 252), amb=0.5)
GLOW_EYE = Mat((126, 204, 255), amb=0.7)


def cape(center, radii, mat, top=0.85, slices=20, stacks=12):
    """A cloak hanging from the shoulders: the back half of an ellipsoid."""
    rx, ry, rz = radii
    return sphere(center, radii, mat, slices, stacks,
                  keep=lambda p: p[2] / rz < -0.03 and -0.97 < p[1] / ry < top)


def snowflake(c, r, mat, spin=0.0):
    F = []
    for k in range(3):
        a = math.pi * k / 3
        d = (r * math.cos(a), r * math.sin(a), 0)
        F += tube(v_sub((0, 0, 0), d), d, 0.012, mat, 5)
        for s in (-1, 1):
            tip = v_mul(d, 0.65 * s)
            for side in (-1, 1):
                b = math.pi * k / 3 + side * 0.6
                F += tube(tip, v_add(tip, (0.3 * r * math.cos(b) * s, 0.3 * r * math.sin(b) * s, 0)), 0.01, mat, 5)
    return xform(F, rot=(0, spin, 0), offset=c)


def build_icequeen(ph):
    """Ice queen: platinum braid over one shoulder, ice-blue gown, a pale
    cape, and a snowflake spinning above her open hand."""
    hc, hr = (0.0, 1.72, 0.0), 0.22
    F = biped({
        "skin": PALE, "legs": False, "hip_y": 1.0, "shoulder_y": 1.40, "hip_r": 0.17, "shoulder_r": 0.185,
        "shoulder_h": 0.06, "torso": ICE_BLUE, "shoulder_mat": PALE, "neck_r": 0.055, "head_c": hc, "head_r": hr,
        "arms": [((-0.21, 1.36, 0.0), (-0.37, 1.14, 0.08), (-0.42, 0.98, 0.24), ICE_BLUE, 0.045, PALE),
                 ((0.21, 1.36, 0.0), (0.26, 1.12, 0.04), (0.20, 0.92, 0.14), ICE_BLUE, 0.045, PALE)],
    })
    F += frustum((0, 0.02, 0), (0, 1.02, 0), 0.42, 0.17, ICE_GOWN, 22)
    F += frustum((0, 0.02, 0), (0, 0.12, 0), 0.43, 0.40, ICE_BLUE, 22, caps=False)
    F += dress_dots(0.02, 0.42, 1.02, 0.17, WHITE, [(30, 0.3), (110, 0.55), (200, 0.4), (290, 0.6), (60, 0.75), (250, 0.8), (150, 0.15)],
                    size=(0.018, 0.018, 0.008))
    F += cape((0, 0.76, -0.06), (0.50, 0.72, 0.26), CAPE_ICE)
    F += snowflake((-0.42, 1.14 + 0.03 * osc(ph, 2), 0.24), 0.11, CAPE_ICE, spin=360.0 * ph)
    H = hair_cap(hc, hr, PLATINUM, bottom=-0.02, front=0.36, fringe=0.52)
    H += sphere(on_sphere(hc, hr, 25, 42, 0.0), (0.16, 0.05, 0.10), PLATINUM, 10, 6, rot=(0, 0, -20))  # swept fringe
    H += beads([(-0.15, 1.82, -0.12), (-0.27, 1.56, -0.02), (-0.25, 1.30, 0.16), (-0.22, 1.02, 0.22)],
               0.068, PLATINUM, 8, taper=0.35)
    H += frustum(on_sphere(hc, hr, 0, 62, 0.03), on_sphere(hc, hr, 0, 70, 0.16), 0.03, 0.0, ICE_SPIKE, 6)  # tiara
    for sx in (-1, 1):
        H += eye(hc, hr, sx * 20, 5, 0.045, pupil=Mat((60, 110, 190)), lash=PLATINUM_DK)
        H += brow(hc, hr, sx * 20, 22, PLATINUM_DK, tilt=-sx * 3, size=(0.048, 0.010, 0.02))
    H += nose(hc, hr, 0.026, PALE, -7)
    H += mouth(hc, hr, "smile", LIPS, pitch=-25, half_yaw=9, r=0.011)
    H += decal(hc, hr, Mat((244, 190, 200)), [(-40, -12, 10), (40, -12, 10)])
    F += xform(H, rot=(0, 0, 2 * osc(ph, 1, 0.25)), pivot=(0, 1.50, 0))
    return F


def build_sunny(ph):
    """Sunny: auburn braids, freckles, magenta cape over a teal dress, waving."""
    hc, hr = (0.0, 1.64, 0.0), 0.22
    wave = 0.04 * osc(ph, 2)
    F = biped({
        "skin": PALE, "legs": False, "hip_y": 0.98, "shoulder_y": 1.34, "hip_r": 0.17, "shoulder_r": 0.18,
        "torso": TEAL_BODICE, "shoulder_mat": TEAL_BODICE, "neck_r": 0.055, "head_c": hc, "head_r": hr,
        "arms": [((-0.21, 1.30, 0.0), (-0.31, 1.10, 0.06), (-0.22, 0.92, 0.17), TEAL_BODICE, 0.045, PALE),
                 ((0.21, 1.30, 0.0), (0.36, 1.18, 0.06), (0.40, 1.42 + wave, 0.10), TEAL_BODICE, 0.045, PALE)],
    })
    F += frustum((0, 0.02, 0), (0, 1.0, 0), 0.40, 0.17, TEAL_DRESS, 22)
    F += frustum((0, 0.02, 0), (0, 0.10, 0), 0.41, 0.385, Mat((232, 190, 90)), 22, caps=False)
    F += dress_dots(0.02, 0.40, 1.0, 0.17, Mat((250, 130, 150)), [(20, 0.25), (100, 0.4), (190, 0.22), (280, 0.45), (340, 0.3)],
                    size=(0.03, 0.03, 0.01))
    F += dress_dots(0.02, 0.40, 1.0, 0.17, SUN_YELLOW, [(60, 0.6), (150, 0.55), (240, 0.65), (320, 0.6)],
                    size=(0.022, 0.022, 0.01))
    F += cape((0, 0.88, -0.05), (0.46, 0.60, 0.24), MAGENTA)
    F += sphere((0, 1.36, 0.17), 0.03, GOLD, 8, 5)                                    # clasp
    H = hair_cap(hc, hr, AUBURN, bottom=-0.15, front=0.36, fringe=0.5)
    for sx in (-1, 1):
        H += beads([(sx * 0.19, 1.68, -0.08), (sx * 0.27, 1.44, 0.05), (sx * 0.23, 1.16, 0.17)], 0.055, AUBURN, 6, taper=0.3)
        H += sphere((sx * 0.23, 1.14, 0.18), 0.035, TEAL_DRESS, 8, 5)
        H += eye(hc, hr, sx * 20, 5, 0.046, pr=0.028, lash=AUBURN)
        H += brow(hc, hr, sx * 20, 23, AUBURN, tilt=-sx * 7, size=(0.048, 0.011, 0.02))
    H += nose(hc, hr, 0.026, PALE, -7)
    H += mouth(hc, hr, "wide", pitch=-25, half_yaw=13)
    H += decal(hc, hr, Mat((244, 176, 176)), [(-40, -12, 12), (40, -12, 12)])
    H += decal(hc, hr, Mat((200, 140, 100)),
               [(-30, -4, 3), (-38, -10, 3), (-46, -3, 3), (-34, -15, 2.5), (30, -4, 3), (38, -10, 3), (46, -3, 3), (34, -15, 2.5)],
               slices=36, stacks=24, lift=0.006)
    F += xform(H, rot=(0, 0, 3 * osc(ph, 1, 0.25)), pivot=(0, 1.42, 0))
    return F


def twig(pts, r=0.028, branches=()):
    F = polyline(pts, r, TWIG, 6)
    for a, b in branches:
        F += polyline([a, b], r * 0.7, TWIG, 5)
    return F


def build_snowman(ph):
    """Snowman: three snowballs, carrot nose, twig arms (one waving), coal
    buttons and a big coal-pebble smile, a red scarf."""
    F = []
    F += sphere((0, 0.42, 0), 0.42, SNOW, 24, 15)
    F += sphere((0, 1.06, 0), 0.32, SNOW, 22, 14)
    hc, hr = (0.0, 1.55, 0.0), 0.26
    F += sphere(hc, hr, SNOW, 22, 14)
    for pitch in (22, 0, -22):                                    # buttons
        F += sphere(on_sphere((0, 1.06, 0), 0.32, 0, pitch, -0.01), 0.035, BLACK, 8, 5)
    F += frustum((0, 1.30, 0), (0, 1.38, 0), 0.27, 0.25, RED, 20)          # scarf
    F += box((0.12, 1.16, 0.26), (0.10, 0.26, 0.03), RED, rot=(0, 0, -8))
    for sx in (-1, 1):                                            # coal eyes + pink cheeks
        F += eye(hc, hr, sx * 20, 8, 0.038, style="dark")
    F += decal(hc, hr, Mat((246, 190, 196)), [(-42, -10, 11), (42, -10, 11)])
    p0 = on_sphere(hc, hr, 0, -4, -0.03)
    F += frustum(p0, v_add(p0, (0, -0.025, 0.24)), 0.05, 0.0, CARROT, 10)
    for k in range(7):                                            # pebble smile
        t = -1 + 2 * k / 6.0
        F += sphere(on_sphere(hc, hr, t * 24, -27 + 7 * t * t, -0.005), 0.02, BLACK, 6, 4)
    for sx in (-1, 1):                                            # twig brows
        F += stick(tube((-0.045, 0, 0), (0.045, 0, 0), 0.01, TWIG, 5), hc, hr, sx * 20, 26, 0.0)
    # arms: left out, right waving from the shoulder
    F += twig([(-0.30, 1.06, 0), (-0.50, 1.00, 0.06), (-0.62, 0.86, 0.10)],
              branches=[((-0.50, 1.00, 0.06), (-0.58, 1.10, 0.08)), ((-0.58, 0.90, 0.09), (-0.68, 0.96, 0.10))])
    R = twig([(0.30, 1.10, 0), (0.46, 1.32, 0.02), (0.52, 1.54, 0.04)],
             branches=[((0.46, 1.32, 0.02), (0.58, 1.36, 0.02)), ((0.50, 1.46, 0.03), (0.60, 1.56, 0.04))])
    F += xform(R, rot=(0, 0, 12 * osc(ph, 1)), pivot=(0.30, 1.10, 0))
    for dx, dz, dy in ((-0.05, 0.02, 0.10), (0.03, -0.03, 0.12), (0.07, 0.04, 0.08)):   # twig hair
        F += tube((dx * 0.5, 1.79, dz * 0.5), (dx, 1.79 + dy, dz), 0.012, TWIG, 5)
    return F


def build_iceman(ph):
    """Ice harvester: blond, fur-trimmed tunic, gloves, an ice pick at his side."""
    hair = Mat((228, 192, 102))
    hc, hr = (0.0, 1.80, 0.0), 0.23
    glove = Mat((72, 52, 40))
    F = biped({
        "skin": SKIN, "hip_y": 0.94, "shoulder_y": 1.46, "hip_r": 0.27, "shoulder_r": 0.26, "shoulder_h": 0.09,
        "leg_dx": 0.12, "leg_r": 0.09, "leg_mat": Mat((62, 54, 60), amb=0.45), "shoe": Mat((70, 46, 30)),
        "shoe_size": (0.17, 0.10, 0.26), "torso": TUNIC, "torso_n": 20, "neck_r": 0.075, "head_c": hc, "head_r": hr,
        "arms": [((-0.31, 1.42, 0.0), (-0.42, 1.16, 0.04), (-0.38, 0.94, 0.10), TUNIC, 0.07, glove),
                 ((0.31, 1.42, 0.0), (0.44, 1.16, 0.06), (0.40, 0.98, 0.16), TUNIC, 0.07, glove)],
    })
    F += frustum((0, 0.88, 0), (0, 0.98, 0), 0.295, 0.285, FUR_TRIM, 20)                # fur hem
    F += frustum((0, 1.40, 0), (0, 1.50, 0), 0.20, 0.235, FUR_TRIM, 18)                # fur collar
    F += frustum((0, 1.03, 0), (0, 1.10, 0), 0.285, 0.283, BELT, 20, caps=False)
    F += box((0, 1.065, 0.284), (0.08, 0.06, 0.03), GOLD)
    for sx in (-1, 1):
        F += sphere((sx * 0.39, 1.02 + (0 if sx < 0 else 0.0), 0.12 if sx > 0 else 0.08), 0.078, FUR_TRIM, 10, 7)  # cuffs
    P = tube((0.40, 0.60, 0.16), (0.40, 1.32, 0.16), 0.026, WOOD, 8)
    P += frustum((0.40, 1.32, 0.16), (0.40, 1.24, 0.46), 0.045, 0.0, IRON, 8)
    P += box((0.40, 1.31, 0.09), (0.07, 0.07, 0.12), IRON)
    F += xform(P, rot=(4 * osc(ph, 1), 0, 0), pivot=(0.40, 0.98, 0.16))
    H = hair_cap(hc, hr, hair, bottom=-0.02, front=0.40, fringe=0.55, grow=0.035)
    for k in range(4):
        H += frustum(on_sphere(hc, hr, -55 + 37 * k, 58, 0.0), on_sphere(hc, hr, -65 + 42 * k, 70, 0.10), 0.04, 0.0, hair, 6)
    for sx in (-1, 1):
        H += eye(hc, hr, sx * 20, 5, 0.045)
        H += brow(hc, hr, sx * 20, 22, hair, tilt=sx * 4, size=(0.055, 0.014, 0.02))
    H += nose(hc, hr, 0.034, NOSE, -7)
    H += mouth(hc, hr, "smile", pitch=-26, half_yaw=11, r=0.012)
    H += decal(hc, hr, Mat((214, 168, 128)), [(0, -34, 26)], lift=0.003)               # stubble
    H += ears(hc, hr, SKIN)
    F += xform(H, rot=(0, 0, 2.5 * osc(ph, 1, 0.25)), pivot=(0, 1.56, 0))
    return F


def build_reindeer(ph):
    """A sitting reindeer: big dark nose, goofy eyes, tongue out, antlers."""
    F = []
    bc, br = (0.0, 0.55, -0.05), (0.34, 0.36, 0.44)
    F += sphere(bc, br, BROWN, 20, 13)
    F += decal(bc, br, BROWN_LIGHT, [(0, -8, 34)], lift=0.005)
    for sx in (-1, 1):
        F += sphere((sx * 0.31, 0.22, 0.05), (0.14, 0.17, 0.27), BROWN, 12, 8)         # folded hind legs
        F += box((sx * 0.31, 0.05, 0.30), (0.13, 0.09, 0.14), Mat((50, 36, 30), amb=0.45))
        F += tube((sx * 0.16, 0.52, 0.22), (sx * 0.16, 0.08, 0.28), 0.07, BROWN, 10)    # front legs
        F += box((sx * 0.16, 0.045, 0.30), (0.14, 0.09, 0.16), Mat((50, 36, 30), amb=0.45))
    F += sphere((0, 0.64, -0.48), 0.065, BROWN_LIGHT, 8, 5)                             # tail
    # head group (tilts on the neck)
    H = frustum((0, 0.80, 0.08), (0, 1.12, 0.22), 0.17, 0.14, BROWN, 14)
    hc, hr = (0.0, 1.24, 0.28), 0.24
    H += sphere(hc, hr, BROWN, 20, 14)
    mc, mr = (0.0, 1.16, 0.48), (0.17, 0.14, 0.20)
    H += sphere(mc, mr, BROWN_LIGHT, 14, 9)
    H += sphere((0, 1.20, 0.66), 0.085, Mat((60, 36, 30), amb=0.45), 12, 8)
    H += sphere((-0.03, 1.235, 0.72), 0.02, WHITE, 6, 4)                                 # nose shine
    H += eye(hc, hr, -24, 12, 0.058, pr=0.03, look=(4, -2))
    H += eye(hc, hr, 24, 10, 0.05, pr=0.028, look=(-4, 0))
    H += mouth(mc, mr, "smile", pitch=-32, half_yaw=22, r=0.011)
    H += sphere(on_sphere(mc, mr, 18, -38, 0.0), (0.035, 0.02, 0.05), LIPS, 8, 5)       # tongue
    for sx in (-1, 1):
        E = sphere((0, 0.09, 0), (0.05, 0.10, 0.035), BROWN, 8, 5)
        E += sphere((0, 0.09, 0.02), (0.03, 0.065, 0.02), BROWN_LIGHT, 6, 4)
        H += xform(E, rot=(0, 0, -sx * (40 + 6 * osc(ph, 1))), offset=(sx * 0.22, 1.38, 0.20))
        base = (sx * 0.12, 1.44, 0.22)
        H += polyline([base, (sx * 0.22, 1.64, 0.18), (sx * 0.30, 1.84, 0.14)], 0.028, ANTLER, 6)
        H += polyline([(sx * 0.18, 1.56, 0.20), (sx * 0.08, 1.72, 0.16)], 0.022, ANTLER, 5)
        H += polyline([(sx * 0.26, 1.74, 0.16), (sx * 0.38, 1.86, 0.12)], 0.022, ANTLER, 5)
        H += polyline([(sx * 0.15, 1.50, 0.21), (sx * 0.24, 1.50, 0.30)], 0.02, ANTLER, 5)
    F += xform(H, rot=(2 * osc(ph, 1), 0, 4 * osc(ph, 1, 0.25)), pivot=(0, 0.90, 0.12))
    return F


def build_prince(ph):
    """The prince: auburn sideburns, cream-and-navy uniform with a sash,
    one brow up, a smug little smile, a hand on his heart."""
    hc, hr = (0.0, 1.80, 0.0), 0.22
    F = biped({
        "skin": SKIN, "hip_y": 0.94, "shoulder_y": 1.46, "hip_r": 0.23, "shoulder_r": 0.24, "shoulder_h": 0.085,
        "leg_dx": 0.11, "leg_r": 0.085, "leg_mat": NAVY, "shoe": BLACK, "shoe_size": (0.15, 0.10, 0.25),
        "torso": CREAM, "torso_n": 20, "shoulder_mat": NAVY, "neck_r": 0.065, "head_c": hc, "head_r": hr,
        "arms": [((-0.28, 1.42, 0.0), (-0.38, 1.14, -0.06), (-0.14, 1.02, -0.22), CREAM, 0.065, WHITE),
                 ((0.28, 1.42, 0.0), (0.38, 1.20, 0.16), (0.11, 1.26 + 0.01 * osc(ph, 1), 0.27), CREAM, 0.065, WHITE)],
    })
    F += frustum((0, 1.40, 0), (0, 1.48, 0), 0.17, 0.19, NAVY, 18)                       # collar
    F += frustum((0, 0.93, 0), (0, 1.0, 0), 0.245, 0.243, GOLD, 20, caps=False)          # belt
    for k in range(4):
        F += sphere((0.0, 1.06 + 0.09 * k, 0.237), 0.018, GOLD, 6, 4)
    for k in range(10):                                                                  # sash
        t = k / 9.0
        y = 1.40 - 0.44 * t
        r = 0.24 - 0.01 * t
        F += stick(sphere((0, 0, 0), (0.055, 0.04, 0.012), NAVY, 8, 5), (0, y, 0), r, -46 + 92 * t, 0, 0.004)
    H = hair_cap(hc, hr, AUBURN, bottom=-0.10, front=0.38, fringe=0.55, grow=0.03)
    for sx in (-1, 1):
        H += stick(box((0, 0, 0), (0.09, 0.17, 0.035), AUBURN), hc, hr, sx * 78, -10, -0.01)   # sideburns
        H += eye(hc, hr, sx * 20, 5, 0.043, look=(0, 1))
    H += brow(hc, hr, -20, 20, AUBURN, tilt=5, size=(0.05, 0.012, 0.02))
    H += brow(hc, hr, 20, 27, AUBURN, tilt=-16, size=(0.05, 0.012, 0.02))
    H += nose(hc, hr, 0.03, NOSE, -7)
    H += mouth(hc, hr, "smug", pitch=-25, half_yaw=10, r=0.012)
    H += stick(sphere((0, 0, 0), (0.045, 0.03, 0.02), SKIN, 8, 5), hc, hr, 0, -44, -0.005)   # chin
    F += xform(H, rot=(0, 0, 3 * osc(ph, 1, 0.25)), pivot=(0, 1.56, 0))
    return F


def build_troll(ph):
    """A round mossy rock-person with crystal jewellery and a big warm grin."""
    F = []
    bc, br = (0.0, 0.55, 0.0), (0.50, 0.48, 0.46)
    F += sphere(bc, br, ROCK, 24, 15)
    F += decal(bc, br, ROCK_DARK, [(-70, -30, 12), (60, 40, 10), (150, 10, 14), (-140, -20, 11)])
    F += hair_cap(bc, br, MOSS, bottom=0.42, front=0.55, fringe=0.75, grow=0.035, slices=22, stacks=13)
    for dx, dy, dz, r in ((-0.20, 1.02, 0.06, 0.10), (0.06, 1.06, -0.04, 0.12), (0.26, 0.98, 0.10, 0.09), (0.0, 0.98, 0.26, 0.08)):
        F += sphere((dx, dy, dz), r, MOSS, 10, 7)
    for sx in (-1, 1):
        F += eye(bc, br, sx * 22, 14, 0.066, pr=0.036)
        F += brow(bc, br, sx * 22, 30, MOSS, tilt=-sx * 6, size=(0.075, 0.022, 0.028))
        F += sphere(on_sphere(bc, br, sx * 92, 10, -0.03), (0.08, 0.10, 0.05), ROCK, 8, 5)
        F += arm((sx * 0.40, 0.46, 0.14), (sx * 0.46, 0.22 + 0.015 * osc(ph, 1), 0.30), ROCK, r=0.07, hand_r=0.09, hand_mat=ROCK)
        F += sphere((sx * 0.20, 0.06, 0.18), (0.14, 0.07, 0.20), ROCK, 10, 6)
    F += sphere(on_sphere(bc, br, 0, -2, -0.03), 0.095, Mat((150, 150, 136)), 12, 8)
    F += mouth(bc, br, "smile", pitch=-25, half_yaw=22, r=0.016)
    for k in range(7):                                            # crystal necklace
        yaw = -66 + 22 * k
        p0 = on_sphere(bc, br, yaw, -40, 0.005)
        F += frustum(v_add(p0, (0, 0.02, 0)), v_add(p0, (0, -0.10, 0.02)), 0.032, 0.0,
                     CRYSTAL_PINK if k % 2 else CRYSTAL_VIOLET, 6)
        if k < 6:
            F += sphere(on_sphere(bc, br, yaw + 11, -36, 0.0), 0.016, Mat((120, 92, 60)), 6, 4)
    return F


def build_snowgiant(ph):
    """A hulking spiky snow creature: ice-spike shoulders, claws, glowing
    eyes, icicle teeth."""
    F = []
    bc, br = (0.0, 0.95, 0.0), (0.55, 0.60, 0.48)
    F += sphere(bc, br, SNOW, 22, 14)
    F += decal(bc, br, SNOW_SHADE, [(0, -30, 30)], lift=0.005)
    for sx in (-1, 1):
        F += sphere((sx * 0.46, 1.34, 0.0), 0.27, SNOW, 16, 11)
        for k, (yaw, pitch) in enumerate(((20, 60), (-30, 55), (0, 30))):
            p0 = on_sphere((sx * 0.46, 1.34, 0.0), 0.27, sx * yaw + sx * 60, pitch, -0.02)
            p1 = on_sphere((sx * 0.46, 1.34, 0.0), 0.27, sx * yaw + sx * 60, pitch, 0.22 - 0.04 * k)
            F += frustum(p0, p1, 0.06, 0.0, ICE_SPIKE, 7)
    for yaw, pitch, L in ((180, 40, 0.30), (150, 20, 0.26), (210, 20, 0.26), (180, 5, 0.22), (165, -20, 0.2), (195, -20, 0.2)):
        F += frustum(on_sphere(bc, br, yaw, pitch, -0.03), on_sphere(bc, br, yaw, pitch, L), 0.07, 0.0, ICE_SPIKE, 7)
    hc, hr = (0.0, 1.66, 0.10), (0.34, 0.29, 0.31)
    F += sphere(hc, hr, SNOW, 20, 13)
    for sx in (-1, 1):
        F += sphere(on_sphere(hc, hr, sx * 22, 12, -0.03), 0.05, GLOW_EYE, 10, 7)
        F += sphere(on_sphere(hc, hr, sx * 20, 11, 0.01), 0.022, BLACK, 8, 5)
        F += brow(hc, hr, sx * 24, 30, SNOW_SHADE, tilt=sx * 20, size=(0.10, 0.03, 0.035), out=0.01)
    F += stick(sphere((0, 0, 0), (0.20, 0.085, 0.02), MOUTH, 12, 7), hc, hr, 0, -26, 0.004)
    for k in range(5):
        yaw = -22 + 11 * k
        p0 = on_sphere(hc, hr, yaw, -17, 0.02)
        F += frustum(p0, v_add(p0, (0, -0.06, 0.01)), 0.016, 0.0, ICE_SPIKE, 5)
    for k in range(3):                                            # crest
        F += frustum(on_sphere(hc, hr, -30 + 30 * k, 60, -0.03), on_sphere(hc, hr, -35 + 35 * k, 68, 0.16), 0.04, 0.0, ICE_SPIKE, 6)
    for sx in (-1, 1):
        hand = (sx * 0.60, 0.68 + 0.015 * sx * osc(ph, 1), 0.34)
        F += arm((sx * 0.62, 1.28, 0.06), hand, SNOW, elbow=(sx * 0.76, 0.98, 0.16), r=0.13, hand_r=0.17, hand_mat=SNOW)
        for j in (-1, 0, 1):
            p0 = v_add(hand, (j * 0.09, -0.05, 0.12))
            F += frustum(p0, v_add(p0, (j * 0.03, -0.09, 0.12)), 0.035, 0.0, ICE_SPIKE, 6)
        F += tube((sx * 0.28, 0.45, 0.0), (sx * 0.28, 0.08, 0.04), 0.16, SNOW, 12)
        F += sphere((sx * 0.28, 0.08, 0.12), (0.22, 0.08, 0.28), SNOW, 12, 7)
        for j in (-1, 0, 1):
            F += frustum((sx * 0.28 + j * 0.10, 0.06, 0.34), (sx * 0.28 + j * 0.12, 0.03, 0.46), 0.03, 0.0, ICE_SPIKE, 5)
    return F


WINTER = {
    "title": "Winter kingdom",
    "blurb": "a snowy kingdom -- an ice queen, her sunny sister and their friends",
    "characters": [
        ("icequeen", "Ice queen", build_icequeen, 0.86),
        ("sunny", "Sunny", build_sunny, 0.84),
        ("snowman", "Snowman", build_snowman, 0.80),
        ("iceman", "Iceman", build_iceman, 0.86),
        ("reindeer", "Reindeer", build_reindeer, 0.80),
        ("prince", "Prince", build_prince, 0.86),
        ("troll", "Troll", build_troll, 0.70),
        ("snowgiant", "Snow giant", build_snowgiant, 0.90),
    ],
}


SETS = {"dwarves": DWARVES, "island": ISLAND, "winter": WINTER}
SET_ORDER = ["dwarves", "island", "winter"]


# --------------------------------------------------------------------------
# renderer
# --------------------------------------------------------------------------

class Camera(object):
    def __init__(self, faces, pitch_deg=10.0, dist_factor=3.0):
        ys = [p[1] for pts, _ in faces for p in pts]
        self.y0, self.h = min(ys), max(ys) - min(ys)
        look = (0.0, min(ys) + self.h * 0.5, 0.0)
        d = self.h * dist_factor
        self.pos = (0.0, look[1] + d * math.tan(math.radians(pitch_deg)), d)
        self.F = v_norm(v_sub(look, self.pos))
        self.R = v_norm(v_cross(self.F, (0.0, 1.0, 0.0)))
        self.U = v_cross(self.R, self.F)
        # lights, given in camera space (x right, y up, z into the scene)
        self.key = self._to_world(v_norm((-0.50, 0.72, -0.48)))
        self.fill = self._to_world(v_norm((0.70, -0.10, -0.60)))
        # floor ellipse radius from the footprint of the lower part of the mesh
        rad = 0.0
        for pts, _ in faces:
            for p in pts:
                if p[1] < self.h * 0.35:
                    rad = max(rad, math.hypot(p[0], p[2]))
        self.floor_r = rad * 1.12

    def _to_world(self, v):
        return v_norm(v_add(v_add(v_mul(self.R, v[0]), v_mul(self.U, v[1])),
                            v_mul(self.F, v[2])))

    def project(self, p):
        d = v_sub(p, self.pos)
        z = v_dot(d, self.F)
        return (v_dot(d, self.R) / z, v_dot(d, self.U) / z)


def shade(mat, n, to_eye, cam):
    """Ambient + key + weak fill + rim.  A little more contrast than a plain
    Lambert so silhouettes and volumes survive 16 colours at 135 px."""
    i = mat.amb * 0.9 + 0.76 * max(0.0, v_dot(n, cam.key)) + 0.16 * max(0.0, v_dot(n, cam.fill))
    i += 0.22 * (1.0 - max(0.0, v_dot(n, to_eye))) ** 2
    r, g, b = mat.rgb
    return (min(255, int(r * i)), min(255, int(g * i)), min(255, int(b * i)))


def project_turntable(frame_faces, cam):
    """Pass 1: rotate, cull, shade and project every frame.  Returns
    (frames, bounds) where frames[i] is a list of (dist, [(x,y)...], rgb)."""
    frames = []
    minx = miny = float("inf")
    maxx = maxy = float("-inf")
    campos = cam.pos
    nframes = len(frame_faces)
    for i, faces in enumerate(frame_faces):
        ang = 2 * math.pi * i / nframes
        ca, sa = math.cos(ang), math.sin(ang)
        polys = []
        for pts, mat in faces:
            w = [(x * ca + z * sa, y, -x * sa + z * ca) for (x, y, z) in pts]
            c = centroid(w)
            n = v_cross(v_sub(w[1], w[0]), v_sub(w[2], w[0]))
            d = v_sub(c, campos)
            if v_dot(n, d) >= 0:
                continue  # back-facing
            nl = v_len(n)
            if nl < 1e-12:
                continue
            n = (n[0] / nl, n[1] / nl, n[2] / nl)
            dist = v_len(d)
            to_eye = (-d[0] / dist, -d[1] / dist, -d[2] / dist)
            col = shade(mat, n, to_eye, cam)
            sp = []
            for p in w:
                x, y = cam.project(p)
                sp.append((x, y))
                if x < minx:
                    minx = x
                if x > maxx:
                    maxx = x
                if y < miny:
                    miny = y
                if y > maxy:
                    maxy = y
            polys.append((dist, sp, col))
        polys.sort(key=lambda t: -t[0])
        frames.append(polys)
    return frames, (minx, miny, maxx, maxy)


def rasterize(frames, bounds, cam, size, ss, fill_frac=0.80, shadow_k=None):
    """Pass 2: fit the whole turntable into the frame and draw each frame on a
    transparent canvas over a two-tone floor shadow (soft disc + denser contact
    shadow, both semi-transparent black)."""
    W, H = size
    Wd, Hd = W * ss, H * ss
    minx, miny, maxx, maxy = bounds
    scale = fill_frac * Hd / (maxy - miny)
    width_px = (maxx - minx) * scale
    if width_px > 0.97 * Wd:
        scale *= 0.97 * Wd / width_px
    cx = Wd / 2.0
    cy = Hd / 2.0 + (miny + maxy) * 0.5 * scale

    def ellipse(r, ox=0.0, oz=0.0, n=56):
        pts = []
        for k in range(n):
            a = 2 * math.pi * k / n
            x, y = cam.project((ox + r * math.cos(a), cam.y0, oz + r * math.sin(a)))
            pts.append((cx + x * scale, cy - y * scale))
        return pts

    floor = ellipse(cam.floor_r)
    images = []
    for i, polys in enumerate(frames):
        im = Image.new("RGBA", (Wd, Hd), (0, 0, 0, 0))    # transparent: composites over any background
        dr = ImageDraw.Draw(im)                            # (ImageDraw replaces pixels, so the
        dr.polygon(floor, fill=FLOOR_RGBA)                 #  opaque figure simply overwrites the shadow)
        k = shadow_k[i] if shadow_k else 1.0
        dr.polygon(ellipse(cam.floor_r * 0.64 * k, cam.floor_r * 0.10, cam.floor_r * 0.04), fill=SHADOW_RGBA)
        for _, sp, col in polys:
            pts = [(cx + x * scale, cy - y * scale) for x, y in sp]
            rgba = col + (255,)
            dr.polygon(pts, fill=rgba, outline=rgba)
        if ss > 1:
            # Pillow resizes RGBA via premultiplied RGBa, so an edge pixel comes out as the
            # figure's colour with a fractional alpha -- no dark fringe over light backgrounds.
            im = im.resize((W, H), Image.LANCZOS)
        images.append(im)
    return images, scale * (maxy - miny) / Hd


def render_character(builder, nframes, size, ss, fill_frac, bob_px=2.0):
    """Build every frame (phase = i/N), add the breathing bob, project and draw."""
    W, H = size
    base = builder(0.0)
    cam = Camera(base)
    amp = bob_px * cam.h / (fill_frac * H)          # ~bob_px pixels in world units
    y_lo, y_hi = cam.y0 + 0.28 * cam.h, cam.y0 + 0.60 * cam.h
    frame_faces, shadow_k = [], []
    for i in range(nframes):
        ph = i / float(nframes)
        b = osc(ph, 2)                               # two breaths per turn
        frame_faces.append(bob_mesh(builder(ph), amp * b, y_lo, y_hi))
        shadow_k.append(1.0 - 0.05 * b)
    frames, bounds = project_turntable(frame_faces, cam)
    images, fill = rasterize(frames, bounds, cam, size, ss, fill_frac, shadow_k)
    return images, fill, len(base)


# --------------------------------------------------------------------------
# outputs
# --------------------------------------------------------------------------

def matte(im, rgb):
    """Composite a transparent RGBA frame over a solid colour -> RGB."""
    bg = Image.new("RGBA", im.size, tuple(rgb) + (255,))
    return Image.alpha_composite(bg, im.convert("RGBA")).convert("RGB")


def write_gif(images, path, fps):
    # GIF transparency is 1-bit (a pixel is either there or not), which would turn the
    # anti-aliased edges into a jagged cut-out, so the GIF is matted over black -- the
    # look the frames have on the tubes with the bridge's default --bg.  The PNG frames
    # themselves stay transparent.
    dur = int(round(1000.0 / fps))
    pal = [matte(im, (0, 0, 0)).quantize(colors=128, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
           for im in images]
    pal[0].save(path, save_all=True, append_images=pal[1:], duration=dur, loop=0,
                disposal=1, optimize=False)


def write_sheet(cells, size, path):
    W, H = size
    label_h = 16
    sheet = Image.new("RGB", (W * len(cells), H + label_h), SHEET_BG)
    dr = ImageDraw.Draw(sheet)
    try:
        font = ImageFont.load_default(size=11)
    except TypeError:  # older Pillow
        font = ImageFont.load_default()
    for i, (name, im) in enumerate(cells):
        sheet.paste(matte(im, SHEET_BG), (i * W, 0))
        dr.text((i * W + 4, H + 2), name, fill=(200, 200, 200), font=font)
        if i:
            dr.line([(i * W, 0), (i * W, H + label_h)], fill=(72, 72, 72))
    sheet.save(path)


def write_set_meta(set_id, spec, out, size, nframes, fps, ss):
    W, H = size
    rel = "samples/" + set_id
    manifest = {
        "set": set_id, "title": spec["title"], "fps": fps, "frames": nframes, "w": W, "h": H,
        "characters": [{"name": n, "label": label, "dir": rel + "/" + n, "frames": nframes}
                       for n, label, _, _ in spec["characters"]],
        "size": [W, H],                                  # older readers
        "license": "original artwork, CC0",
    }
    with open(os.path.join(out, "manifest.json"), "w") as fh:
        fh.write(json.dumps(manifest, separators=(",", ":")) + "\n")
    names = ", ".join("`%s` (%s)" % (n, label) for n, label, _, _ in spec["characters"])
    with open(os.path.join(out, "README.md"), "w") as fh:
        fh.write("# %s\n\n" % spec["title"])
        fh.write("Original low-poly turntable animations (%dx%d, %d frames, %d fps) of %s: %s.\n\n"
                 % (W, H, nframes, fps, spec["blurb"], names))
        fh.write("The designs, names, colours and props are our own -- evocative homages built from "
                 "spheres, cones and boxes, not traced from or named after any film. Released CC0 "
                 "(public domain); use them however you like.\n\n")
        fh.write("Each `<name>/f00.png .. f%02d.png` is one frame of a seamless loop (`<name>.gif` is the "
                 "same loop; `sheet.png` shows the front view of everyone). `manifest.json` lists the "
                 "cast for `tools/bridge/bridge.py play chars:%s` and the helper page.\n\n" % (nframes - 1, set_id))
        fh.write("The frames are RGBA PNGs with a **transparent background**: only the figure and its "
                 "semi-transparent black floor shadow are drawn, so they composite over anything -- a solid "
                 "colour, a gradient, an animated scene. `tools/bridge/bridge.py play chars:%s --bg ...` picks "
                 "what goes behind them on the tubes (`black` by default, `#rrggbb`, `#rrggbb-#rrggbb` vertical "
                 "gradient or `glow:#rrggbb`), and the helper page draws them over its own background. The GIFs "
                 "are matted over black and `sheet.png` over dark grey.\n\n" % set_id)
        fh.write("Regenerate with `python3 tools/render_characters.py --set %s --frames %d --size %dx%d --ss %d` "
                 "(Python 3.9 + Pillow, no other deps).\n" % (set_id, nframes, W, H, ss))


def main(argv=None):
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--set", default="all", help="character set to render: %s or all (default)" % "|".join(SET_ORDER))
    ap.add_argument("--out", default=None, help="output directory (default: samples/<set>)")
    ap.add_argument("--frames", type=int, default=36, help="frames per turntable loop")
    ap.add_argument("--size", default="135x240", help="frame size WxH")
    ap.add_argument("--ss", type=int, default=3, help="supersampling factor (1 = off)")
    ap.add_argument("--fps", type=int, default=12, help="GIF frame rate")
    ap.add_argument("--only", default="", help="comma-separated subset of character names (no sheet/manifest)")
    ap.add_argument("--no-sidecar", action="store_true", help="don't rebuild samples/characters.js afterwards")
    args = ap.parse_args(argv)

    W, H = (int(v) for v in args.size.lower().split("x"))
    size = (W, H)
    sets = SET_ORDER if args.set == "all" else [s.strip() for s in args.set.split(",")]
    for s in sets:
        if s not in SETS:
            ap.error("unknown set %r (have %s)" % (s, ", ".join(SET_ORDER)))
    if args.out and len(sets) != 1:
        ap.error("--out needs a single --set")
    wanted = [n.strip() for n in args.only.split(",") if n.strip()]
    samples = os.path.normpath(os.path.join(here, "..", "samples"))

    t0 = time.time()
    for set_id in sets:
        spec = SETS[set_id]
        out = os.path.normpath(args.out) if args.out else os.path.join(samples, set_id)
        os.makedirs(out, exist_ok=True)
        todo = [c for c in spec["characters"] if not wanted or c[0] in wanted]
        if not todo:
            print("%s: none of %s in this set" % (set_id, wanted))
            continue
        print("== %s (%s) -> %s" % (set_id, spec["title"], out))
        sheet_cells = []
        for name, label, builder, fill_frac in todo:
            t1 = time.time()
            images, fill, npoly = render_character(builder, args.frames, size, args.ss, fill_frac)
            cdir = os.path.join(out, name)
            os.makedirs(cdir, exist_ok=True)
            for i, im in enumerate(images):
                im.save(os.path.join(cdir, "f%02d.png" % i))
            write_gif(images, os.path.join(out, name + ".gif"), args.fps)
            sheet_cells.append((name, images[0]))
            gif_kb = os.path.getsize(os.path.join(out, name + ".gif")) / 1024.0
            print("  %-10s %4d polys  fill %.0f%%  gif %3.0f KB  %.1fs" %
                  (name, npoly, fill * 100, gif_kb, time.time() - t1))
        if not wanted:
            write_sheet(sheet_cells, size, os.path.join(out, "sheet.png"))
            write_set_meta(set_id, spec, out, size, args.frames, args.fps, args.ss)
    print("render total %.1fs" % (time.time() - t0))

    if not args.no_sidecar and not args.out:
        sys.path.insert(0, here)
        import gen_characters_sidecar
        gen_characters_sidecar.main(["--samples", samples])
    return 0


if __name__ == "__main__":
    sys.exit(main())
