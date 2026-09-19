#!/usr/bin/env python3
"""Cable session: which baud rates does THIS cable / bridge chip / ESP32 combination really carry?

  test/serial-baud-probe.py /dev/cu.usbserial-XXXX [--rates 1000000,2000000,...] [--tube 1]

Uses the firmware's safe in-session switch (`baud N`: reverts by itself after 8 s without a clean
command line), so a rate that fails costs ~9 s and never strands the shell. Per rate that answers:
  text   100x `fps` + 20x `status` (JSON must parse) -> both directions carry clean bytes
  blit   10 PAL4x2 full tiles (8.2 KB each) to one tube, ack-timed -> KB/s, ack ms, tile-updates/s
Needs pyserial (PlatformIO ships one: $(head -1 $(which pio) | cut -c3-) ).
"""
import sys, time, json, struct, argparse
import serial

HDR = "<BBBBHHHHBBHI"
def frame(op, tube, payload=b"", x=0, y=0, w=135, h=240, fmt=0, seq=0):
    return struct.pack(HDR, 0xE5, 0x7B, op, tube, x, y, w, h, fmt, seq, 0, len(payload)) + payload

def pal4x2_tile(k):
    pal = b"".join(struct.pack(">H", ((i * 2) << 11) | ((i * 4) << 5) | (31 - i * 2)) for i in range(16))
    rows = bytearray()
    for y in range(120):
        for xb in range(68):
            a = (xb + y + k) & 15; b = (xb + y + k + 1) & 15
            rows.append((a << 4) | (b if xb < 67 else 0))
    return pal + bytes(rows)

class Link:
    def __init__(s, port):
        s.ser = serial.Serial(); s.ser.port = port; s.ser.baudrate = 115200; s.ser.timeout = 0.02
        s.ser.dtr = False; s.ser.rts = False; s.ser.open(); s.rate = 115200
    def cmd(s, line, wait=0.6, until=None):
        s.ser.reset_input_buffer(); s.ser.write(line.encode() + b"\n"); s.ser.flush()
        end = time.time() + wait; buf = bytearray()
        while time.time() < end:
            d = s.ser.read(4096)
            if d:
                buf += d
                if until and until in buf: break
        return bytes(buf)
    def alive(s):
        return b"commands:" in s.cmd("help", 1.2, b"commands:")
    def wait_boot(s, secs=14):
        end = time.time() + secs
        while time.time() < end:
            if s.alive(): return True
            time.sleep(0.4)
        return False
    def switch(s, rate):
        prev = s.rate
        r = s.cmd(f"baud {rate}", 0.5, b"OK baud")
        if b"OK baud" not in r: return False, f"no OK at {prev}: {r[-60:]!r}"
        time.sleep(0.12); s.ser.baudrate = rate; time.sleep(0.12); s.ser.reset_input_buffer()
        if s.alive(): s.rate = rate; return True, ""
        s.ser.baudrate = prev; t0 = time.time()          # let the node's watchdog take it back
        while time.time() - t0 < 12:
            time.sleep(1.0)
            if s.alive(): return False, f"no clean reply at {rate}; node reverted to {prev} after {time.time()-t0:.0f}s"
        return False, f"no clean reply at {rate} AND no revert to {prev}"

def text_test(l):
    ok_fps = 0
    for _ in range(100):
        r = l.cmd("fps", 0.25, b"draw_ms=")
        if r.count(b"fps=") == 1 and b"draw_ms=" in r and all(32 <= c < 127 or c in (10, 13) for c in r): ok_fps += 1
    ok_js = 0
    for _ in range(20):
        r = l.cmd("status", 0.8, b"}\r\n")
        try:
            a = r.index(b"{"); json.loads(r[a:r.rindex(b"}") + 1].decode()); ok_js += 1
        except Exception: pass
    return ok_fps, ok_js

def blit_test(l, tube, n=10):
    l.ser.reset_input_buffer(); acks = []; bad = 0; t0 = time.time(); total = 0
    for k in range(n):
        f = frame(1, tube, pal4x2_tile(k), fmt=3, seq=(k + 1) & 255); t = time.time()
        l.ser.write(f); l.ser.flush(); total += len(f)
        buf = bytearray(); end = time.time() + 3.0; got = None
        while time.time() < end and got is None:
            buf += l.ser.read(64)
            for i in range(len(buf) - 3):
                if buf[i] == 0xA5 and buf[i + 1] == ((k + 1) & 255): got = buf[i + 2]; break
        if got is None or got != 0: bad += 1
        else: acks.append((time.time() - t) * 1000)
    dt = time.time() - t0
    return dict(kbps=round(total / dt / 1024, 1), upd_s=round((n - bad) / dt, 1), ack_ms=round(sum(acks) / len(acks)) if acks else None, bad=bad)

def main():
    ap = argparse.ArgumentParser(); ap.add_argument("port"); ap.add_argument("--rates", default="460800,1000000,2000000,1500000,921600,500000"); ap.add_argument("--tube", type=int, default=1)
    a = ap.parse_args(); l = Link(a.port)
    print(f"opening {a.port} @115200 (DTR/RTS low) ...", flush=True)
    if not l.wait_boot():
        for r in (460800, 230400, 921600):
            l.ser.baudrate = r
            if l.alive(): l.rate = r; break
        else: print("the shell does not answer at any known rate"); return 2
    print(f"shell answers at {l.rate}", flush=True)
    res = {}
    f0, j0 = text_test(l); res[l.rate] = dict(text=f"{f0}/100 fps, {j0}/20 status")
    print(f"  {l.rate:>8}: text {res[l.rate]['text']}", flush=True)
    for rate in [int(x) for x in a.rates.split(",")]:
        ok, why = l.switch(rate)
        if not ok: res[rate] = dict(fail=why); print(f"  {rate:>8}: FAIL — {why}", flush=True); continue
        f, j = text_test(l); b = blit_test(l, a.tube)
        res[rate] = dict(text=f"{f}/100 fps, {j}/20 status", **b)
        print(f"  {rate:>8}: text {f}/100 {j}/20 · blit {b['kbps']} KB/s · {b['upd_s']} tile-upd/s · ack {b['ack_ms']} ms · bad {b['bad']}", flush=True)
    if l.rate != 115200: l.switch(115200)
    l.cmd("mode clock")
    print(json.dumps(res))
    return 0
if __name__ == "__main__": sys.exit(main())
