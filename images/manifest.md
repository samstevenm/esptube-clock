# Image manifest (blog-ready captions)

Originals in `images/raw/`. **PUBLIC** = safe for the public GitHub repo (device/PCB/results);
**LOCAL** = keep out of the public repo (shows home/desk), use only in the Quarto blog if wanted.

## Batch 01 — 2026-09-14

| File in `images/raw/` | Vis | Caption |
|---|---|---|
| `2026-09-14_Front_Wide_Whole_ASSY.jpeg` | LOCAL | Hero: the clock as gifted, six tubes lit (`1 4 2 7 ? 6`), purple underglow. (kitchen in frame) |
| `2026-09-14_Front_Wide_Whole_ASSY_Mode_Menu.jpeg` | LOCAL | Stock SET menu across the tubes; shattered dome on 4; dark tube 5. (home in frame) |
| `2026-09-14_SI-HI-IPS-CLOCK_4_BUTTONS_TopDownView.JPG` | PUBLIC | **The photo that cracked it** — PCB silkscreen "SI HAI IPS CLOCK", 4 buttons UP/MODE/DOWN/POWER. |
| `2026-09-14_Damaged-Ribbon-Cable.jpeg` | PUBLIC | Teardown: a display carrier + its FPC ribbon, and the ESP32-WROOM-32D. |
| `2026-09-14_Screen-DaughterBoard-FRONT.jpeg` | PUBLIC | A single ST7789 135×240 display module, front. |
| `2026-09-14_Screen-DaughterBoard-Back-ZIF-disconnected.jpeg` | PUBLIC | Same module's carrier board / FPC (ZIF) connector, back. |
| `2026-09-14_Tube-Index-Multi-Color.jpeg` | PUBLIC | Self-test: each tube a distinct color + its index — mapping the slots by eye. |
| `2026-09-14_Tubes_Displaying_726.jpeg` | PUBLIC | Digits pushed over REST / the clock running. |
| `2026-09-14_Tubes_Displaying_A_Wide_Image.jpeg` | PUBLIC | **Spanning canvas** — one wide image sliced across all the tubes. |

## Batch 02 — 2026-09-15 (the working stack running content)

All of these are whole-clock **desk shots with home in frame**, so all **LOCAL** (kept out of the
public repo). Great for the blog — except the two marked **PRIVATE** (family/children).

| File in `images/raw/` | Vis | Caption |
|---|---|---|
| `2026-09-14_Front_Wide_Whole_ASSY_Widgets_and_image.jpeg` | LOCAL | **Hero:** five per-tube widgets at once — a picture, a **BITCOIN ticker**, an image, **weather**, and a **Nixie clock**, each with its own underglow. |
| `2026-09-14_Front_Wide_Whole_ASSY_Widgets_and_image2.jpeg` | **PRIVATE** | Nixie time + **BITCOIN & TSLA** tickers + weather + a family photo (**contains a child — keep private**); shattered dome + bare module at right. |
| `2026-09-14_Front_Wide_Whole_ASSY_Images.jpeg` | **PRIVATE** | Family photos across the tubes (**contains children — keep private**); the helper is open on the laptop. |
| `2026-09-14_Front_Wide_Whole_ASSY_Numbers.jpeg` | LOCAL | Five tubes showing digits `1 8 1 7 0` beside the bare 6th display module. |
| `2026-09-14_Front_Wide_Photo.jpeg` | LOCAL | A spanning product image (lighting spec sheet) sliced across the tubes; helper on the laptop. |
| `2026-09-14_Front_Wide_1926.jpeg` | LOCAL | **Big digital clock** (`19:26`) across the tubes; the laptop shows the matching time. |
| `2026-09-14_Front_Wide_1926_2.jpeg` | LOCAL | The same big digital clock a moment later (`19:26:24`); shattered dome + bare module at right. |

_Public repo includes only the PUBLIC-tagged files (device/PCB/results); the LOCAL desk/home
shots and the **PRIVATE** family shots stay out of it (per Sam's choice). PRIVATE = do not publish
anywhere (children)._

## Generated — 2026-09-17

| File in `images/` | Vis | Caption |
|---|---|---|
| `nixie-plates.png` | PUBLIC | The ten lit nixie plates + the unlit plate exactly as baked into firmware (synthetic, from `tools/gen_nixie_glyphs.py`). |
