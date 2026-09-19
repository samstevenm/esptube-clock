# Shows — `esptube-show` cue-list format

A **show** is a timed **cue list**: an ordered list of steps, each an action held for a
number of seconds. It's the same *sequence-of-timed-media* idea as **W3C SMIL** and
classic show-control cue lists — kept here as compact JSON that extends the `esptube-scene`
files, so a show imports, exports, and edits as one text file. Most steps drive the clock's **native nixie engine** (the clock renders + animates
from its own baked glyphs — zero pixels streamed); graphical steps push pixels.

Play / export / import from the helper's **🎬 Scenes → Show** row. The built-in
**Grand Tour** demo walks the clock through all of it (`grand-tour.esptube-show.json`).

## Shape

```json
{ "app": "esptube-show", "v": 1, "name": "My Show", "loop": false,
  "steps": [ { "act": "...", "dur": 3, "cap": "optional label", ... } ] }
```

- `dur` — seconds to hold the step (min 0.25).
- `cap` — optional short caption shown in the helper's progress readout.
- `loop` — restart at the end instead of settling on the clock.
- A show that isn't looping ends by returning to the **nixie clock** (`/preset/0`).

## Step actions (`act`)

| `act` | fields | what it does |
|---|---|---|
| `nixie` | `text`, `effect`, `ms` | Native nixie message. `effect`: `scroll` (smooth horizontal marquee), `flash`, `static`, `vscroll` (smooth vertical, hyphenated), `vpage` (vertical page-flip), `scrollstep` (legacy). `ms` = motion rate. Zero pixels — the clock scrolls it itself. |
| `countdown` | `seconds` | Native on-device countdown (flashes `0000`, then the clock). |
| `preset` | `preset` 0–4 | Device face: `0` nixie clock · `1` digital · `2` LED show · `3` off · `4` date. |
| `white` | `text`, `font`, `color`, `bg`, `size` | Plain built-in-font text across the tubes (pixels). |
| `led` | `led` (`rainbow`/`breathe`/`comet`/`off`/`solid`), `rgb` | Underglow effect (runs on-device). |
| `anim` | `id`, `params` | Run a studio animation for the step: `pong` (a ball across the tubes), `swap` (coloured blocks + numbers rotating around the tubes — per-tube addressing / positions swapping), `matrix`, `starfield`, `plasma`, `ledwave`, `news`, `ticker`, `sparkline`, `calendar`, … Text effects (`news`/`ticker`) render as nixie per the global style; graphical ones push pixels. |
| `clear` | — | Blank the tubes. |

Text is folded to the device glyph set (`A–Z 0–9 - : . ! ? ° % + /`); `▲→+`, `▼→-`,
`•→-`, commas dropped. Long text in `vscroll`/`vpage` is hyphenated to the live-tube count.

The **global nixie style** (effect · rate · ⚡native, set in the helper's 🕯️ Nixie panel) governs
how `news`/`ticker` and the message box render everywhere; a show's `nixie` step sets its own
`effect`/`ms` explicitly, so a saved show always plays the way it was authored.

## Bundled shows

- **`grand-tour.esptube-show.json`** — the showpiece: plain text → digital → nixie → cheeky lines →
  live news → per-tube swap → colours → LEDs → **pong** → images → live crypto → bombast → the
  long-term-nuclear-waste warning (vertical) → home. Escalating, silly, then ominous.
- **`the-works.esptube-show.json`** — the rest of the box: a vertical-scroll intro, **individual
  tubes** and **positions swapping** (`swap`), **pong**, primitive colours, simple images, live
  data, and a vertical-page recap. Both are seeded into the helper's 🎬 Shows library on load.

## Interop

The JSON maps 1:1 onto SMIL's `<seq>` of timed items (and a future `<par>` for
simultaneous display + LED tracks), so a show can be emitted as SMIL for other players
without changing the model — we adopt the standard cue-list abstraction rather than a new one.
