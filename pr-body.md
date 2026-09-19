Adds `.github/workflows/ci.yml`: one `ubuntu-latest` job on every `push` and `pull_request`, running the three native test suites as separate named steps so a red run names the suite.

**What runs**
- `test/nixie-native.sh` — compiles `nixie_native.cpp` against `firmware/custom-fw/src` with `clang++ -std=c++17 -O2 -Wall`; RLE integrity, every glyph-pair cross-fade, all 86,400 clock times through `tick()`.
- `test/esp1-frame-native.sh` — compiles `esp1_frame_native.cpp` the same way; CRC16, fuzzed COBS, epoch rule, BLIT length checks, UART framer.
- `test/helper-unit.mjs` — extracts the `@module` regions from `tools/helper.html` and runs them under Node 22 in a `vm` context.

**Why native-only**
The suites already compile the firmware's render/frame headers on the host and need nothing but a C++17 compiler and Node. Adding PlatformIO/ESP-IDF would add minutes of toolchain install per run and still could not exercise the tubes, UART, WiFi, or BLE. The hardware-facing tests in `test/` (`clock-sweep.sh`, `rest-smoke.sh`, `esp1-*.py`, `serial-baud-probe.py`, `weather-providers.sh`) stay manual. No secrets, no deploy.

**Toolchain choices**
- `actions/checkout@v4`, `actions/setup-node@v4` with `node-version: '22'`.
- Compiler: the scripts default to `clang++` (overridable via `CXX`); `ubuntu-latest` ships it, so the "ensure compiler" step only `apt-get install`s clang if it is somehow missing, then prints the compiler and Node versions into the log.

**Local run (macOS, Apple clang 17.0.0, Node v26.8.2)**
- `bash test/nixie-native.sh` → `RESULT: ALL PASS` (47 plates, 10,580 cross-fade frames, 86,400 clock times), exit 0
- `bash test/esp1-frame-native.sh` → `8023 checks, 0 failed / ALL PASS`, exit 0
- `node test/helper-unit.mjs` → `1635 checks, 0 failed / ALL PASS`, exit 0
- `CXX=g++ bash test/nixie-native.sh` also passes (confirms the override path).

Draft so the first Actions run can be reviewed before this is considered done.

🤖 Generated with [Claude Code](https://claude.com/claude-code)

https://claude.ai/code/session_01UkFG176ShkdXkb5C4jJ11d
