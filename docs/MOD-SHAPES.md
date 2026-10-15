# MiXXynth — Editable Modulation Shapes (LFOs and ENVs)

The four LFOs and three modulation envelopes are no longer fixed waveforms. Each
one is a **breakpoint shape** you draw directly, in the style of Serum and Vital.
They live in the left sidebar, are visible on **every tab**, and each one carries a
drag chip that can be dropped onto **any knob**.

Replaces the old fixed-waveform LFOs, the ADSR mod envelopes and the 8-step
"MSEG-lite" (which was a poor-man's version of the same idea).

## Research: what was available, and what the licences allow

| Source | Licence | Verdict |
|---|---|---|
| **Vital** (`mtytel/vital`) — `src/interface/editor_components` line editor + `LineGenerator` | **GPLv3** (verified from its `LICENSE`) | Study-only. Copying any of it would relicense this MIT project, so **nothing** is copied. |
| **Surge XT** MSEG | **GPLv3** | Study-only, same reason. |
| **Serum** | Closed source | Behaviour reference only. |
| `sst-jucegui` (already a dependency) | MIT | **Has no** breakpoint/curve/line editor — verified by grepping the whole module. Only an unrelated `ZoomContainer` matches. |
| `chowdsp_utils` (local reference) | BSD-3 | **Has no** curve/line editor either — verified by grep. |

**Conclusion: there is no permissively-licensed component to reuse, so this is an
independent MIT implementation** of the same well-known model. That is also the
safer outcome: the DSP and the drawing share one code path, so what you see is
exactly what you hear.

## The model

A shape is up to **16 breakpoints**. Each point carries:

- `x` — position through the shape, 0..1
- `y` — level, 0..1
- `tension` — the curve of the segment that **starts** at this point, −1..+1

`tension` is applied as a power curve, `y = pow(u, 2^(-tension*3))`, so the
exponent spans 0.125..8. It is monotonic, so a segment can never fold back on
itself:

- `tension = 0` → straight line
- `tension > 0` → rises fast, then flattens (ease out)
- `tension < 0` → rises slowly, then accelerates (ease in)

`x` is snapped to a 16-step grid (hold **Shift** to place freely). The first and
last points are pinned to x = 0 and x = 1 and only move vertically, which is what
both Serum and Vital do.

## Setting the amount: the depth ring

Dropping a source on a knob assigns it with a default depth of 0.35. To set the
amount, **drag the ring around the knob** — the gesture Serum and Vital use.

| Gesture | Result |
|---|---|
| Drag the ring up / down | Sets that source's amount, 110 px for the full −1..+1 travel |
| Right-click a knob | Menu: every assigned source with its amount, an **Amount** submenu of exact values (+1.00 … −1.00), and **Clear all modulation** |
| Click an assigned source in the menu | Removes just that one pair |

Details that matter:

- The dial is inset by 13 px on every side so the ring sits **outside** the dial,
  where it is both visible and hit-testable. Before this the ring was drawn 1 px
  outside the dial edge and could not be grabbed at all.
- The label strip becomes a live readout while dragging (`+0.42`), green for
  positive and red for negative, and snaps to exactly 0 within ±0.02.
- Up to **three** sources are drawn as separate arcs stepping outwards; beyond that
  a `+n` marker appears. Multiple sources are why the ring shows a list rather than
  a single value.
- While a ring is being dragged the knob owns its own value and the 30 Hz refresh
  leaves it alone, so the parameter and the drawing cannot fight each other.
- `refreshModRings` hashes the slot configuration and skips rebuilding the 121
  assignment lists when nothing changed, so the timer is not allocating every tick.

| Mode | Behaviour |
|---|---|
| **LFO** | Loops. Phase advances at `rate` (free Hz, or host-synced divisions). |
| **ENV** | One pass on note-on, then holds its last point while the note is held. On note-off it ramps to zero over `release`. |
| **One-shot** | One pass on note-on, then holds. Ignores note-off. |

Per-shape controls: **rate**, **sync + division**, **phase**, **delay** (note-on
delay that holds the first point), **smooth** (one-pole on the output — Serum's
"smooth"), **bipolar** (map y 0..1 to −1..+1 around the shape's midpoint; off keeps
it unipolar) and **retrig** (LFOs only: restart phase on note-on).

## Editing gestures

| Gesture | Result |
|---|---|
| Click empty space | Add a point there and start dragging it |
| Drag a point | Move it (x snapped to the grid; Shift = free) |
| Double-click a point | Delete it (endpoints cannot be deleted) |
| Right-drag a segment | Bend that segment's curve (the Vital gesture) |
| Right-click a point | Linear / Ease out / Ease in / Delete / Reset shape |
| Right-click a segment | Flatten curve / Reset shape |
| `reset` button | Back to the factory default for that shape |

Dragging reports live `x`, `y` and curve values to the status line under the
sidebar. A playhead sweeps the shape of the newest active voice while you play.

## Where the shapes live, and why they are not parameters

Shapes are stored in the plugin state `ValueTree` under a `shapes` child, **not** as
individual parameters. That is what Serum and Vital do, and it avoids adding ~340
parameters (7 shapes × 16 points × 3 values). They serialise with the preset and
survive save/reload.

The trade-off, stated plainly: **shape controls (rate, smooth, phase, delay) are
not host-automatable and cannot themselves be modulation destinations.** Before
this change `LFO 1 Rate` was an automatable parameter with a knob. If you want that
back, M4 can add a single `lfo{n}rateOffset` parameter that offsets the shape's
rate, rather than duplicating the source of truth.

## Thread safety

Shapes are edited on the message thread and read on the audio thread, so they are
**double-buffered** with an atomic publish index (`shapeBuffers[2]` +
`activeShapeBuffer`). `setShape` copies the inactive buffer, edits it and publishes
it; `processBlock` copies the active buffer once per block. The audio thread can
never observe a half-written shape, and there is no lock on the audio path.

Playheads are published the other way: `VoiceManager::publishShapePhases` copies the
newest active voice's phases into a `std::array<std::atomic<float>>` once per block
(single writer, single reader), which the editor's 30 Hz timer reads.

## Layout

The sidebar is a sibling of the tabbed component, not a child of a tab, which is
what makes the shapes visible on every tab:

```
title
|- rail (312 wide)                     <- always visible
|  |- "MODULATION"
|  |- viewport -> content
|  |  |- ShapeStrip x7  (chip + mode + 88px editor + rate/sync/div/smooth/phase/bip/rtg/delay)
|  |  '- "PERFORMANCE" + 9 chips (Macro 1-4, Velocity, Mod Wheel, Aftertouch, Key Track, Timbre)
|  '- status line
|- tabs (FM / Matrix / VA-Filt / Voice / Mod / FX)
|- performance bar (pitch + mod wheels, on-screen keyboard)
'- macros bar (Macro 1-4 + Master)
```

Content is 1303px tall in an ~804px viewport at the default window size, so the
sidebar scrolls. The audit asserts that (check 19) so a future layout change cannot
silently clip the last shape.

## Verification

- `MiXXynthSmoke`: 14 new checks covering tension endpoints and shape,
  monotonicity, linear interpolation, endpoint clamping, `sortPoints` ordering,
  the factory defaults, a full `ValueTree` round-trip, a missing-node fallback, and
  ENV-hold / ENV-release / one-shot-hold behaviour driven through the real
  `computeSources` path.
- `MiXXynthEditorAudit`: 4 new checks for the sidebar (strip count, heights,
  visibility, scrolling).
- `pluginval --strictness-level 5`: SUCCESS.

## Known limitations

1. **Not sample-accurate at block boundaries.** Shape phase advances once per
   32-sample sub-block, like the rest of the mod engine.
2. **No copy/paste or shape presets yet.** Every shape resets to its factory
   default. A shape bank (copy/paste between slots, save/load named shapes) is M4
   work.
3. **One shape per source.** No stereo split or per-note randomisation yet, unlike
   Vital's stereo LFOs.
4. **Removing the MSEG source is a state-compatibility break.** Existing presets
   that referenced `MSEG` will have that slot fall back to `Off`, because the source
   enum shifted by one (`kNumModSources` 18 -> 17). Pre-release, so no migration was
   written.
5. **Curve drawing uses 160 line segments**, not a true analytic path. It is
   visually smooth at sidebar size but is not a vector-exact render.
