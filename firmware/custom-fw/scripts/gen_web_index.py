#!/usr/bin/env python3
# PlatformIO PRE-build step: embed data/index.html into src/web_index.h as a
# PROGMEM raw string literal, so the control-panel UI ships inside the ~1 MB app
# image (no ~12 MB LittleFS upload over flaky USB needed). Runs on every build,
# so editing data/index.html is enough — no manual re-paste.
#
# In a pure-Python run this looks broken; Import() is provided by the PIO/SCons
# build environment.
Import("env")  # noqa: F821
import os
import sys

DELIM = "HTMLPAGE"
proj = env.subst("$PROJECT_DIR")  # noqa: F821
src = os.path.join(proj, "data", "index.html")
dst = os.path.join(proj, "src", "web_index.h")

if not os.path.exists(src):
    sys.stderr.write("gen_web_index: %s not found\n" % src)
    env.Exit(1)  # noqa: F821

with open(src, "r", encoding="utf-8") as f:
    html = f.read()

# A raw string literal ends at the first `)DELIM"`. Refuse to emit a broken
# header if that sequence ever appears in the page.
if (")" + DELIM + '"') in html:
    sys.stderr.write("gen_web_index: raw-string delimiter collision in index.html\n")
    env.Exit(1)  # noqa: F821

out = (
    "// AUTO-GENERATED from data/index.html by scripts/gen_web_index.py.\n"
    "// Do not edit by hand — edit data/index.html and rebuild.\n"
    "#pragma once\n"
    "#include <pgmspace.h>\n"
    'static const char INDEX_HTML[] PROGMEM = R"' + DELIM + "(\n"
    + html
    + ")" + DELIM + '";\n'
)

# Only rewrite when changed so timestamps stay stable and the file isn't
# needlessly recompiled every build.
old = None
if os.path.exists(dst):
    with open(dst, "r", encoding="utf-8") as f:
        old = f.read()

if old != out:
    with open(dst, "w", encoding="utf-8") as f:
        f.write(out)
    print("gen_web_index: wrote %s (%d bytes of HTML)" % (dst, len(html)))
else:
    print("gen_web_index: src/web_index.h up to date (%d bytes of HTML)" % len(html))

# ---- the FULL helper (tools/helper.html), gzipped, + the PWA icon: the clock serves them at
# /helper and /icon.png so a phone on the clock's hotspot (or the LAN) gets the whole console
# with nothing to install ("Add to Home Screen" makes it an app). ----
import gzip

def emit_bytes(src_path, dst_path, name, gz):
    if not os.path.exists(src_path):
        sys.stderr.write("gen_web_index: %s not found\n" % src_path); env.Exit(1)  # noqa: F821
    data = open(src_path, "rb").read()
    if gz:
        data = gzip.compress(data, 9, mtime=0)
    body = ",".join(str(b) for b in data)
    text = ("// AUTO-GENERATED from %s by scripts/gen_web_index.py — do not edit.\n#pragma once\n#include <pgmspace.h>\n"
            "static const size_t %s_LEN = %d;\nstatic const uint8_t %s[] PROGMEM = {%s};\n" % (os.path.relpath(src_path, proj), name, len(data), name, body))
    prev = open(dst_path, "r", encoding="utf-8").read() if os.path.exists(dst_path) else None
    if prev != text:
        open(dst_path, "w", encoding="utf-8").write(text)
        print("gen_web_index: wrote %s (%d bytes%s)" % (dst_path, len(data), ", gzip" if gz else ""))

# build stamp in its own tiny header: __DATE__/__TIME__ inside api.cpp only changed when api.cpp itself
# recompiled, so /status.build lied after every OTA that touched other files.
# FW_VERSION is a COMPARABLE version (git short SHA + -dirty) so a script can answer
# "is device X on build N?" — a wall-clock BUILD_STAMP can't (two builds a second apart collide).
import datetime
import subprocess

def _fw_version(project_dir):
    try:
        sha = subprocess.check_output(["git", "-C", project_dir, "rev-parse", "--short", "HEAD"],
                                      stderr=subprocess.DEVNULL).decode().strip()
        dirty = subprocess.call(["git", "-C", project_dir, "diff", "--quiet", "--ignore-submodules"],
                                stderr=subprocess.DEVNULL) != 0
        return (sha + "-dirty") if dirty else sha
    except Exception:
        return "dev"

stamp = datetime.datetime.now().strftime("%b %d %Y %H:%M:%S")
fw = _fw_version(proj)
open(os.path.join(proj, "src", "build_stamp.h"), "w").write(
    '#pragma once\n#define BUILD_STAMP "%s"\n#define FW_VERSION "%s"\n' % (stamp, fw))
print("gen_web_index: build stamp %s, version %s" % (stamp, fw))

emit_bytes(os.path.join(proj, "..", "..", "tools", "helper.html"), os.path.join(proj, "src", "web_helper.h"), "HELPER_GZ", True)
emit_bytes(os.path.join(proj, "data", "icon.png"), os.path.join(proj, "src", "web_icon.h"), "ICON_PNG", False)
