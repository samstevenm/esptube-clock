#!/usr/bin/env python3
"""esp1-cancel.py — ESP/1.1 over WebSocket against a live clock (no cable): node-owned epoch,
stale/successor generations, CANCEL (+clear) beating a frame that is still arriving, REST clear
doing the same, corrupt lengths not wedging a parser, two writers never starving each other, and
an aborted REST pixel upload no longer blocking the streams.

Usage: test/esp1-cancel.py <host> [--tube 1]
"""
import argparse, base64, json, os, socket, struct, sys, time, threading, urllib.request

W, H = 135, 240
F_NOACK, F_CLEAR, F_V11 = 0x01, 0x04, 0x80

def ws_connect(host, port=5557, timeout=6):
    s = socket.create_connection((host, port), timeout=timeout); s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    key = base64.b64encode(os.urandom(16)).decode()
    s.sendall((f"GET / HTTP/1.1\r\nHost: {host}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n").encode())
    resp = b""
    while b"\r\n\r\n" not in resp:
        c = s.recv(1024)
        if not c: raise ConnectionError("handshake closed")
        resp += c
    if b" 101 " not in resp.split(b"\r\n", 1)[0]: raise ConnectionError("bad handshake: %r" % resp[:80])
    return s
def ws_send(s, opcode, payload):
    mask = os.urandom(4); n = len(payload); h = bytes([0x80 | opcode])
    if n < 126: h += bytes([0x80 | n])
    elif n < 65536: h += bytes([0x80 | 126]) + struct.pack(">H", n)
    else: h += bytes([0x80 | 127]) + struct.pack(">Q", n)
    s.sendall(h + mask + bytes(b ^ mask[i & 3] for i, b in enumerate(payload)))
def ws_recv(s, timeout=4.0):
    s.settimeout(timeout)
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
    if h[1] & 0x80: rd(4)
    return op, rd(n)
def ack_of(s, seq, timeout=4.0):
    end = time.time() + timeout
    while time.time() < end:
        try: op, d = ws_recv(s, max(0.05, end - time.time()))
        except (socket.timeout, TimeoutError): return None
        if op == 2 and len(d) >= 4 and d[0] == 0xA5 and d[1] == seq: return d
    return None
def hdr(op, tube=0, x=0, y=0, w=W, h=H, fmt=0, seq=0, gen=0, flags=F_V11, ln=0):
    return struct.pack("<BBBBHHHHBBBBI", 0xE5, 0x7B, op, tube, x, y, w, h, fmt, seq, gen, flags, ln)
def status(host): return json.load(urllib.request.urlopen(f"http://{host}/status", timeout=5))
def post(host, path, data=b"", ctype="application/json"):
    r = urllib.request.Request(f"http://{host}{path}", data=data, method="POST", headers={"Content-Type": ctype}); return urllib.request.urlopen(r, timeout=6).read()

ok_all = True
def check(name, ok, detail=""):
    global ok_all; ok_all &= bool(ok); print(f"  {'PASS' if ok else 'FAIL'}  {name}{('  — ' + str(detail)) if detail else ''}", flush=True)
def succ(e): return 1 if e >= 255 else e + 1
def blank(host): return all(t["content"]["kind"] == "blank" for t in status(host)["tubes"] if t["populated"])

def main():
    ap = argparse.ArgumentParser(); ap.add_argument("host"); ap.add_argument("--tube", type=int, default=1); a = ap.parse_args(); host, tube = a.host, a.tube
    st0 = status(host); print(f"clock {host}: build {st0.get('build')} mode {st0.get('mode')} proto {st0.get('stream', {}).get('proto')} epoch {st0.get('stream', {}).get('epoch')}")
    A = ws_connect(host); B = ws_connect(host)
    red = bytes([0xF8, 0x00]) * (W * H); green = bytes([0x07, 0xE0]) * (W * H)

    # legacy client: byte 3 of the ack stays 0
    ws_send(A, 2, struct.pack("<BBBBHHHHBBHI", 0xE5, 0x7B, 3, 0, 0, 0, 0, 0, 0, 7, 0, 0)); d = ack_of(A, 7)
    check("legacy PING: ack byte 3 = 0", d is not None and d[2] == 0 and d[3] == 0, d.hex() if d else None)
    ws_send(A, 2, hdr(3, seq=8, w=0, h=0)); d = ack_of(A, 8); e = d[3] if d else 0
    check("1.1 PING returns the node's epoch", d is not None and e >= 1 and e == status(host)["stream"]["epoch"], f"epoch {e}")

    # stale and successor generations
    stale = (e - 3) % 255 or 255
    ws_send(A, 2, hdr(1, tube, seq=9, gen=stale, ln=len(red)) + red); d = ack_of(A, 9, 8)
    check("stale gen: skipped, status 4, ack carries the true epoch", d is not None and d[2] == 4 and d[3] == e, d.hex() if d else None)
    ws_send(A, 2, hdr(1, tube, seq=10, gen=succ(e), ln=len(green)) + green); d = ack_of(A, 10, 8)
    check("successor gen: draws and advances the epoch", d is not None and d[2] == 0 and d[3] == succ(e), d.hex() if d else None); e = d[3] if d else e

    # a corrupt length must not wedge the parser: status 3 at once, and the very next frame works
    ws_send(A, 2, struct.pack("<BBBBHHHHBBBBI", 0xE5, 0x7B, 1, tube, 0, 0, W, H, 0, 11, e, F_V11, 0xFFFFFFF0)); d = ack_of(A, 11, 3)
    ws_send(A, 2, hdr(3, seq=12, w=0, h=0)); d2 = ack_of(A, 12, 3)
    check("insane length: status 3 immediately, parser not wedged", d is not None and d[2] == 3 and d2 is not None, (d.hex() if d else None, d2.hex() if d2 else None))
    ws_send(A, 2, hdr(1, tube, seq=13, gen=e, ln=100) + bytes(100)); d = ack_of(A, 13, 3)
    check("wrong-but-bounded length: skipped with status 3", d is not None and d[2] == 3, d.hex() if d else None)

    # THE race: a slow-drip RAW565 tile on socket A, CANCEL+clear on socket B while it is arriving
    ws_send(A, 2, hdr(1, tube, seq=20, gen=e, ln=len(red)) + red[:W * 2 * 80])        # first 80 rows only
    time.sleep(0.4); t0 = time.time()
    ws_send(B, 2, hdr(6, 0xFF, seq=21, gen=e, flags=F_V11 | F_CLEAR, w=0, h=0)); d = ack_of(B, 21, 4); t_cancel = (time.time() - t0) * 1000
    check("CANCEL+clear on another socket: acked, epoch advanced", d is not None and d[2] == 0 and d[3] == succ(e), f"{t_cancel:.0f} ms · {d.hex() if d else None}")
    ws_send(A, 2, red[W * 2 * 80:])                                                   # the rest of the stale tile arrives AFTER the cancel
    d = ack_of(A, 20, 6)
    check("the frame that was mid-flight is abandoned (status 4), not drawn over the clear", d is not None and d[2] == 4, d.hex() if d else None)
    time.sleep(0.3); check("glass is blank after CANCEL+clear", blank(host)); e = status(host)["stream"]["epoch"]

    # same race with REST /tubes/clear
    ws_send(A, 2, hdr(1, tube, seq=30, gen=e, ln=len(green)) + green[:W * 2 * 80]); time.sleep(0.4)
    r = json.loads(post(host, "/tubes/clear")); ws_send(A, 2, green[W * 2 * 80:]); d = ack_of(A, 30, 6)
    check("REST /tubes/clear beats an in-flight frame too", d is not None and d[2] == 4 and r.get("epoch") == succ(e), (d.hex() if d else None, r))
    time.sleep(0.3); check("glass is blank after REST clear", blank(host)); e = status(host)["stream"]["epoch"]

    # two writers: a legacy one (gen 0) and a 1.1 one on the same tube never starve each other
    okA = okB = 0
    for k in range(6):
        ws_send(A, 2, struct.pack("<BBBBHHHHBBHI", 0xE5, 0x7B, 1, tube, 0, 0, W, 16, 0, 40 + k, 0, W * 16 * 2) + red[:W * 16 * 2]); dA = ack_of(A, 40 + k, 4)
        ws_send(B, 2, hdr(1, tube, y=32, h=16, seq=60 + k, gen=e, ln=W * 16 * 2) + green[:W * 16 * 2]); dB = ack_of(B, 60 + k, 4)
        okA += dA is not None and dA[2] == 0; okB += dB is not None and dB[2] == 0
    check("legacy + 1.1 writers interleaved: nobody starves", okA == 6 and okB == 6, f"{okA}/6 legacy, {okB}/6 v1.1")

    # an aborted REST pixel upload used to keep the panel lock forever: streams must still draw afterwards
    try:
        s = socket.create_connection((host, 80), timeout=5); bnd = "xXx"
        body_head = (f"--{bnd}\r\nContent-Disposition: form-data; name=\"file\"; filename=\"f.565\"\r\nContent-Type: application/octet-stream\r\n\r\n").encode()
        total = len(body_head) + 64800 + len(f"\r\n--{bnd}--\r\n")
        s.sendall((f"POST /tube/{tube}/raw565 HTTP/1.1\r\nHost: {host}\r\nContent-Type: multipart/form-data; boundary={bnd}\r\nContent-Length: {total}\r\n\r\n").encode() + body_head + red[:20000])
        time.sleep(0.5); s.close()                                                     # hang up mid-body
    except Exception as ex: print("   (abort injection failed:", ex, ")")
    time.sleep(1.5); ws_send(B, 2, hdr(1, tube, y=64, h=16, seq=80, gen=e, ln=W * 16 * 2) + green[:W * 16 * 2]); d = ack_of(B, 80, 6)
    check("after an ABORTED REST upload the stream still draws (no leaked panel lock)", d is not None and d[2] == 0, d.hex() if d else None)

    # NOACK silences success only
    ws_send(B, 2, hdr(1, tube, y=96, h=16, seq=81, gen=e, flags=F_V11 | F_NOACK, ln=W * 16 * 2) + green[:W * 16 * 2]); d = ack_of(B, 81, 0.8)
    ws_send(B, 2, hdr(1, tube, y=96, h=16, seq=82, gen=stale, flags=F_V11 | F_NOACK, ln=W * 16 * 2) + green[:W * 16 * 2]); d2 = ack_of(B, 82, 2)
    check("NOACK: no ack on success, errors still acked", d is None and d2 is not None and d2[2] == 4, (d, d2.hex() if d2 else None))

    # shell text over WebSocket now runs in loop() and still answers
    ws_send(A, 1, b"uart"); op, d = ws_recv(A, 3); check("WebSocket shell command answered via the loop() mailbox", op == 1 and b'"baud"' in d, d[:60])
    st1 = status(host)["stream"]; print(f"    stream counters: superseded {st1.get('superseded')} stalled {st1.get('stalled')} bad {st1.get('bad')} uart {st1.get('uart')}")
    post(host, "/mode/clock"); A.close(); B.close()
    print("ALL PASS" if ok_all else "SOME FAILED"); return 0 if ok_all else 1
if __name__ == "__main__": sys.exit(main())
