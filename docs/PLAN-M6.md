# MiXXynth — M6: consolidation and depth

Status: **planned, not started.** Supersedes the ordering of M5's remaining
workstreams. M5 workstreams 1-3 (modulation UX, the WeirdVerb rename, convolution)
are done; M5's *feature* workstreams (distortion, chorus, presets) are deferred
until this is finished, because this is depth on what already exists rather than
new modules.

## Goal

The first four tabs — **FM, Matrix, VA/Filt, Voice** — become one tab, and the
sections in it get real depth rather than being grids of unlabelled knobs. Two
motives:

1. **The FM section is 48 knobs labelled individually**, which is a wall of
   controls, not an instrument. Six operators sharing a row of named knobs reads
   as 48 separate things instead of six operators with four properties each.
2. **Turning a knob tells you almost nothing.** Today a knob shows its *name*
   and, only while dragging the mod ring, the depth. There is no number, no
   curve, no picture. That is the single biggest gap in the whole plugin.

Everything below is scoped to **step 1: the FM section rebuilt as a grid.** The
Matrix, VA/Filt and Voice sections move into the same tab afterwards, once the
grid's look and feel is settled.

## The grid

A toolbar sits above the grid:

```
+--------------------------------------------------------------------------+
|  [d6]                                        Silent: no carrier has Level |
+--------------------------------------------------------------------------+
    ^ square dice button, icon only            ^ the audibility warning, moved
```

Then the grid itself — seven rows (a header plus six operators) and six columns:

```
+------------+---------+---------+---------+----------+--------------------+
| FM INDEX   |  icon   |  icon   |  icon   |  icon    |   ADSR             |
|  0.150     | shape   | ratio   | level   |  fdbk    |                    |
+------------+---------+---------+---------+----------+--------------------+
| OP 1  [on] |  ~wave~ |  2.000  |  0.850  |  0.100   |   /\______         |
+------------+---------+---------+---------+----------+--------------------+
| OP 2  [on] |  ~wave~ |  1.000  |  0.500  |  0.000   |   /\______         |
+------------+---------+---------+---------+----------+--------------------+
|   OP 3..6  |    .    |    .    |    .    |    .     |       .            |
+------------+---------+---------+---------+----------+--------------------+
```

- The **FM Index** cell sits at the top-left of the header row, above the operator
  column, as a square slider like every other cell.
- The four column headers are **icons**, not words, so the square aesthetic holds.
- The **operator cell** is a label and an on/off toggle in one — the toggle
  replaces the separate checkbox that used to sit to the left of each row.
- The **ADSR column is rectangular**, everything else is square.

### Cell behaviour

Every cell is a vertical drag slider: drag up and down, the value follows. The
value is drawn in the cell. There are no separate knobs anywhere in this section.

### Colour

| State | Fill |
|---|---|
| 0% | black — no fill |
| value, unmodulated | orange, filled from the bottom, proportional to value |
| value, modulated | **blue**, still filled from the bottom |

A modulated cell's fill **moves live** with the modulation, so you watch the
modulation happen rather than inferring it from an arc.

*Interpretation to confirm:* the whole fill switches to blue when a cell carries
any modulation, rather than only the modulated delta being blue. It is the more
legible of the two and the one that reads at a glance.

### Modulation depth: the white line

The colour fill shows the *current value*. It does not show how far the value can
be pushed, which is what the old arcs were for. That is a **white line overlay**
whose outer edge can be grabbed and dragged to scale the depth.

- One white line per assignment, matching the arc behaviour, so a cell with two
  sources shows two lines and each is individually adjustable.
- The inner end sits at the current value; the outer end sits at value + depth.
  Depth is bipolar, so the line may extend above or below.
- Dragging the outer edge changes that assignment's depth.
- The lines only appear when the cell actually has assignments.

**Interaction conflict, and how it resolves:** the cell body is a value slider and
the white line's edge is a depth handle, so one cell has two drag targets. The
outer edge is hit-tested **first**, within a few pixels, and everything else drags
the value. When a cell has no assignments there is no overlay and no ambiguity.

## What gets reused, and what is new

Most of this already exists and should not be rewritten.

| Piece | Source | Notes |
|---|---|---|
| Envelope editor | `ShapeEditorComponent` | Already does breakpoint editing, `setPlayhead`, `onChange`, accent colour, grid divisions. Used verbatim for the ADSR column. |
| Cell data | `mixxynth::ui::ContinuousSource` | `getValue()` is already normalised 0-1 — exactly the fill height — and `getValueAsStringFor()` gives the display text. |
| Mod assignment | `ModKnob`'s drop target + right-click menu | `ModKnob` is currently the **only** drag-and-drop target and the only owner of the assign/clear/amounts menu. |
| Live values | the playhead publish path | `VoiceManager::publishShapePhases` -> processor `std::atomic<float>` array -> editor timer. |

### New: publishing live modulated values

The colour spec needs the *modulated* value, and there is no such thing today.
Modulation is evaluated **per voice** (`ModMatrix.h`: *"shapes keep their own phase
and state per voice"*), so there are up to N modulated values, not one.

The codebase already solves this exact shape of problem for the playheads: the
newest voice's phases are published once per block into relaxed atomics and read
by the editor's timer. Modulated values extend that same path, and follow the same
**newest voice** convention, so the display agrees with what the playheads already
do. Single writer, single reader, no locks.

Operator phases are not published today either — only LFO/ENV phases are — so the
live shape cell needs the newest voice's six operator phases published the same
way.

### New: the square cell component

A new `SquareCell` draws the fill, the value, the optional live waveform and the
white depth lines, and handles the two drag targets.

The mod-assignment behaviour (drag a source onto it, right-click to assign / clear
/ set amounts) is **shared** with `ModKnob` rather than reimplemented, so the two
cannot drift. This matters more than it looks: the destination is squares
everywhere, so `ModKnob` and `SquareCell` will coexist for a while, and two copies
of the assignment logic is how the FM grid and the VA page end up behaving
differently.

### The operator envelope: `AdsrEditor`, not `ShapeEditorComponent`

The plan was to reuse `ShapeEditorComponent` behind an adapter between the four
float parameters and a `ModShape`. That turned out to be the wrong shape of reuse,
for one specific reason: **a breakpoint editor needs its points to be distinct,
and the init patch has every stage at zero.**

With attack, decay and release all at zero, an adapter that places a point at each
stage boundary produces three points stacked on the origin. They cannot be told
apart, cannot be dragged apart, and so the most common state in the plugin is the
one state that editor cannot edit.

So `AdsrEditor` is its own component with three **semantic** handles - attack
peak, decay corner, release end - rather than free breakpoints. It still shares the
drawing: it builds the equivalent `ModShape` and renders it through the same
`buildShapePath` the modulation section uses, with the same colours, grid and
curve, so the two cannot drift apart visually.

**The parameter format does not change**, so no preset is affected and the DSP
keeps using the same four floats.

Two decisions inside it, recorded because neither is obvious:

- **The time axis is a power curve, not a straight line.** The window spans the
  three parameter maxima plus a nominal half-second sustain hold, so 14.5 s. On a
  linear axis a 2 ms attack is 0.014% of the width, which is not somewhere a
  pointer can go; the curve puts it at 2.9%, which is eight pixels across a 300 px
  cell, and the audit checks exactly that. It is also exactly invertible, so a drag
  lands on the value its handle is drawn at instead of sliding away from the
  pointer.
- **The sustain plateau is a drawing convention.** A sustain lasts as long as the
  note does, so it has no duration to put on a time axis; the display gives it a
  fixed half second. The first version left it out, and the result decayed straight
  into its release - which reads as an ADR, with the sustain level as a single point
  rather than a level. That was visible in the first screenshot and not in any
  check, so there is now a check for it.

## Decisions

**The algorithm section is removed from the UI, and the parameter is pinned to
"Free (matrix)".** `algorithm` is a choice of 32 DX7 algorithms plus index 32,
"Free (matrix)", which hands routing to the matrix. Removing the UI while leaving
the parameter wherever it was would leave an invisible control still overriding
the matrix — which is the "does not function as intended" problem being escaped.
Pinning it to Free makes the matrix the single source of routing truth, which is
also what the merged tab assumes. `Algorithms.h` and the DX7 DSP stay in place so
the feature can return once it works properly. The "copy algorithm to matrix" and
"free mode" buttons go with it.

**Ratio stays continuous** (0-16, skewed), so its cell reads `2.000`. Snapping to
musical ratios would change how existing presets sound for no functional gain, and
continuous is strictly more expressive. This was left to my judgement.

**The grid scales with the window**, and the window's minimum size is raised if
needed so the grid never becomes unreadable. Squares stay square at every size.

## Deferred

- Merging Matrix, VA/Filt and Voice into this tab.
- The graphical filter, filter modes, filter routing and the mixing functions
  from the previous round of questions — all still wanted, all after this.
- M5's remaining feature workstreams (distortion, chorus, presets).

## Order of work

1. ~~`SquareCell`~~ — **done** (`fabaa53`). Delivered more than planned: the cell
   also carries the white depth lines, the depth-edge drag, the live-value hook
   and the drop target, because a cell without those is not a cell. The
   `ModTarget` extraction came with it.
2. ~~Migrate `ModKnob` onto `ModTarget`~~ — **done.** The duplicate drag-drop,
   menu and depth-drag logic is gone; `ModKnob` is now only the dial and the arcs.
3. ~~The grid layout~~ — **done.** The FM section is a six-column grid of square
   cells with glyph column headers and a die button in the toolbar. The algorithm
   selector is gone and the parameter is pinned to Free. The ADSR column still
   holds knobs, which is step 6.
4. The live shape cell, via published operator phases.
5. The live modulated fill, via the published-value path.
6. ~~The ADSR column~~ — **done**, but not via `ShapeEditorComponent`. See below.
7. ~~Remove the algorithm section; pin the parameter to Free~~ — **done**, as part
   of step 3.
8. Audit checks and a visual pass — ongoing. Every step has added checks rather
   than saving them for the end, because a check written a step later is a check
   written after the bug has already been shipped once.

Each step keeps the plugin buildable and the tests passing.

## Progress

**Steps 1-3 and 6 complete.** 179 audit checks, 0 failures; pluginval strictness 5
SUCCESS; smoke OK.

- **Step 1** (`fabaa53`): `SquareCell` and `ModTarget` exist, build and are tested.
- **Step 2**: `ModKnob` derives from `ModTarget`, and its duplicate drag-drop, menu
  and depth-drag logic is deleted. **The audit suite passed unchanged**, which is
  the evidence that nothing moved rather than a claim that it did not.
- **Step 3**: the FM section is a grid. The editor now holds a list of `ModTarget`,
  so a knob and a cell are registered and wired identically.
- **Step 6** (`AdsrEditor`): the ADSR column is graphical envelopes rather than four
  knobs per operator, drawn in the modulation section's ENV style.

**The grid is visible and complete.** The FM tab is six columns - operator, shape,
ratio, level, feedback, ADSR - and seven rows, with FM Index in the header row. The
algorithm selector is gone; the parameter is pinned to Free (matrix) as the page
builds, so routing is the matrix's job alone. Each operator's envelope is a
draggable curve, not four numbers.

**Still to come:** the shape cell shows its shape's *name*, not its waveform (step
4); the cells do not yet show live modulation (step 5).

**One thing to look at:** the grid is square-celled and centred, and the tab is
wider than the grid needs, so there is dead space either side. That is the honest
consequence of the "every cell is square" rule - the height binds before the width.
If the envelopes want more room, the ADSR column could absorb that slack instead.
