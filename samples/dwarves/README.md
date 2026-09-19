# Snow White & the dwarves

Original low-poly turntable animations (135x240, 36 frames, 12 fps) of a princess and seven bearded dwarves from the public-domain Grimm tale "Snow White": `princess` (Princess), `miner` (Miner), `baker` (Baker), `fiddler` (Fiddler), `snoozer` (Snoozer), `grouch` (Grouch), `shy` (Shy), `giggler` (Giggler).

The designs, names, colours and props are our own -- evocative homages built from spheres, cones and boxes, not traced from or named after any film. Released CC0 (public domain); use them however you like.

Each `<name>/f00.png .. f35.png` is one frame of a seamless loop (`<name>.gif` is the same loop; `sheet.png` shows the front view of everyone). `manifest.json` lists the cast for `tools/bridge/bridge.py play chars:dwarves` and the helper page.

The frames are RGBA PNGs with a **transparent background**: only the figure and its semi-transparent black floor shadow are drawn, so they composite over anything -- a solid colour, a gradient, an animated scene. `tools/bridge/bridge.py play chars:dwarves --bg ...` picks what goes behind them on the tubes (`black` by default, `#rrggbb`, `#rrggbb-#rrggbb` vertical gradient or `glow:#rrggbb`), and the helper page draws them over its own background. The GIFs are matted over black and `sheet.png` over dark grey.

Regenerate with `python3 tools/render_characters.py --set dwarves --frames 36 --size 135x240 --ss 3` (Python 3.9 + Pillow, no other deps).
