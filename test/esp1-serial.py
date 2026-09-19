#!/usr/bin/env python3
"""esp1-serial.py — exercise the USB UART: text shell + ESP/1 frames over serial.

Needs pyserial. The system python lacks it; use esptool's interpreter:
  /opt/homebrew/Cellar/esptool/*/libexec/bin/python3 test/esp1-serial.py [/dev/cu.usbserial-XXXX] [--baud 921600]

1. Shell: help · status · fps · wifi (expects text replies).
2. `baud N`: switches the node's UART speed, reopens the port at N.
3. Frames: PING ×20 (round trip), then PAL4 full-panel blits to every populated
   tube for --seconds, counting acks — the wired frame rate.
4. Restores 115200 and puts the clock back on its face.
"""
import argparse, glob, json, struct, sys, time
try:
    import serial
except ImportError:
    sys.exit("pyserial missing — run with /opt/homebrew/Cellar/esptool/*/libexec/bin/python3")

W, H = 135, 240

def header(op, tube, x, y, w, h, fmt, seq, ln):
    return struct.pack("<BBBBHHHHBBHI", 0xE5, 0x7B, op, tube, x, y, w, h, fmt, seq, 0, ln)

def pal4_frame(k, tube):
    """16-colour test tile: diagonal stripes shifting with k, unique hue per tube (no Pillow needed)."""
    pal = [((i * 2) << 11) | ((i * 4 + tube * 8) & 0x3F) << 5 | (i * 2) for i in range(16)]
    out = bytearray(b"".join(struct.pack(">H", c) for c in pal))
    rowb = (W + 1) // 2
    for y in range(H):
        row = bytearray(rowb)
        for x in range(W):
            v = ((x + y + k * 6) // 12) & 15
            if x & 1: row[x >> 1] |= v
            else:     row[x >> 1] = v << 4
        out += row
    return bytes(out)

def open_port(port, baud):
    """Open WITHOUT asserting DTR/RTS: the board's auto-reset circuit (the one esptool
    uses) would otherwise reboot the ESP32 every time we reopen at a new baud."""
    s = serial.Serial(); s.port = port; s.baudrate = baud; s.timeout = 1
    s.dtr = False; s.rts = False; s.open(); s.dtr = False; s.rts = False
    time.sleep(0.2); s.reset_input_buffer(); return s

def line(ser, cmd, wait=0.6):
    ser.reset_input_buffer(); ser.write((cmd + "\n").encode()); time.sleep(wait)
    out = ser.read(ser.in_waiting or 1).decode("utf-8", "replace")
    print(f"> {cmd}\n{out.rstrip()}")
    return out

def read_ack(ser, timeout=3.0):
    ser.timeout = timeout
    while True:
        b = ser.read(1)
        if not b: return None
        if b[0] == 0xA5:
            rest = ser.read(3)
            if len(rest) == 3: return rest[0], rest[1]
            return None

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port", nargs="?", default=(glob.glob("/dev/cu.usbserial-*") or [None])[0])
    ap.add_argument("--baud", type=int, default=921600)
    ap.add_argument("--seconds", type=float, default=6)
    a = ap.parse_args()
    if not a.port: sys.exit("no /dev/cu.usbserial-* port")
    print("port", a.port)
    ser = open_port(a.port, 115200)

    # 1. shell
    line(ser, "help"); st = line(ser, "status"); line(ser, "fps"); line(ser, "wifi")
    tubes = [1, 2, 3, 4, 5]
    try:
        js = json.loads(st.strip().splitlines()[-1])
        print("shell status ok:", js.get("mode"), js.get("preset"), js.get("ip"))
    except Exception:
        print("(status line not JSON — continuing)")

    # 2. baud switch
    if a.baud != 115200:
        # change speed on the OPEN port — closing/reopening pulses DTR and reboots the board
        ser.write(f"baud {a.baud}\n".encode()); time.sleep(0.3)
        ser.baudrate = a.baud; time.sleep(0.2); ser.reset_input_buffer()
        line(ser, "fps")

    # 3. frames: pings then PAL4 blits
    lat = []
    for i in range(20):
        t = time.monotonic(); ser.write(header(3, 0, 0, 0, 0, 0, 0, i, 0)); r = read_ack(ser)
        if r: lat.append(time.monotonic() - t)
    if lat: lat.sort(); print(f"PING over UART @{a.baud}: median {lat[len(lat)//2]*1000:.1f} ms · max {lat[-1]*1000:.1f} ms · {len(lat)}/20 acked")
    ring = {t: [pal4_frame(k, t) for k in range(6)] for t in tubes}
    sent = acked = nbytes = 0; k = 0; t0 = time.monotonic(); bad = 0
    while time.monotonic() - t0 < a.seconds:
        k += 1
        for t in tubes:
            p = ring[t][k % 6]
            ser.write(header(1, t, 0, 0, W, H, 2, sent & 0xFF, len(p)) + p); sent += 1; nbytes += 20 + len(p)
            r = read_ack(ser, 5.0)
            if r is None: print("ack timeout"); break
            acked += 1
            if r[1] != 0: bad += 1
    el = time.monotonic() - t0
    print(f"PAL4 over UART @{a.baud}: {acked/el:5.1f} tube-updates/s = {acked/el/len(tubes):4.1f} fps × {len(tubes)} · {nbytes/el/1024:5.1f} KB/s · bad {bad}")

    # 4. restore
    ser.write(b"baud 115200\n"); time.sleep(0.3); ser.baudrate = 115200; time.sleep(0.2); ser.reset_input_buffer()
    line(ser, "preset 0"); ser.close()

if __name__ == "__main__":
    main()
