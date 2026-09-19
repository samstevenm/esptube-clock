# 02 — Connecting to the Clock

Two connection tracks, in the order we'll use them:

1. **USB serial** — required first, to flash firmware and read the boot log.
2. **Network (WiFi)** — the end goal: shell-like access, virtual buttons, per-tube widgets,
   no cable.

---

## Track 1 — USB serial (for flashing)

### Current status (2026-09-14)
Probed this Mac while (presumably) the clock was not connected / not on a data link:

```
/dev/cu.*  → only Bluetooth serial ports (BLT-HD, Transit350). No USB serial.
system_profiler SPUSBDataType → no CP210x / CH340 / FTDI / ESP32 USB-UART bridge found.
```

So macOS is **not seeing the clock as a serial device.** That matches "USB not recognized."

### Diagnosing why (work through in order)

1. **Is it actually plugged into the Mac and powered?** Plug the clock's USB into the Mac
   directly (not a hub), then re-run the probe:
   ```bash
   ls /dev/cu.* /dev/tty.*
   system_profiler SPUSBDataType | grep -iE "CP210|CH340|FTDI|Espressif|serial|UART"
   ```
   If a `/dev/cu.usbserial-*` or `/dev/cu.usbmodem*` appears → we're in business.

2. **Cable.** The #1 cause of "not recognized." Many cables are charge-only. Use a known
   **data** cable (ideally the one that came with a phone/SSD you know does data). Swap it
   before anything else.

3. **Is the USB port even a data port?** Some clocks in this family power over USB/DC but
   expose **programming only via UART pads/header** on the board. If, with a known-good data
   cable, still nothing enumerates *and* the board has **no CP2102/CH340 bridge chip** near
   the USB connector, then the port is power-only and we flash via UART pads (step 5).

4. **Driver (only if a device shows in USB tree but no `/dev/cu` node appears).**
   - CP210x (Silicon Labs): install the macOS VCP driver.
   - CH340/CH341 (WCH): install the macOS CH34x driver.
   Recent macOS often needs no driver for CP210x; CH340 clones sometimes do.

5. **Flash via UART pads (fallback if USB is power-only or the bridge is dead).**
   Use an external **3.3 V USB-to-UART adapter** (CP2102/CH340 dongle). Wire:
   `adapter GND→board GND`, `adapter TX→board RX`, `adapter RX→board TX`,
   (do **not** feed 5 V into a 3.3 V logic pin). To enter download mode on ESP32:
   hold **IO0/BOOT low**, pulse **EN/RST**, release. Then `esptool` can talk to it.
   > We'll confirm the exact pad locations from a clear PCB photo before wiring anything.

### Once serial works — read before you write
Before overwriting the stock firmware, **back it up** so we can always return to it:
```bash
# with esptool installed (pip install esptool), port name from step 1:
esptool.py --port /dev/cu.usbserial-XXXX --baud 921600 read_flash 0x0 0x400000 stock-backup.bin
```
Save `stock-backup.bin` into `firmware/`. Also capture the **boot serial log** (115200 baud
is the usual console rate) — it often reveals the exact firmware/model.

---

## Track 2 — Network access (the end goal)

You flagged the real constraint: when the Mac joins the **clock's own WiFi AP**, the Mac
loses internet, so Claude/tools break. Two ways around it:

### Option A (preferred) — put the clock on your home WiFi (station mode)
Custom firmware joins your existing WiFi as a client. Then the Mac stays on home WiFi with
internet **and** can reach the clock on the same LAN (by IP or an mDNS hostname like
`esptube.local`). No tethering, no AP juggling. This is what we'll target in firmware.

### Option B — stay on the clock's AP, tether the Mac's internet over iPhone USB
If we must be on the clock's own AP (e.g. before it's configured for home WiFi):
- iPhone **Personal Hotspot**, connected to the Mac by **USB (Lightning/USB-C)**.
- macOS routes internet over the USB (`iPhone USB` interface) while the Mac's **Wi-Fi**
  adapter is joined to the clock's AP.
- Set the **service order** so `iPhone USB` is above `Wi-Fi` for the default route, so
  Claude/tools keep internet while Wi-Fi talks to the clock. (This is exactly the workflow
  you described — it works.)

### What "shell access + virtual buttons + per-tube widgets" looks like
These are firmware features we'll add (custom firmware, built on the EleksTubeHAX baseline):
- **Shell-like REPL over the network** — a telnet/WebSocket/HTTP endpoint that accepts
  commands (set time, set a tube's content, dump status, reboot, etc.).
- **Virtual buttons** — commands that inject the same events the physical buttons do, so the
  stock menu (or our menu) is fully drivable remotely.
- **Per-tube widget push** — an API where a script sends `{tube: 5, render: <text/image/value>}`
  and that panel updates. Graceful-degradation: tubes that are missing/dead are skipped, the
  rest keep working.

We'll design this control surface once the hardware baseline is proven.

---

## Immediate next step
Plug the clock into the Mac with a **known-good data cable**, tell me it's connected, and
I'll re-probe. In parallel: scan for the clock's **WiFi SSID** and note it — that name is a
fast clue to the exact model.
