# MiXXynth — M7: presets and randomize

Status: **in progress.** Absorbs M5 workstream 7 (presets) and SPEC S8's randomize
core, both of which M6 deferred. M5's ordering put this after the modules and before
the visual pass, and the modules are done, so this is the next thing.

## Goal

Two halves of one idea: **the patch is an object you can keep, and one you can roll.**

1. A **bank**: factory presets that ship with the plugin, user presets beside them,
   one categorized list, a dirty marker, and the same list visible to a DAW as
   programs.
2. A **randomizer**: one global roll over five named sections, a lock per section, a
   roll constrained to musical values rather than to 0..max, and an undo.

Neither is an afterthought. S8 names randomization as a core feature rather than a
toy, and the reason is the engine: 458 parameters across six operators, a free
routing matrix, a VA layer, per-operator filters and twelve effects is a space that
cannot be searched by hand at a useful rate.

## The state seam, first

`getStateInformation` / `setStateInformation` were the only route in or out of a
patch, so presets, Init and the roll's undo would each have had to go through a
`MemoryBlock` to reuse them. They are split into three:

| Call | What it is |
|---|---|
| `buildStateTree()` | parameters + shapes as a `ValueTree`, no XML |
| `applyStateTree (tree)` | the reverse, `true` when the tree was usable |
| `buildInitState()` | every parameter at its default, the default modules, an empty rack |

`getStateInformation`/`setStateInformation` become the XML wrapper around those two,
so there is exactly one implementation of "what a patch is" and the existing
round-trip the audit already proves keeps covering it.

## Decisions

**A preset file is the state XML, with no wrapper and no metadata.** The name is the
filename and the category is the folder, so there is nothing to keep in sync and a
file the user has exported from a DAW session loads unchanged.

**A preset is a PARTIAL state, applied over Init.** A file carries only what it
changes. Loading is therefore: take the init tree, overlay the preset's properties,
apply once. That makes a factory preset 30-60 readable lines instead of 458, and it
is what makes the bank reviewable at all. The cost is real and is written down here
rather than discovered later: **a preset that omits a parameter inherits that
parameter's DEFAULT, not the previous patch's value**, so changing a parameter's
default changes every preset that did not mention it. The audit proves the
alternative is true - a load is deterministic and Init restores exactly.

**The factory bank is embedded in the binary** (`presets/<Category>/<Name>.xml` in
the repo, through `juce_add_binary_data`), not copied to disk on first run as M5
workstream 7 sketched. Reason: a VST3 installed into a DAW has no repository to copy
from, and a disk mirror that is written but never read is a second source of truth
that can drift. The XMLs stay text in the repo, so there are no binary assets and no
licensing exposure. **This is the one place M7 departs from the M5 plan and it is
flagged for the author rather than applied quietly.**

**Categories are folders**, which is the Serum-1 bank shape: `presets/Bass/*.xml`.
The browser shows a section per category, with factory items marked, and user
presets in the same list.

**Host programs are the same list.** `getNumPrograms` / `getProgramName` /
`setCurrentProgram` are wired to the bank, so a DAW sees the presets instead of the
plugin maintaining a second list that drifts from the one on screen.

**Undo is one level.** It puts back the patch as it was before the last roll or
preset load. Deeper undo is a general editor feature that PRODUCT.md lists as
undecided, and it is not smuggled in through the dice.

**The roll is seeded.** The same seed and the same locks produce the same patch,
which is what makes "the roll respects its locks" a check instead of an opinion.

**The loader forces the algorithm to Free.** M6 removed the algorithm UI and pinned
the parameter so the routing matrix is the single source of routing truth. A preset
(or an old session) that names a DX7 algorithm would otherwise restore an invisible
control that overrides the matrix - the exact bug M6 set out to kill. There is no UI
to move it off, so it is forced in `applyStateTree`, once, for every route in.

## Sections

The roll covers five, each with its own lock:

| Section | What it moves | The constraint that makes it musical |
|---|---|---|
| FM | index, per-op ratio/level/feedback/wave, operator envelopes | ratios from a musical set; at least one carrier left audible |
| VA | level, shape morph, octave, detune, sub, noise, VA envelope | the VA stays a blend under the FM rather than taking over |
| Filter | global type/cutoff/reso/drive; per-op filters and their envelopes | cutoff inside the audible band; per-op filters mostly off |
| Mod | the 32 matrix slots and the editable shapes | plausible source->destination pairs, modest depths, shapes from archetypes |
| FX | slot types, their parameters, bypass | 2-4 modules from the descriptor defaults, jittered; a real mix |

Audibility is not a hope: a roll always leaves at least one carrier with a real
ratio, a real level and an envelope that sustains, which is the same guarantee the
FM dice already makes.

## Order of work

1. The state seam, with the existing round-trip checks still passing.
2. `PresetStore` - the bank: listing, reading, saving, deleting, merging over Init.
3. `PresetManager` - the session: load, next/previous, Init, dirty, host programs.
4. `Randomizer` - sections, locks, seeded roll, undo.
5. `MiXXynthPatchAudit` - a third gate, for the patch model.
6. The patch bar: the browser and the roll controls.
7. The factory bank.
8. Docs, then the gates: smoke, both audits, pluginval strictness 5, and the
   standalone.

Each step keeps the plugin buildable and every gate passing.

## Verification

- **`MiXXynthSmoke`** - unchanged; it is the DSP gate and nothing here touches DSP.
- **`MiXXynthEditorAudit`** - the patch bar's controls exist, are sized, and the
  browser is populated from the bank.
- **`MiXXynthPatchAudit`** (new) - the patch model: the bank lists and parses, every
  factory preset's ids exist and its values are in range, every preset is audible,
  save/load/delete round-trip, a load is deterministic, Init restores exactly, the
  roll is reproducible from a seed, a lock holds, an undo puts the patch back, and
  the loader forces Free.
- **`pluginval --strictness-level 5`** - programs are a host-visible surface, so
  this is the gate that says the bank did not break state.
- **The standalone**, by eye and ear, with screenshots kept as evidence.

## Progress

- [x] 1. The state seam (`buildStateTree` / `applyStateTree` / `buildInitState`)
- [x] 2. `PresetStore` - the bank
- [x] 3. `PresetManager` - the session, the dirty marker, the host programs
- [x] 4. `Randomizer` - five sections, locks, a seeded roll, one undo
- [x] 5. `MiXXynthPatchAudit` - a third gate
- [x] 6. The patch bar (`Source/PatchBar.{h,cpp}`)
- [x] 7. The factory bank - thirty presets, eight categories
- [x] 8. Docs and gates

**Gates:** smoke OK; editor audit OK (192 checks, including eleven new ones for the bar);
patch audit OK (53 checks); pluginval strictness 5 SUCCESS.

### What the checks caught, and what it cost to have them

Written down because three of these are the reason the checks exist rather than a formality
that happened to pass:

1. **A preset written with its values as properties of the ROOT loads as an init patch.** All
   thirty presets parsed, listed, and produced silence. APVTS keeps each parameter in a `PARAM`
   CHILD node, so root properties are ignored - no error, no warning, just a bank that does
   nothing. This is now the first thing `docs/PRESETS.md` says, and the patch audit loads every
   preset and compares the result rather than trusting the file.
2. **A parameter stores a NORMALISED value, so a state round trip is lossy.** Writing 4000 Hz
   and reading it back gives 4000.0005. That made the dirty dot light up on its own after a
   load - a marker that cries wolf. The fingerprint now quantises to six significant digits,
   which is far finer than any control's step.
3. **`isInitPatch` compared the live patch against an init patch that cannot exist.** The init
   tree carried the algorithm parameter's own default (0, a DX7 algorithm) while every route in
   forces Free, because M6 removed the algorithm UI. Init now writes Free too, so "the init
   patch" is a patch the plugin can actually be in.
4. **The generator knows every control's range and refuses to write outside one.** It caught
   `Riser`'s four-second attack against an operator attack range of 0..2, and `TimeL` against
   the delay's `Time L` - two mistakes that would have loaded as something else and said
   nothing.

### The one place this departs from the M5 plan

The factory bank is **embedded in the binary** rather than copied to disk on first run. A VST3
installed into a DAW has no repository to copy from, and a disk mirror that is written but never
read is a second source of truth that can drift. The XMLs are still text in the repo. Flagged
for the author rather than applied quietly.

