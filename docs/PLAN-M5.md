# M5 plan — modulation UX, module expansion, presets, visual world

Status: **in progress**. Workstreams 1 (modulation UX) and 2 (the WeirdVerb rename)
are done. Workstreams 3-10 are planned but not started.

Restore point created before any M5 work, and pushed:

| ref | commit |
|---|---|
| `refs/tags/m4b-complete` | `b49de93` (annotated) |
| `refs/heads/stable-m4b` | `b49de93` |
| `refs/heads/master` | `b49de93` |

```sh
git checkout stable-m4b          # look around
git reset --hard m4b-complete    # throw M5 away entirely
```

Nothing in this plan is committed until it passes the same gates as M4b (smoke,
editor audit, pluginval strictness 5, visual confirmation).

---

## Proposed order, and why

The list has a natural dependency order that is not quite the order it was given in:

```
1. modulation UX fixes      independent bug fixes, nothing else depends on them
2. rename Reverb -> WeirdVerb   trivial, but must precede the visual pass
3. the six new modules      the bulk of the work
4. presets                  needs the modules to exist to be worth saving
5. visual world (Impeccable)    LAST, so every module is styled once
```

**The visual pass goes last on purpose.** If the faceplate lands first, every one
of the six new modules has to be styled as it is written and then re-checked
whenever the design moves. Styling once at the end costs one pass instead of
seven. It also lets the Impeccable `new-work` flow see the finished surface, so
the world it establishes actually fits what it is dressing.

If you would rather see the faceplate sooner, say so and I will reorder — but I
would want to freeze the design first and build the modules to it.

---

## Workstream 1 — modulation UX

Two separate problems, both confirmed in the code rather than guessed at.

### 1a. The Matrix tab does not sync with drag-and-drop

**Root cause found.** `MiXXynthEditor::MatrixPage::SlotRow` (`Source/PluginEditor.cpp:985`)
binds its source and destination `ComboBox`es **once, in its constructor**, with
`dontSendNotification`, and has no refresh path at all:

```cpp
src.setSelectedId (srcParam->getIndex() + 1, juce::dontSendNotification);
```

Dropping a chip on a knob calls `assignModSlot`, which writes the `mod{n}src` and
`mod{n}dst` parameters. The parameters change; the combos never re-read them. So
the Matrix tab goes on showing `Off` for a slot that is now live. The row's depth
knob has the same problem, and it is not in the editor's refresh set either.

**Fix.** Give `SlotRow` a `poll()` that re-reads all three of its parameters and
updates its controls, guarded so it cannot clobber a combo the user has open or a
knob mid-drag. Call it from `MatrixPage::poll()`, which the editor timer already
runs at 30 Hz. The reverse direction — editing the Matrix tab updating the knob
rings — should already work, because `refreshModRings()` polls the parameters
every tick, but that needs verifying rather than assuming.

**Regression test.** A round trip in the audit: assign through the drag-drop code
path, assert the Matrix row reflects it; then edit the Matrix row, assert the
knob's ring assignment reflects that.

### 1b. The modulation amount ring

A ring already exists and is already draggable, so this is less broken than it
looks — but it is genuinely hard to discover and cannot do everything:

- **Only the first source is draggable.** `mouseDown` does
  `ringDragSrc = assignments.front().source`, so a knob with two sources can only
  have its first one adjusted by dragging. The others are menu-only, which is
  exactly the complaint.
- **The band is thin.** `ringWidthFor()` is `jlimit (5, 13, shortSide / 7)` px.
- **The arc does not read like Serum's.** Serum and Vital draw the modulated range
  *from the knob's current value* outward, so you can see at a glance where the
  knob sits and how far the modulation will push it. Ours draws the depth without
  that anchor.

**Proposed.**
- One hit-testable arc **per assigned source**, each highlighted on hover, so any
  of them can be dragged — not just the first.
- Draw each arc from the knob's current value outward to `value + depth`, which is
  the Serum convention and makes the range legible without a tooltip.
- Widen the grab band and enforce a minimum hit target, so it is comfortable on
  the smallest knobs.
- Keep the right-click menu for numeric entry, and for removing one source.
- Keep the existing live readout while dragging.

**Regression test.** A knob with two sources: dragging the second arc must change
the second slot and leave the first alone.

---

## Workstream 2 — rename Reverb to WeirdVerb

**Done.** A full rename, as chosen: class, files and display name, because more
reverb modules are coming and `ReverbFx` would have been ambiguous next to a
convolution reverb.

| was | is |
|---|---|
| `Source/Fx/ReverbFx.{h,cpp}` | `Source/Fx/WeirdVerbFx.{h,cpp}` |
| `class ReverbFx` | `class WeirdVerbFx` |
| `mixxynth::fxReverb` | `mixxynth::fxWeirdVerb` |
| `fxTypeName()` -> `"Reverb"` | -> `"WeirdVerb"` |

**State compatibility is safe.** A slot's type is stored as a *choice index*, not
a name, so the entry moved nowhere and sessions saved before the rename still
load with the module in the same slot.

The audit needed no changes for this: it asserts on the type *index* and on the
descriptor, not on the name string, so it passed unchanged. The card title and
the `+` menu both read `WeirdVerb` now, confirmed visually.

---

## Workstream 3 — convolution reverb

The rack currently has **no way to put a file into a module**. The card supports
choice boxes and knobs only, because `FxParamInfo` only describes those two kinds.
Convolution therefore needs a third control kind first:

- `FxParamInfo` gains a file kind (label, picker filter, whether an IR is loaded)
- The card gains a row with a "Load IR…" button and the current file's name
- `FxRack` gains a way for a module to be handed a file, on the message thread

**Engine.** `juce::dsp::Convolution` (MIT, ships with JUCE) already does
partitioned FFT convolution with a background loader thread, which is the correct
answer for real-time safety. Hand-rolling it means re-solving the same threading
problem with more ways to get it wrong. I propose using it, and only hand-rolling
later if we need something it cannot do (zero-latency mode, IR morphing, or
blending two IRs).

**The hard question is the IR itself.** A convolution reverb is nothing without
impulse responses, and where they come from changes the design:

| Option | Consequence |
|---|---|
| Path only | The preset breaks the moment the IR moves or is on another machine |
| Embed the IR in the preset | Works anywhere, but one IR can be 1-10 MB and the state is XML |
| Bundled library, referenced by ID | Robust and small, but we have to ship and license IRs |

I lean toward **path plus a bundled starter library**, with an opt-in "embed this
IR in the preset" toggle for people who want portable patches.

Controls: Pre-Delay, Attack (IR start trim), Length (IR tail trim), Low Cut,
High Cut, Width, Mix, Normalise.

---

## Workstream 4 — multi-mode distortion, warm

"Warm" is the brief, and it is a real engineering constraint rather than a label.
The things that make distortion sound cold and cheap are exactly the things that
are easy to do by accident: aliasing, a hard-knee transfer curve, and harmonics
with no even-order component.

So the design is built around those:

- **Oversampling** (2x / 4x / 8x, with a proper polyphase half-band FIR, not naive
  upsampling). This is most of "warm" on its own.
- **Pre-emphasis before the non-linearity and de-emphasis after it**, so the drive
  is frequency-aware the way a tube stage or a tape head actually is.
- **Asymmetry / bias** so even harmonics appear, which is the single biggest
  difference between "warm" and "buzzy".
- **DC blocker** after the non-linearity, because asymmetry generates DC offset.
- **A gentle tone stack**, not a tilt EQ: a low shelf and a high shelf around the
  drive stage.

Modes: **Tube, Tape, Fuzz, Diode, Fold, Crush.** Each is a different transfer
curve plus its own pre/post emphasis; they are not one curve with more gain.

Controls: Mode, Drive, Bias, Tone, Low Cut, Mix, Output, Oversample.

---

## Workstream 5 — delay, with stereo ping-pong

- Two delay lines, per-channel time, with **cross-feedback** for ping-pong. The
  cross amount is a control rather than a switch, so the range runs from two
  independent delays, through partial ping-pong, to strict L/R/L/R bouncing.
- **Tempo sync** reusing the existing `syncDivisionCyclesPerBeat()` and the host
  playhead, which the processor already reads for LFO sync — so this is reuse, not
  new plumbing.
- **Damping inside the feedback path**, so repeats darken as they decay. This is
  what makes a delay sit in a mix instead of cluttering it.
- **Modulation** on the delay time; a static digital delay is the giveaway of a
  cheap one.
- Stereo width, and a Mix that leaves the dry signal alone.

Controls: Sync, Time L, Time R, Feedback, Ping-Pong, Damping, Mod, Width, Mix.

---

## Workstream 6 — Juno chorus (ensemble)

First, a correction to the premise, because it changes the design: **the Juno
chorus is not an ensemble.** The Roland Juno-6 / 60 / 106 chorus is a *single*
bucket-brigade delay line with a **triangle** LFO and a fixed delay of roughly
5 ms. A true ensemble effect (Solina, ARP) uses *three* delay lines with
phase-offset LFOs. The Juno's modes are I (slow, deeper), II (faster) and, on the
106, I+II.

What actually gives it its sound, and what the implementation has to get right:

- **A triangle LFO, not a sine.** This matters more than anything else. A sine
  gives a generic chorus; a triangle gives *that* Juno shimmer.
- **A short fixed delay (~5 ms) with no feedback.** Feedback turns it into a
  flanger.
- **A dark wet path.** The BBD's bandwidth is roughly 6–8 kHz, and the compander
  around it adds a characteristic softness.
- **Mode-dependent rate and depth**, not a free rate knob, if we want the actual
  Juno behaviour rather than a chorus that happens to be blue.

Controls: Mode (I / II / I+II), Rate, Depth, Tone, Mix.

---

## Workstream 7 — presets

- A preset bar: dropdown of names, prev/next arrows, Save, Save As, Delete, Init.
- **Storage**: XML under `%APPDATA%/MiXXynth/Presets/`. The state already
  round-trips everything that matters — the audit proves the shapes and the whole
  FX rack survive a save/load — so this is mostly UI plus file handling rather
  than new serialisation.
- **Factory presets**: a folder in the repo, copied next to the user presets on
  first run, listed in the same dropdown and marked as factory (not overwritable).
- A **dirty marker** so you can see the patch has been edited since load.
- Host-visible programs are a separate mechanism; I would wire
  `getNumPrograms`/`setCurrentProgram` to the same store so DAWs see the presets
  too, rather than maintaining two lists.

Open question: how many factory presets, and do you want to author them or should I?

---

## Workstream 8 — flanger

Short modulated delay with feedback, which is the whole difference between it and
the chorus above. The parts that decide whether it sounds like a flanger or like a
broken chorus:

- **A short delay range** (~0.5–8 ms) and a **wide LFO depth**, so the comb notches
  sweep far enough to be heard as a jet rather than a wobble.
- **Signed feedback.** Positive feedback gives the classic hollow sweep; negative
  gives the thinner, more metallic one. Both are useful and the polarity is a
  control, not a hidden constant.
- **Stereo phase offset** on the LFO, so the two channels sweep apart.

Controls: Rate, Depth, Feedback, Polarity, Delay, Stereo, Mix.

---

## Workstream 9 — phaser

Cascaded first-order allpasses swept by an LFO. The stage count is the main
character control and should be a proper selector rather than a continuous knob
pretending to be one.

- **2 to 12 stages**, even and odd both allowed (odd counts give a different notch
  pattern and are worth having).
- **Feedback** around the allpass chain, which sharpens the notches and is most of
  what separates a lush phaser from a weak one.
- **Stereo**: LFO phase offset, and optionally a stage-count offset between
  channels.
- A **centre** control so the sweep can be placed in the spectrum rather than
  always covering the same range.

Controls: Stages, Rate, Depth, Feedback, Centre, Stereo, Mix.

---

## Workstream 10 — the visual world

I engaged the Impeccable agent, and its context probe returned a hard gate:

> `PRODUCT_INIT_REQUIRED`: No product context or visual authority was found. New
> builds and redesigns must finish `reference/init.md` for PRODUCT.md, then
> `reference/new-work.md` establishes the world and surface.

and, on autonomy:

> Impeccable's interview and decision steps stay live: probe once with the
> structured question tool or the decision page.

So this workstream starts with an **interview** (`impeccable init`) that writes
`PRODUCT.md`, then `new-work` establishes the visual world. I will not hand-write a
faceplate before that, because doing so would just be my taste dressed up as a
design system, and the whole point of involving Impeccable is to not do that.

### On "custom knobs from online and open-source textures"

I want to flag a real risk before we commit to it. Raster knob and faceplate assets
sourced from the internet are the one part of this project with a **licensing
exposure that code review cannot catch**. "Open source" is not a licence; CC0,
CC-BY and CC-BY-NC all mean different things, and a plugin that ships a CC-BY-NC
texture is not MIT any more. Attribution requirements also leak into the About box
and the README.

There is also a craft argument. A raster knob is fixed-resolution and fixed-palette;
it has to be re-cut for every size, and it fights the existing sst-jucegui widgets
rather than fitting them. The alternative:

| Approach | Licensing | Scales | Theming | Cost |
|---|---|---|---|---|
| Procedural in code (JUCE paths, gradients, noise) | none | perfect | free | more code, no external art |
| CC0 raster assets, vendored | none, but verify each | poor | re-cut per theme | asset prep + attribution ledger |
| CC-BY raster assets | attribution required | poor | re-cut per theme | same, plus legal text |

My recommendation is **procedural for the knobs and the faceplate texture, with
optional CC0 raster only for the faceplate background**. That gives a
resolution-independent, themeable, zero-exposure result that can still look
expensive — brushed metal, anodised panels and subtle noise are all achievable with
gradients and a noise texture generated at load.

You have given permission to install tooling for the GUI work. That unblocks the
raster route if you want it: Impeccable reported `IMAGE_TOOLS: no image converter
found (cwebp, sips, magick, ffmpeg)`, so I would install ImageMagick to cut,
re-tint and downsample assets. I will not install anything until you have chosen
the route, since the choice decides whether it is needed at all.

### What the visual pass has to cover

The editor is one 2300-line file with six tab pages plus a sidebar, so the pass is
not just a repaint:

- a faceplate/panel system, and a dark-metal or anodised treatment for the shell
- the knob family (dial, cap, indicator, ring) as a coherent set at every size
- the sidebar shape strips, which are drawn by hand and are the most distinctive
  part of the UI today
- the tab bar, macro bar, keyboard and wheels
- the new FX cards, which will be the busiest surface once six modules exist
- focus/hover/disabled states, since none of that is systematic right now

---

## Decisions

Answered by the project owner. These are settled, not open.

1. **Work order.** Mod fixes → rename → the six modules one at a time → presets →
   visual pass last, so every module is styled once. *(chosen)*
2. **Convolution reverb IRs.** A **bundled starter library**, with the ability for
   the user to add their own. **Embedding the IR in a preset is the preferred
   behaviour**, so patches are portable. Consequence to design for in WS3: the
   state has to carry IR audio data, which means a preset can be megabytes rather
   than kilobytes, and the embedding has to be deliberate (base64 or a binary
   child node) rather than an accident of serialisation.
3. **Juno chorus.** **Hardware emulation**, not a flavoured chorus. Mode-dependent
   rate and depth as the hardware has them, because the specific sound is the
   point.
4. **Presets.** About **thirty placeholder factory presets**, authored by MiXX,
   listed in a dropdown inside the plugin.
5. **Visual assets.** **Procedural for now.** Specific downloaded images can come
   later if a texture genuinely needs one. This means no ImageMagick install is
   needed yet, and no licensing ledger.
6. **Rename depth.** **Full rename** — class and files as well as the display name,
   because more reverb modules are coming and `ReverbFx` would be ambiguous.
7. **Module scope per turn.** **One module at a time**, each fully gated and
   committed, so every one has its own tests and a clean revert point.

---

## Verification strategy

Same gates as M4b, applied to every workstream:

- **`MiXXynthSmoke`** for DSP. Every new module gets: finite and bounded output,
  the controls doing what they say (monotonic where that is meaningful), and the
  specific failure modes — distortion aliasing and DC offset, delay ping-pong
  routing, chorus LFO shape, flanger feedback stability, phaser notch depth,
  convolution tail length and IR-missing behaviour.
- **`MiXXynthEditorAudit`** for wiring. The new round-trip checks for modulation
  sync, every new parameter reachable, no control at zero bounds, and state
  round-trips for every new module including the convolution IR reference.
- **`pluginval --strictness-level 5`** on the VST3.
- **Visual confirmation** in the standalone, with screenshots kept as evidence.
- **A clean commit per workstream**, so any single one can be reverted without
  unpicking the others.

`stable-m4b` / `m4b-complete` stay untouched as the known-good fallback for the
whole of M5.
