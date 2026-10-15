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

### 5. Minor: ambiguous matrix destination names
Matrix destinations read `Op1>2`; now `Op1 > Op2`.

## Current state

```
params=245 knobs=141
modulatable knobs=49  non-modulatable knobs=92
visible child components=131
EDITOR AUDIT OK
```

Every one of the 245 parameters is reachable from the UI, and all 49 modulation
destinations are wired to the knob they should be.

## Interactive checklist (standalone)

Run the standalone and confirm by ear/eye:

- [ ] FM tab: algorithm combo changes the tone; each Op Ratio/Level/Fb/A/D/S/R knob
      moves its own dial and audibly changes the sound
- [ ] Matrix tab: each routing knob changes the timbre; carrier toggles add/remove
      operators from the output
- [ ] VA/Filt tab: VA Level blends the saw/square/sub/noise in; filter type, cutoff,
      resonance and drive all respond
- [ ] Voice tab: polyphony limits held notes; unison thickens and spreads
- [ ] Mod tab: 32 slot rows; LFO shape/rate/sync/division/phase/retrig/bipolar;
      env ADSR; MSEG steps, rate and loop
- [ ] Drag a source chip onto a green-outlined knob -> a depth ring appears and the
      sound modulates
- [ ] Drag a chip onto a red-outlined knob -> it flashes red and does nothing
- [ ] Right-click a modulated knob -> its ring clears
- [ ] No dial is clipped or reads an obviously wrong position
