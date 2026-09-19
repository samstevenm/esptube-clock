#!/usr/bin/env python3
# serial-capture.py — capture the clock's serial console for a fixed window, then exit.
#
# Why this exists: the CH340 on this board resets the ESP32 when the port is opened (DTR),
# and it occasionally re-enumerates mid-session ("Errno 6: Device not configured"). This tool
# captures FROM the boot banner, tolerates the re-enumeration by reopening, and stops after
# `seconds` so it never wedges a port. Use it to watch [btn]/[menu]/[net] logs while pressing
# the physical buttons, or to confirm a fresh flash booted.
#
# Usage:
#   python3 tools/serial-capture.py [PORT] [SECONDS]
#   PORT defaults to the first /dev/cu.usbserial-* ; SECONDS defaults to 90.
# Needs pyserial — PlatformIO ships it: `pio system info | grep Python` gives the interpreter.
import sys, glob, time

try:
    import serial
except Exception as e:
    print("need pyserial (run with PlatformIO's python):", e); sys.exit(2)

def default_port():
    for pat in ("/dev/cu.usbserial-*", "/dev/cu.wchusbserial*", "/dev/ttyUSB*"):
        m = sorted(glob.glob(pat))
        if m: return m[0]
    return None

port = sys.argv[1] if len(sys.argv) > 1 else default_port()
dur  = float(sys.argv[2]) if len(sys.argv) > 2 else 90
if not port:
    print("no serial port found"); sys.exit(1)

t0 = time.time(); s = None; reopens = 0
while time.time() - t0 < dur:
    try:
        if s is None:
            s = serial.Serial(port, 115200, timeout=0.3); time.sleep(0.8)   # opening resets the board (DTR)
            sys.stdout.write("\n[capture: opened %s]\n" % port); sys.stdout.flush()
        d = s.read(400)
        if d:
            sys.stdout.write(d.decode("utf-8", "replace")); sys.stdout.flush()
    except Exception as e:
        try:
            if s: s.close()
        except Exception: pass
        s = None; reopens += 1
        if reopens > 6:
            sys.stdout.write("\n[capture: gave up after %d reopens: %s]\n" % (reopens, e)); break
        time.sleep(0.5)
if s:
    try: s.close()
    except Exception: pass
