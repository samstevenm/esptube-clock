#!/usr/bin/env python3
"""ESP/1.1 over the USB cable: COBS datagrams, epoch, CANCEL, ECHO, windowed NOACK band streaming.

  test/esp1-dgram.py /dev/cu.usbserial-XXXX [--baud 1000000] [--seconds 10] [--tubes 1,2,3,4,5]

Needs pyserial (PlatformIO's python has it). Prints PASS/FAIL per check and a speed line.
"""
import sys, time, json, struct, argparse, threading, os, random
import serial

HDR = "<BBBBHHHHBBBBI"
F_NOACK, F_CLEAR, F_V11 = 0x01, 0x04, 0x80
def crc16(b, crc=0xFFFF):
    for x in b:
        crc ^= x << 8
        for _ in range(8): crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc
def cobs_enc(b):
    out = bytearray([0]); code_at = 0; code = 1
    for x in b:
        if x == 0: out[code_at] = code; code_at = len(out); out.append(0); code = 1
        else:
            out.append(x); code += 1
            if code == 0xFF: out[code_at] = code; code_at = len(out); out.append(0); code = 1
    out[code_at] = code; return bytes(out)
def cobs_dec(b):
    out = bytearray(); i = 0
    while i < len(b):
        c = b[i]; i += 1
        if c == 0: return None
        if i + c - 1 > len(b): return None
        out += b[i:i + c - 1]; i += c - 1
        if c != 0xFF and i < len(b): out.append(0)
    return bytes(out)
def frame(op, tube=0, payload=b"", x=0, y=0, w=135, h=240, fmt=0, seq=0, gen=0, flags=F_V11):
    return struct.pack(HDR, 0xE5, 0x7B, op, tube, x, y, w, h, fmt, seq, gen, flags, len(payload)) + payload
def dgram(fr, opener=True, closer=True):
    raw = fr + struct.pack(">H", crc16(fr))
    return (b"\0" if opener else b"") + cobs_enc(raw) + (b"\0" if closer else b"")

class Link:
    def __init__(s, port):
        s.ser = serial.Serial(); s.ser.port = port; s.ser.baudrate = 115200; s.ser.timeout = 0.01
        s.ser.dtr = False; s.ser.rts = False; s.ser.open()
        s.replies = []; s.text = bytearray(); s.lock = threading.Lock(); s.run = True; s.buf = bytearray(); s.legacy = bytearray(); s.paused = False; s.parked = False   # legacy = the RAW byte log (legacy acks contain 0x00)
        threading.Thread(target=s._rx, daemon=True).start()
    def _rx(s):
        while s.run:
            if s.paused: s.parked = True; time.sleep(0.005); continue
            s.parked = False
            try: d = s.ser.read(4096)
            except Exception: time.sleep(0.01); continue
            if not d: continue
            with s.lock:
                s.buf += d; s.legacy += d
                while True:
                    z = s.buf.find(b"\0")
                    if z < 0: s.text += s.buf; s.buf.clear(); break
                    seg = bytes(s.buf[:z]); del s.buf[:z + 1]
                    dec = cobs_dec(seg) if len(seg) >= 7 else None
                    if dec and len(dec) >= 6 and dec[0] in (0xA5, 0xA7) and crc16(dec[:-2]) == struct.unpack(">H", dec[-2:])[0]:
                        s.replies.append((time.time(), dec[:-2]))
                    else: s.text += seg
    def take(s, pred, timeout):
        end = time.time() + timeout
        while time.time() < end:
            with s.lock:
                for i, (t, r) in enumerate(s.replies):
                    if pred(r): del s.replies[i]; return t, r
            time.sleep(0.001)
        return None, None
    def set_baud(s, rate):      # never reconfigure the port while the reader thread is inside read()
        s.paused = True
        while not s.parked: time.sleep(0.002)
        s.ser.baudrate = rate; time.sleep(0.15)
        with s.lock: s.buf.clear(); s.text.clear()
        s.paused = False
    def ack(s, seq, timeout=2.0): return s.take(lambda r: r[0] == 0xA5 and r[1] == seq, timeout)
    def cmd(s, line, wait=0.8, until=None):
        with s.lock: s.text.clear()
        s.ser.write(line.encode() + b"\n"); s.ser.flush(); end = time.time() + wait
        while time.time() < end:
            with s.lock:
                if until and until in s.text: break
            time.sleep(0.01)
        with s.lock: return bytes(s.text)
    def alive(s): return b"commands:" in s.cmd("help", 1.5, b"binary:")
    def uart(s):
        r = s.cmd("uart", 0.8, b"}")
        try: a = r.index(b"{"); return json.loads(r[a:r.index(b"}", a) + 1])
        except Exception: return {}

ok_all = True
def check(name, ok, detail=""):
    global ok_all; ok_all &= bool(ok); print(f"  {'PASS' if ok else 'FAIL'}  {name}{('  — ' + detail) if detail else ''}", flush=True)

def pal4x2_band(k, first_row, rows):   # rows = payload rows (each drawn twice)
    pal = b"".join(struct.pack(">H", (((i * 2) & 31) << 11) | (((i * 4) & 63) << 5) | ((31 - i * 2) & 31)) for i in range(16))
    body = bytearray()
    for y in range(first_row, first_row + rows):
        for xb in range(68):
            a = (xb + y + k) & 15; b = (xb + y + k + 1) & 15
            body.append((a << 4) | (b if xb < 67 else 0))
    return pal + bytes(body)

def main():
    ap = argparse.ArgumentParser(); ap.add_argument("port"); ap.add_argument("--baud", type=int, default=1000000)
    ap.add_argument("--ip", default="", help="the clock's LAN address: verify the glass state over REST too"); ap.add_argument("--seconds", type=float, default=10); ap.add_argument("--tubes", default="1,2,3,4,5"); ap.add_argument("--fifo", type=int, default=0)
    a = ap.parse_args(); tubes = [int(t) for t in a.tubes.split(",")]
    l = Link(a.port); print(f"opening {a.port} ...", flush=True)
    t0 = time.time()
    while not l.alive():
        if time.time() - t0 > 16: print("shell does not answer at 115200"); return 2
        time.sleep(0.5)
    if a.baud != 115200:
        r = l.cmd(f"baud {a.baud}", 0.5, b"OK baud"); time.sleep(0.12); l.set_baud(a.baud)
        check(f"switch to {a.baud}", any(l.alive() for _ in range(3)))     # the first line after a rate switch can straddle it
    if a.fifo: print("   ", l.cmd(f"uart fifo {a.fifo}", 0.6, b"}").decode(errors="replace").strip())
    u0 = l.uart(); print("    uart:", u0, flush=True)

    # 1. legacy raw PING still gets the old 4-byte ack with byte 3 = 0
    with l.lock: l.legacy.clear()
    l.ser.write(struct.pack("<BBBBHHHHBBHI", 0xE5, 0x7B, 3, 0, 0, 0, 0, 0, 0, 77, 0, 0)); l.ser.flush(); time.sleep(0.25)
    with l.lock: lg = bytes(l.legacy)
    i = lg.find(b"\xA5\x4D"); check("legacy PING: raw ack, byte 3 = 0", i >= 0 and len(lg) >= i + 4 and lg[i + 2] == 0 and lg[i + 3] == 0, lg[-12:].hex())

    # 2. datagram PING returns the epoch
    l.ser.write(dgram(frame(3, seq=1))); t, r = l.ack(1); epoch = r[3] if r else 0
    check("datagram PING -> framed ack with the epoch", bool(r) and r[2] == 0 and epoch >= 1, f"epoch {epoch}")

    # 3. ECHO: 48 x 1400 random bytes, node-side CRC must match
    bad = 0; tE = time.time(); nbytes = 0
    for k in range(48):
        pay = os.urandom(1400); seq = (k + 2) & 255
        l.ser.write(dgram(frame(7, payload=pay, seq=seq, w=0, h=0))); nbytes += 1400
        t, r = l.take(lambda r: r[0] == 0xA7 and r[1] == seq, 2.0)
        if not r or (r[2] << 8 | r[3]) != crc16(pay) or (r[4] | r[5] << 8) != 1400: bad += 1
    check("ECHO 67 KB host->node integrity", bad == 0, f"{bad} of 48 wrong · {nbytes / (time.time() - tE) / 1024:.0f} KB/s stop-and-wait")

    # 4. stale / successor generations
    stale = (epoch - 2) % 255 or 255
    l.ser.write(dgram(frame(1, tubes[0], pal4x2_band(0, 0, 16), y=0, h=32, fmt=3, seq=60, gen=stale))); t, r = l.ack(60)
    check("stale gen -> status 4 + true epoch", bool(r) and r[2] == 4 and r[3] == epoch, f"{r.hex() if r else None}")
    succ = 1 if epoch >= 255 else epoch + 1
    l.ser.write(dgram(frame(1, tubes[0], pal4x2_band(0, 0, 16), y=0, h=32, fmt=3, seq=61, gen=succ))); t, r = l.ack(61)
    check("successor gen -> draws and advances the epoch", bool(r) and r[2] == 0 and r[3] == succ, f"{r.hex() if r else None}"); epoch = r[3] if r else epoch

    # 5. lost delimiters heal within one datagram
    l.ser.write(dgram(frame(3, seq=70), opener=False)); l.ser.write(dgram(frame(3, seq=71))); t, r = l.ack(71, 1.0); check("lost opener: the next datagram is fine", bool(r))
    l.ser.write(dgram(frame(3, seq=72), closer=False)); l.ser.write(dgram(frame(3, seq=73))); l.ser.write(dgram(frame(3, seq=74)))
    t72, r72 = l.ack(72, 1.0); t74, r74 = l.ack(74, 1.0); check("lost closer: that one and the one after next are fine", bool(r72) and bool(r74))
    junk = bytearray(dgram(frame(3, seq=75))); junk[9] ^= 0x20; l.ser.write(bytes(junk)); t, r = l.take(lambda r: r[0] == 0xA5 and r[2] in (3, 5), 1.0)
    check("corrupt datagram -> refused (status 5 bad CRC, or 3 when the COBS structure broke), not drawn", bool(r) and r[2] in (3, 5), f"{r.hex() if r else None}")

    # 6. windowed NOACK band streaming: 8 bands per tile, only the last one acked, <= 2 tiles in flight
    u0 = l.uart()
    sent = acked = lost = 0; nb = 0; seq = 100; inflight = {}; k = 0; tS = time.time(); ackms = []
    while time.time() - tS < a.seconds:
        tube = tubes[k % len(tubes)]; k += 1
        for bi in range(8):
            rows = 16 if bi < 7 else 8; last = bi == 7
            fr = frame(1, tube, pal4x2_band(k, bi * 16, rows), y=bi * 32, h=rows * 2, fmt=3, seq=seq if last else 0, gen=epoch, flags=F_V11 | (0 if last else F_NOACK))
            d = dgram(fr); l.ser.write(d); nb += len(d)
        inflight[seq] = time.time(); sent += 1; seq = 100 + (seq - 99) % 150
        while len(inflight) >= 2:
            t, r = l.take(lambda r: r[0] == 0xA5 and r[1] in inflight, 2.5)
            if not r: lost += len(inflight); inflight.clear(); break
            ackms.append((t - inflight.pop(r[1])) * 1000); acked += (r[2] == 0); 
            if r[2] == 4: epoch = r[3]
    while inflight:
        t, r = l.take(lambda r: r[0] == 0xA5 and r[1] in inflight, 2.5)
        if not r: lost += len(inflight); break
        inflight.pop(r[1]); acked += (r[2] == 0)
    dt = time.time() - tS; u1 = l.uart()
    d = {k2: u1.get(k2, 0) - u0.get(k2, 0) for k2 in ("crc_bad", "resync", "overrun", "junk")}
    print(f"    stream: {nb / dt / 1024:.1f} KB/s · {acked / dt:.1f} tile-updates/s · ack {sum(ackms) / max(1, len(ackms)):.0f} ms · sent {sent} acked {acked} lost {lost} · counters {d}", flush=True)
    check("band streaming: nothing lost, no CRC errors, no overruns", lost == 0 and acked == sent and d["crc_bad"] == 0 and d["overrun"] == 0, str(d))

    # 7. cancel latency: a RAW565 tile in flight as NOACK bands, CANCEL+clear cuts in
    raw_band = bytes([0xF8, 0x00]) * (135 * 5)                                # 5 red rows = 1350 bytes
    for bi in range(20): l.ser.write(dgram(frame(1, tubes[0], raw_band, y=bi * 5, h=5, fmt=0, seq=0, gen=epoch, flags=F_V11 | F_NOACK)))
    tc = time.time(); l.ser.write(dgram(frame(6, 0xFF, seq=90, gen=epoch, flags=F_V11 | F_CLEAR, w=0, h=0))); l.ser.flush()
    for bi in range(20, 40): l.ser.write(dgram(frame(1, tubes[0], raw_band, y=bi * 5, h=5, fmt=0, seq=(200 + bi) if bi == 39 else 0, gen=epoch, flags=F_V11 | (0 if bi == 39 else F_NOACK))))
    t, r = l.ack(90, 3.0); newep = r[3] if r else 0
    check("CANCEL+clear acked, epoch advanced", bool(r) and r[2] == 0 and newep != epoch, f"{(t - tc) * 1000:.0f} ms from write to ack (20 bands were queued ahead of it)" if r else "no ack")
    t, r = l.ack(239, 3.0); check("bands of the old epoch sent AFTER the cancel are refused (status 4)", bool(r) and r[2] == 4, f"{r.hex() if r else None}")
    if a.ip:
        import urllib.request
        try: js = json.load(urllib.request.urlopen(f"http://{a.ip}/status", timeout=4)); blank = all(tb["content"]["kind"] == "blank" for tb in js["tubes"] if tb["populated"])
        except Exception as e: js = {"err": str(e)}; blank = False
        check("glass is blank after CANCEL+clear (REST /status)", blank, f"mode {js.get('mode')} epoch {js.get('stream', {}).get('epoch')} superseded {js.get('stream', {}).get('superseded')}")
    l.ser.write(dgram(frame(3, seq=91))); t1 = time.time(); t, r = l.ack(91, 1.0)
    check("idle CANCEL-path round trip", bool(r), f"{(t - t1) * 1000:.0f} ms" if r else "")

    check("shell still alive", l.alive())
    if a.baud != 115200: l.cmd("baud 115200", 0.5, b"OK baud"); time.sleep(0.12); l.set_baud(115200); l.alive()
    l.cmd("mode clock"); l.run = False
    print("ALL PASS" if ok_all else "SOME FAILED"); return 0 if ok_all else 1
if __name__ == "__main__": sys.exit(main())
