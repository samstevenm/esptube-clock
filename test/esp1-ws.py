#!/usr/bin/env python3
"""esp1-ws.py — exercise the node's WebSocket endpoint (ws://<host>:5557) with the stdlib.

The browser helper streams through this port. Checks: handshake, a text frame runs a
shell command (reply is a text frame), a binary PING gets a binary 4-byte ack, and
PAL4x2 full-panel blits to every populated tube for --seconds (frame rate via acks),
one WebSocket per tube like the helper does.

Usage: test/esp1-ws.py [host] [--seconds 5] [--token T]
"""
import argparse, base64, json, os, socket, struct, sys, time, urllib.request

W, H = 135, 240

def ws_connect(host, port=5557, timeout=5):
    s = socket.create_connection((host, port), timeout=timeout)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    key = base64.b64encode(os.urandom(16)).decode()
    s.sendall((f"GET / HTTP/1.1\r\nHost: {host}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
               f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n").encode())
    resp = b""
    while b"\r\n\r\n" not in resp:
        c = s.recv(1024)
        if not c: raise ConnectionError("handshake closed")
        resp += c
    if b" 101 " not in resp.split(b"\r\n", 1)[0]: raise ConnectionError("bad handshake: %r" % resp[:80])
    return s

def ws_send(s, opcode, payload):
    mask = os.urandom(4); n = len(payload)
    h = bytes([0x80 | opcode])
    if n < 126: h += bytes([0x80 | n])
    elif n < 65536: h += bytes([0x80 | 126]) + struct.pack(">H", n)
    else: h += bytes([0x80 | 127]) + struct.pack(">Q", n)
    masked = bytes(b ^ mask[i & 3] for i, b in enumerate(payload))
    s.sendall(h + mask + masked)

def ws_recv(s):
    def rd(n):
        b = b""
        while len(b) < n:
            c = s.recv(n - len(b))
            if not c: raise ConnectionError("closed")
            b += c
        return b
    h = rd(2); op = h[0] & 0x0F; n = h[1] & 0x7F
    if n == 126: n = struct.unpack(">H", rd(2))[0]
    elif n == 127: n = struct.unpack(">Q", rd(8))[0]
    if h[1] & 0x80: rd(4)     # server frames aren't masked, but tolerate it
    return op, rd(n)

def header(op, tube, x, y, w, h, fmt, seq, ln):
    return struct.pack("<BBBBHHHHBBHI", 0xE5, 0x7B, op, tube, x, y, w, h, fmt, seq, 0, ln)

def pal4x2_frame(k, tube):
    pal = [((i * 2) << 11) | (((i * 4 + tube * 8) & 0x3F) << 5) | (i * 2) for i in range(16)]
    out = bytearray(b"".join(struct.pack(">H", c) for c in pal))
    rowb = (W + 1) // 2
    for y in range((H + 1) // 2):
        row = bytearray(rowb)
        for x in range(W):
            v = ((x + 2 * y + k * 6) // 12) & 15
            if x & 1: row[x >> 1] |= v
            else:     row[x >> 1] = v << 4
        out += row
    return bytes(out)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("host", nargs="?", default="esptube.local")
    ap.add_argument("--seconds", type=float, default=5)
    ap.add_argument("--token", default="")
    a = ap.parse_args()
    st = json.load(urllib.request.urlopen(f"http://{a.host}/status", timeout=5))
    tubes = [t["index"] for t in st["tubes"] if t.get("populated") and t["index"] not in st.get("dead_tubes", [])]
    print(f"node {a.host}: tubes {tubes}")

    s = ws_connect(a.host)
    if a.token: ws_send(s, 0x2, f"HELLO {a.token}\n".encode()); print("auth:", ws_recv(s))
    ws_send(s, 0x1, b"fps"); op, d = ws_recv(s); print("text 'fps' ->", op, d.decode(errors='replace').strip())
    t = time.monotonic(); ws_send(s, 0x2, header(3, 0, 0, 0, 0, 0, 0, 1, 0)); op, d = ws_recv(s)
    print(f"binary PING -> op {op} ack {d.hex()} in {(time.monotonic()-t)*1000:.1f} ms")
    s.close()

    # one socket per tube, ≤2 frames in flight each
    socks = {tb: ws_connect(a.host) for tb in tubes}
    if a.token:
        for tb in tubes: ws_send(socks[tb], 0x2, f"HELLO {a.token}\n".encode()); ws_recv(socks[tb])
    ring = {tb: [pal4x2_frame(k, tb) for k in range(6)] for tb in tubes}
    pending = {tb: 0 for tb in tubes}; seq = 0; sent = acked = nbytes = bad = 0; k = 0
    t0 = time.monotonic()
    while time.monotonic() - t0 < a.seconds:
        k += 1
        for tb in tubes:
            p = ring[tb][k % 6]
            ws_send(socks[tb], 0x2, header(1, tb, 0, 0, W, H, 3, seq & 0xFF, len(p)) + p); seq += 1; sent += 1; nbytes += 20 + len(p); pending[tb] += 1
            while pending[tb] > 2:
                op, d = ws_recv(socks[tb]); pending[tb] -= 1; acked += 1
                if len(d) != 4 or d[2] != 0: bad += 1
    for tb in tubes:
        while pending[tb] > 0:
            op, d = ws_recv(socks[tb]); pending[tb] -= 1; acked += 1
    el = time.monotonic() - t0
    print(f"WS pal4x2 x{len(tubes)} sockets: {acked/el:5.1f} tube-updates/s = {acked/el/len(tubes):4.1f} fps × {len(tubes)} · {nbytes/el/1024:5.0f} KB/s · bad {bad}")
    for sck in socks.values(): sck.close()
    return 0

if __name__ == "__main__":
    sys.exit(main())
