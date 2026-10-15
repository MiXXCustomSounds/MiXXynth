# MiXXynth — Editor Audit (Task 1)

Automated by `tests/EditorAudit.cpp` (`MiXXynthEditorAudit` target, also run in CI).
The interactive half — dragging every knob in the standalone — is at the bottom.

## What the audit checks

| # | Check | Why |
|---|---|---|
| 1 | Destination name list length == `kNumModDests` | Catches enum/name drift |
| 2 | Names at key enum indices (`Off`, `FM Index`, `Op1 Ratio`, `Op6 Level`, `Master`, `VA Level`, `Filter Drive`, first/last matrix cell) | Catches a reordered enum silently remapping every slot |
| 3 | Every knob has a parameter | Catches a knob wired to a null parameter |
| 4 | Every knob's dest is in `[0, kNumModDests)` | Catches out-of-range destinations |
| 5 | Every knob's `(paramID -> dest)` matches a hard-coded expectation | The core "is it mapped to what it should be" check |
| 6 | No parameter ID is wired to two different destinations | Catches copy/paste mistakes |
| 7 | Every APVTS parameter is reachable from some UI control | Catches unreachable parameters |
| 8 | `isModTarget()` agrees with `dest != dstOff` | Guards the drop-rejection logic |
| 9 | Drop assigns source + destination, and gives a non-zero depth | Drag-drop backend |
| 10 | The same pair reuses its slot; a new pair takes the next free slot | Slot allocation |
| 11 | `Off` source and `Off` destination are refused | Guard rails |
| 12 | Clear empties its slot and leaves others alone | Right-click clear |
| 13 | Depth ring reflects and clears with its slot | Visual feedback |
| 14 | No visible control is left at zero bounds | Regression guard for the `setSize()` ordering bug |
| 15 | Six tabs exist and each has content | Layout regression guard |
| 16 | The sidebar holds exactly one shape strip per LFO/ENV (`kNumShapes`) | Catches a strip silently failing to build |
| 17 | Every strip is visible and at least 100px tall | The editors must be usable, not collapsed |
| 18 | Every strip is sized (non-zero width) | Same zero-bounds guard as #14, for the sidebar |
| 19 | The sidebar content is taller than its viewport, so it scrolls | Catches a layout that would clip the last shape |
| 20 | `setModDepthFor` creates the pair, sets the amount, can go negative, and never duplicates a slot | Backs the draggable depth ring |
| 21 | `removeModSlot` clears only its own pair | Per-source removal from the knob menu |
| 22 | `VA Pulse Width` is a modulation destination | The page claims all six knobs are destinations |
| 23 | A routing edit switches the algorithm to Free, the triggering edit survives, and the algorithm was copied into the matrix | The Matrix tab used to be inert (see `docs/MATRIX-AND-VA.md`) |
| 24 | Loading a state does **not** force Free mode | A preset load must not look like 30 knob edits |

## Bugs found and fixed

### 1. Every dial read the wrong position (serious)
`AudioProcessorParameter::getValue()` returns the **normalised 0–1** value, and
`RangedAudioParameter` does *not* override it. `ContinuousSource` returned that
normalised value from `getValue()` while returning the **real** range from
`getMin()`/`getMax()`, so sst-jucegui computed
`getValue01() = (normalised - min) / (max - min)` — wrong for every parameter
whose range is not 0–1. For example `fmIndex` (range 0–12, default 3) displayed as
`0.25 / 12 ≈ 2%` instead of 25%.

Fixed in `ParamSources.h`: `getValue()` and `getDefaultValue()` now denormalise
via `convertFrom0to1`, and `getValue01()` returns the parameter's normalised value
directly (which is exactly the fraction the dial wants, and stays correct for
skewed ranges such as the log filter cutoff).

The same confusion caused a second bug: `assignModSlot` tested
`std::abs (dep->getValue()) < 0.0001f` to decide whether to apply a default depth.
For the bipolar `-1..1` depth, a value of 0 is *normalised* 0.5, so the test never
fired and **a dropped modulation source was never given any depth** — drag-and-drop
silently produced silent modulation. Both `assignModSlot` and `refreshModRings` now
denormalise before reading.

### 2. Right-click to clear never fired
sst-jucegui's `ContinuousParamEditor::mouseDown` consumes right-clicks, calls
`onPopupMenu(...)` and returns, so the event never reached `ModKnob::mouseDown`.
The clear gesture is now hooked through `knob.onPopupMenu`, with `ModKnob::mouseDown`
kept for clicks in the padding outside the dial.

### 3. Drops on non-modulatable knobs failed silently
48 knobs are not modulation destinations (per-op feedback, per-op ADSR, LFO rate,
mod-env ADSR, MSEG rate, slot depths, macros, voice settings). `assignModSlot`
rejected them with no feedback. Now `ModKnob` shows a **red outline while dragging
over** a non-target and **flashes red on drop**, versus green for a valid target.

### 4. ~21 parameters had no UI at all
Confirmed by grep and then by the audit's coverage check:
`polyphony`, `unison`, `unisonDetune`, `unisonSpread` (polyphony and unison were
completely unreachable), `msegLoop`, and per-LFO `sync`, `syncDiv`, `phase`,
`retrig`. Fixed by adding a **Voice** tab (polyphony, unison, unison detune,
unison spread) and extending each Mod-page LFO row with sync / division / phase /
retrig, plus a MSEG loop toggle.

> **Superseded by the shape editor.** The per-LFO `shape`/`sync`/`syncDiv`/`phase`/
> `retrig`/`bipolar`, the per-env `a`/`d`/`s`/`r` and all nine `mseg*` parameters
> were **removed** when the LFOs and envelopes became editable breakpoint shapes
> (`docs/MOD-SHAPES.md`). Those controls now live on the shape itself rather than
> as host parameters, which is what Serum and Vital do. The Voice-tab half of this
> fix still stands.

### 5. Minor: ambiguous matrix destination names
Matrix destinations read `Op1>2`; now `Op1 > Op2`.

## Current state

```
params=198 knobs=121
modulatable knobs=50  non-modulatable knobs=71
visible child components=268
strips=7 chips=9 rail=0 34 312 866 viewport=6 24 300 804 content=0 0 300 1303
minStripH=161 allVisible=1 allSized=1 scrolls=1
EDITOR AUDIT OK
```

Every one of the 198 parameters is reachable from the UI, and all 50 modulation
destinations are wired to the knob they should be. The knob count fell from 142 to
121 because the 21 LFO/envelope/MSEG controls moved out of the parameter list and
onto the shapes in the sidebar.

## Interactive checklist (standalone)

Run the standalone and confirm by ear/eye:

- [ ] FM tab: algorithm combo changes the tone; each Op Ratio/Level/Fb/A/D/S/R knob
      moves its own dial and audibly changes the sound
- [ ] Matrix tab: each routing knob changes the timbre; carrier toggles add/remove
      operators from the output
- [ ] VA/Filt tab: VA Level blends the saw/square/sub/noise in; filter type, cutoff,
      resonance and drive all respond
- [ ] Voice tab: polyphony limits held notes; unison thickens and spreads
- [ ] Mod tab: 32 slot rows, source/destination/depth all editable
- [ ] Sidebar (any tab): all seven shape editors are visible and keep their position
      when you switch tabs
- [ ] Sidebar: click empty space inside a shape to add a point, drag it, double-click
      it to delete
- [ ] Sidebar: right-drag a segment to bend its curve; right-click a point for
      Linear / Ease in / Ease out / Delete / Reset
- [ ] Sidebar: switch a shape to ENV and one-shot and hear the difference; play a note
      and watch the playhead sweep
- [ ] Drag a shape's chip onto a green-outlined knob -> a depth ring appears and the
      sound modulates
- [ ] Drag a chip onto a red-outlined knob -> it flashes red and does nothing
- [ ] **Drag the ring around a modulated knob** -> the amount changes, the label shows
      the value, and the modulation depth audibly follows
- [ ] **Right-click a modulated knob** -> menu lists the source and its amount;
      "Clear all modulation" removes it
- [ ] Matrix tab: the banner says which mode is in charge. Move a routing knob while
      an algorithm is selected -> it switches to Free, keeps your edit, and audibly
      reroutes. "Copy algorithm to matrix" seeds the grid.
- [ ] VA/Filt tab: the readout under the controls reports the VA/FM balance and the
      filter state. Moving Cutoff is audible straight away (the filter is on and
      open by default).
- [ ] No dial is clipped or reads an obviously wrong position
- [ ] Save the project, reopen it, and confirm the shapes you drew came back
