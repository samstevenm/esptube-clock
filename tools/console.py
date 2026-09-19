#!/usr/bin/env python3
"""console.py — open the ESPTube console the way a browser trusts it:  http://localhost:8765

Camera, screen and tab capture (getUserMedia / getDisplayMedia) only exist on a SECURE page:
file://, https://, or localhost. The copy the clock hosts (http://esptube.local/helper) is plain
http on a LAN name, so there the browser does not even define navigator.mediaDevices. This serves
tools/helper.html and samples/ from localhost — a secure context, and ONE stable origin, so your
scenes and layout don't depend on which folder the file was opened from. Standard library only.

  python3 tools/console.py                 # serve and open the browser
  python3 tools/console.py <clock-ip>  # ... and point the console at that clock
  python3 tools/console.py --no-open --port 8765
"""
import argparse, os, sys, threading, webbrowser
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))     # the repo root (tools/..)
PORT = 8765

class H(SimpleHTTPRequestHandler):
    def __init__(self, *a, **k):
        super().__init__(*a, directory=ROOT, **k)
    def log_message(self, fmt, *args):
        pass
    def end_headers(self):
        self.send_header("Cache-Control", "no-store")                  # always the helper you just edited
        super().end_headers()
    def translate_path(self, path):
        p = path.split("?", 1)[0].split("#", 1)[0]
        if p in ("/", "/helper", "/helper/", "/index.html"):
            return os.path.join(ROOT, "tools", "helper.html")
        if p == "/icon.png":
            return os.path.join(ROOT, "firmware", "custom-fw", "data", "icon.png")
        if p == "/tools/helper.html":
            return os.path.join(ROOT, "tools", "helper.html")
        if not p.startswith("/samples/") or ".." in p:
            return os.path.join(ROOT, "__nope__")                      # nothing else: the working copy also holds private notes and photos
        return super().translate_path(path)

def serve(port=PORT, quiet=False):
    """Start the console server on 127.0.0.1:port in this thread. Raises OSError if the port is taken."""
    srv = ThreadingHTTPServer(("127.0.0.1", port), H); srv.daemon_threads = True
    if not quiet: print(f"ESPTube console: http://localhost:{port}/   (secure context: camera / screen capture work here)")
    srv.serve_forever()

def serve_in_background(port=PORT):
    """For the bridge: bring the console up next to the control API; False if something already owns the port."""
    try:
        srv = ThreadingHTTPServer(("127.0.0.1", port), H); srv.daemon_threads = True
    except OSError:
        return False
    threading.Thread(target=srv.serve_forever, daemon=True).start(); return True

if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("clock", nargs="?", default="", help="the clock's IP or name, e.g. <clock-ip> or esptube.local")
    ap.add_argument("--port", type=int, default=PORT); ap.add_argument("--no-open", action="store_true")
    a = ap.parse_args()
    url = f"http://localhost:{a.port}/" + (f"?clock={a.clock}" if a.clock else "")
    if not a.no_open: threading.Timer(0.6, lambda: webbrowser.open(url)).start()
    try: serve(a.port)
    except OSError as e: sys.exit(f"port {a.port}: {e} — is the console (or the bridge) already running? Just open {url}")
    except KeyboardInterrupt: pass
