# Helper UI/UX audit — 2026-09-17

*Looked at `tools/helper.html` as a first-time user would: desktop (800 px) and phone (375 px),
top to bottom, then fixed what was cheap. Candid.*

## Verdict

It does a lot and it is honest about device state — but it grew by accretion. The top panel
had become a flat wall of **nine** sub-groups with **three overlapping ways** to decide what the
clock shows (raw Mode, Device face, Push scene), two unlabeled numbers, and jargon in the
explanatory notes (RGB565, IndexedDB, POSIX TZ) on a page whose stated goal is *kid- and
non-English-friendly*. Scene rows carried seven identical-looking icon buttons each. None of that
is structural; it's ordering, labeling and hiding.

## Findings → what changed

| # | Finding | Severity | Status |
|---|---|---|---|
| 1 | **Unlabeled numbers** in the Nixie message row (`400`, `90`): nothing said one was *ms per step* and the other *seconds*; on a phone they wrapped onto a line of their own. | high | **Fixed** — two labeled rows: *every `400` ms · 📣 Show* and *⏳ `90` s · Start countdown · ⏹ Back to clock*. |
| 2 | **Three ways to control what shows** (🎬 Mode clock/off/manual · 🎭 Device face · 📤 Push). "Manual" is a firmware concept, meaningless to a kid. | high | **Fixed** — Device face leads; raw Mode + virtual buttons + timezone + working tubes moved into a collapsed **⚙️ Setup & advanced**. |
| 3 | **No hierarchy** in Clock controls (nine sub-groups, same visual weight). | high | **Fixed** — order is now: what the clock shows → nixie message → brightness/back lights → push; advanced folded away. |
| 4 | **"Now:" status** buried in a note under Mode, far from the header status pill; hands-off note elsewhere again. | med | **Fixed** — "Now: 🎭 nixie · clock · 🔌 auto-push paused (resume)" sits directly under the Device face buttons. |
| 5 | **Scene rows**: 7 tiny icon buttons (📂 ▶️ 💾 ✏️ 📋 ⤓ 🗑️), tap targets ~30 px; ▶️ not clearly primary. | med | **Fixed** — ▶️ (primary) + 📂 visible; the rest behind **⋯**. |
| 6 | **Jargon notes** (raw565, IndexedDB, POSIX, bandwidth-bound) at full weight between panels. | med | **Fixed** — collapsed into ℹ️ *Where things are saved* / ℹ️ *How pushing works*. |
| 7 | **Countdown looked wrong** ("milliseconds?"): the seconds-ones digit was laid out into the empty slot 0, so the visible number ticked every 10 s. | high (firmware) | **Fixed** — countdown right-aligns onto *live* tubes; verified `0134 → 0132`. |
| 8 | A test that takes over the glass (clock sweep) gave no on-device cue — got power-cycled twice as "stuck". | med | **Fixed** — sweeps announce **TEST** in nixie letters first; the header shows 🧪 progress. |
| 9 | Push label said "Push wide" in spanning mode — cryptic. | low | **Fixed** — "📤 Push the picture" / "📤 Push the tubes" per mode. |
| 10 | Phone: header stacks to four rows; Device face buttons wrap with "Off" orphaned. | low | open (icon-only buttons under 560 px) |
| 11 | Emoji-only buttons rely on `title` tooltips (no hover on touch). | low | open (aria-labels; long-press hint) |
| 12 | Tube cards are tall; the preview dominates and the widget picker is below the fold on a phone. | low | open (collapsible preview on narrow screens) |
| 13 | Three state indicators (header pill, PAUSED banner, hands-off note) in three styles. | low | acceptable — each answers a different question; banner is deliberately loud. |

## Principles going forward
- **One obvious way** to do each thing; the raw/firmware way lives under *advanced*.
- **Every number has a unit** on the same line.
- **Primary action is the amber one**, one per row.
- **Explain on demand** (ℹ️ details), never inline at full weight.
- **Anything that takes over the glass announces itself on the glass.**
