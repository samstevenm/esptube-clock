#!/usr/bin/env python3
"""esp1-bench.py — measure the ESP/1 stream channel on a live clock (stdlib only).

Sends full-panel frames of a moving test pattern to every populated tube for N
seconds per format and reports achieved frames/s, tube-updates/s and KB/s, using
the node's acks (so what's counted is what was DRAWN). Also checks the ack
status codes and that bad frames are rejected.

Usage: test/esp1-bench.py [host] [--seconds 5] [--fmt raw,rle,pal4] [--inflight 2] [--token T]
Spec: docs/ESP1-PROTOCOL.md.
"""
import argparse, json, socket, struct, sys, time, urllib.request

W, H = 135, 240

def rgb565(r, g, b): return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)

def pattern(t, tube):
    """RGB565 pixel list: moving colour bands + a bouncing block, unique per tube."""
    px = [0] * (W * H)
    band = int(t * 60) % H
    bx = int((W - 30) * (0.5 + 0.5 * __import__('math').sin(t * 2 + tube)))
    by = int((H - 30) * (0.5 + 0.5 * __import__('math').cos(t * 1.3 + tube)))
    for y in range(H):
        base = rgb565(((y + band) * 2) & 0xFF, (tube * 40) & 0xFF, ((y * 3 + band) & 0xFF))
        row = y * W
        for x in range(W):
            px[row + x] = base if x % 45 < 30 else 0
        if by <= y < by + 30:
            for x in range(bx, bx + 30): px[row + x] = 0xFFFF
    return px

def enc_raw(px): return b"".join(struct.pack(">H", p) for p in px)

def enc_rle(px):
    out = bytearray(); i = 0; n = len(px)
    while i < n:
        v = px[i]; run = 1
        while i + run < n and px[i + run] == v and run < 255: run += 1
        out += bytes((run, v >> 8, v & 0xFF)); i += run
    return bytes(out)

def enc_pal4(px):
    pal = []; idx = {}
    for p in px:
        if p not in idx:
            if len(pal) < 16: idx[p] = len(pal); pal.append(p)
            else: idx[p] = 0
    pal += [0] * (16 - len(pal))
    out = bytearray(b"".join(struct.pack(">H", c) for c in pal))
    rowb = (W + 1) // 2
    for y in range(H):
        row = bytearray(rowb)
        for x in range(W):
            v = idx[px[y * W + x]]
            if x & 1: row[x >> 1] |= v
            else:     row[x >> 1] = v << 4
        out += row
    return bytes(out)

def enc_pal4x2(px):
    """fmt 3: palette + ceil(H/2) rows of nibbles; the node draws each row twice (line doubling)."""
    half = [px[(2 * y) * W + x] for y in range((H + 1) // 2) for x in range(W)]   # keep even rows
    pal = []; idx = {}
    for p in half:
        if p not in idx:
            if len(pal) < 16: idx[p] = len(pal); pal.append(p)
            else: idx[p] = 0
    pal += [0] * (16 - len(pal))
    out = bytearray(b"".join(struct.pack(">H", c) for c in pal))
    rowb = (W + 1) // 2
    for y in range((H + 1) // 2):
        row = bytearray(rowb)
        for x in range(W):
            v = idx[half[y * W + x]]
            if x & 1: row[x >> 1] |= v
            else:     row[x >> 1] = v << 4
        out += row
    return bytes(out)

ENC = {"raw": (0, enc_raw), "rle": (1, enc_rle), "pal4": (2, enc_pal4), "pal4x2": (3, enc_pal4x2)}

def header(op, tube, x, y, w, h, fmt, seq, ln):
    return struct.pack("<BBBBHHHHBBHI", 0xE5, 0x7B, op, tube, x, y, w, h, fmt, seq, 0, ln)

LOST = [0]
def recv_ack(s):
    """4-byte ack; a timeout counts as a lost ack (returns seq -1) instead of aborting the run"""
    a = b""
    try:
        while len(a) < 4:
            c = s.recv(4 - len(a))
            if not c: raise ConnectionError("closed")
            a += c
    except socket.timeout:
        LOST[0] += 1; return -1, 255
    if a[0] != 0xA5: raise ValueError("bad ack %r" % a)
    return a[1], a[2]

def udp_bench(a):
    """UDP: the same byte stream cut into datagrams; acks come back over UDP (may be lost)."""
    st = json.load(urllib.request.urlopen(f"http://{a.host}/status", timeout=5))
    tubes = [t["index"] for t in st["tubes"] if t.get("populated") and t["index"] not in st.get("dead_tubes", [])]
    print(f"node {a.host}: tubes {tubes}, UDP :5556, chunk {a.chunk}")
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.settimeout(0.25)
    addr = (socket.gethostbyname(a.host), 5556)
    for name in a.fmt.split(","):
        fmt, enc = ENC[name]
        ring = {tube: [enc(pattern(k * 0.25, tube)) for k in range(8)] for tube in tubes}
        seq = 0; sent = 0; acked = 0; nbytes = 0; bad = 0; lost = 0; pending = {}; k = 0
        t0 = time.monotonic()
        while time.monotonic() - t0 < a.seconds:
            k += 1
            for tube in tubes:
                payload = ring[tube][k % 8]
                frame = header(1, tube, 0, 0, W, H, fmt, seq & 0xFF, len(payload)) + payload
                for off in range(0, len(frame), a.chunk): s.sendto(frame[off:off + a.chunk], addr)
                pending[seq & 0xFF] = time.monotonic(); seq += 1; sent += 1; nbytes += len(frame)
                # keep ≤ inflight frames outstanding; a lost ack times out and counts as lost
                while len(pending) > a.inflight:
                    try:
                        d, _ = s.recvfrom(64)
                        if len(d) >= 4 and d[0] == 0xA5:
                            if d[1] in pending: pending.pop(d[1]); acked += 1
                            if d[2] != 0: bad += 1
                    except socket.timeout:
                        lost += len(pending); pending.clear()
        while pending:
            try:
                d, _ = s.recvfrom(64)
                if len(d) >= 4 and d[0] == 0xA5 and d[1] in pending: pending.pop(d[1]); acked += 1
            except socket.timeout: lost += len(pending); pending.clear()
        el = time.monotonic() - t0
        print(f"udp {name:5s}: {acked/el:6.1f} drawn tube-updates/s = {acked/el/len(tubes):5.1f} fps × {len(tubes)} · sent {sent/el:5.1f}/s · "
              f"{nbytes/el/1024:7.1f} KB/s · {nbytes/max(sent,1)/1024:5.1f} KB/frame · bad {bad} · unacked {lost}")
        time.sleep(0.3)
    return 0

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("host", nargs="?", default="esptube.local")
    ap.add_argument("--seconds", type=float, default=5)
    ap.add_argument("--fmt", default="raw,rle,pal4")
    ap.add_argument("--inflight", type=int, default=2)
    ap.add_argument("--token", default="")
    ap.add_argument("--conns", type=int, default=6, help="TCP connections (one per tube, up to 6); 1 = the old single-socket behaviour")
    ap.add_argument("--udp", action="store_true", help="send over UDP :5556 in 1400-byte datagrams (acks back over UDP)")
    ap.add_argument("--chunk", type=int, default=1400)
    a = ap.parse_args()
    if a.udp:
        return udp_bench(a)

    st = json.load(urllib.request.urlopen(f"http://{a.host}/status", timeout=5))
    tubes = [t["index"] for t in st["tubes"] if t.get("populated")]
    tubes = [i for i in tubes if i not in st.get("dead_tubes", [])]
    print(f"node {a.host}: tubes {tubes}, stream port {st['stream']['port']}, token={'yes' if st['stream']['token'] else 'no'}")

    nconn = max(1, min(a.conns, len(tubes)))
    for name in a.fmt.split(","):
        fmt, enc = ENC[name]
        # one TCP connection per tube (up to --conns): each gets its own lwIP receive window
        socks = []
        for c in range(nconn):
            s = socket.create_connection((a.host, st["stream"]["port"]), timeout=5)
            s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            if a.token:
                s.sendall(f"HELLO {a.token}\n".encode()); r = s.recv(3)
                if r != b"OK\n": print("auth failed", r); return 1
            socks.append(s)
        sock_of = {tube: socks[i % nconn] for i, tube in enumerate(tubes)}
        pending = {id(s): [] for s in socks}
        # pre-encode a ring of frames so the (slow, pure-Python) encoder isn't what we measure
        ring = {tube: [enc(pattern(k * 0.25, tube)) for k in range(8)] for tube in tubes}
        seq = 0; sent = 0; acked = 0; nbytes = 0; bad = 0; lat = []; k = 0
        def drain(s, limit):
            nonlocal acked, bad
            q = pending[id(s)]
            while len(q) > limit:
                sq, status = recv_ack(s); acked += 1
                exp, ts = q.pop(0); lat.append(time.monotonic() - ts)
                if sq != exp or status != 0: bad += 1
        t0 = time.monotonic()
        while time.monotonic() - t0 < a.seconds:
            k += 1
            for tube in tubes:
                s = sock_of[tube]; payload = ring[tube][k % 8]
                s.sendall(header(1, tube, 0, 0, W, H, fmt, seq & 0xFF, len(payload)) + payload)
                pending[id(s)].append((seq & 0xFF, time.monotonic())); seq += 1; sent += 1; nbytes += 20 + len(payload)
                drain(s, a.inflight)
        try:   # node-side view while the stream is still hot: parser CPU % and loop rate
            ns = json.load(urllib.request.urlopen(f"http://{a.host}/status", timeout=3))["stream"]
            print(f"      node: fps {ns['fps']} · {ns['kbps']} KB/s · busy {ns.get('busy_pct')}% inside parser · {ns.get('loops_per_s')} loops/s · {ns.get('clients')} clients")
        except Exception as e: print("      node status:", e)
        for s in socks: drain(s, 0)
        el = time.monotonic() - t0
        avg = sum(lat) / len(lat) if lat else 0
        print(f"{name:5s} x{nconn} conn: {acked/el:6.1f} tube-updates/s = {acked/el/len(tubes):5.1f} fps × {len(tubes)} tubes · {nbytes/el/1024:7.1f} KB/s · "
              f"{nbytes/max(sent,1)/1024:5.1f} KB/frame · ack latency {avg*1000:5.0f} ms · bad {bad} · lost acks {LOST[0]}")
        LOST[0] = 0
        for s in socks: s.close()
        time.sleep(0.3)
        s = socks[0]

    # protocol checks: a bad tube and a bad fmt must be acked with non-zero status, then the channel keeps working
    s = socket.create_connection((a.host, st["stream"]["port"]), timeout=5)
    if a.token: s.sendall(f"HELLO {a.token}\n".encode()); s.recv(3)
    s.sendall(header(1, 9, 0, 0, W, H, 0, 7, 4) + b"\0\0\0\0"); sq, stt = recv_ack(s); print(f"bad tube  -> ack seq {sq} status {stt} (want 1)")
    s.sendall(header(1, tubes[0], 0, 0, W, H, 9, 8, 2) + b"\0\0");     sq, stt = recv_ack(s); print(f"bad fmt   -> ack seq {sq} status {stt} (want 2)")
    s.sendall(header(3, 0, 0, 0, 0, 0, 0, 9, 0));                       sq, stt = recv_ack(s); print(f"ping      -> ack seq {sq} status {stt} (want 0)")
    s.sendall(header(2, 0xFF, 0, 0, W, H, 0, 10, 2) + b"\x00\x00");     sq, stt = recv_ack(s); print(f"fill all black -> ack seq {sq} status {stt} (want 0)")
    s.close()
    return 0

if __name__ == "__main__":
    sys.exit(main())
