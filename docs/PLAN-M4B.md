# MiXXynth — M4b Plan

Five workstreams from the request. This file is the plan of record: what changes,
in what order, why that order, what each piece costs, and what is still undecided.

## Scope

| # | Request | Kind |
|---|---|---|
| 1 | LFOs and ENVs become add/remove **modules** (default 1 LFO + 1 ENV, scrollable, stays in the sidebar) | Mod-system refactor + UI |
| 2 | FM tab knobs default to **0**, plus a **dice** button that randomises every FM-tab knob | Defaults + UI |
| 3 | Operators can be **toggled on/off**; each gets a **wave shape knob** that smoothly interpolates between the basic shapes | DSP + params + UI |
| 4 | FX **rack**: add / rearrange / remove modules; empty by default; `+` opens a module picker | New architecture |
| 5 | First module: a **reverb modelled on the Strymon blueSky** (plate / room / spring; decay with infinite tail, pre-delay, tone, mod, mix) | New DSP + UI |

## Ordering, and why

```
3  Operators: on/off + wave shape      <- additive, zero regression risk, no open questions
2  FM tab: zeroed defaults + dice      <- same file area as 3, do it while we are there
1  Dynamic LFO/ENV modules             <- refactor of the mod system the sidebar already uses
4  FX rack framework                   <- greenfield architecture, must exist before 5
5  blueSky-style reverb module         <- first rack module, depends on 4
```

**Why operators first.** Workstreams 2 and 3 both touch the FM tab and the per-op
defaults, so doing 3 then 2 avoids editing the same code twice. Workstream 3 is
purely additive - new parameters default to the current behaviour (every operator
on, every wave a pure sine) - so it cannot regress anything, which makes it the
safe place to start. Workstream 2 *changes* existing defaults and is the one with a
genuine open question (below), so it should not lead.

**Why the rack before the reverb.** The reverb is a module *in* the rack. Building
it first would mean writing it twice: once standalone, once as a module. The rack
framework is also the part with the most architectural risk, so it wants the most
runway.

**Why modules before the rack.** They are independent, but the LFO/ENV module
system and the FX rack solve the same problem twice (dynamic, add/remove,
serialised, reorderable units). Doing LFO/ENV first means the rack can reuse the
pattern rather than inventing a second one.

---

## Workstream 3 — Operators: on/off + wave shape

### New parameters (12)

| ID | Name | Type | Default |
|---|---|---|---|
| `op{n}on` | Op n On | bool | **true** |
| `op{n}wave` | Op n Wave | float 0..1 | **0.0** (sine) |

`op{n}wave` is a single continuous control, not a choice: it sweeps across the
shape set so the shapes "smoothly interpolate" as asked.

Shape set and knob positions:

```
0.000      0.333      0.667      1.000
sine   ->  triangle ->  saw   ->  square
```

### DSP

- **Morph domain: harmonic, not time.** Each shape is defined by its harmonic
  amplitude/phase series (sine: h1 only; triangle: odd h, 1/h^2; saw: all h, 1/h;
  square: odd h, 1/h). Interpolating in the harmonic domain keeps the shapes
  phase-coherent, so a morph does not produce the cancellation notches a naive
  time-domain crossfade gives between, say, a saw and a square.
- **Band-limiting.** One table per shape, built at `prepareToPlay`, with a
  harmonic count chosen from the operator's frequency (a small mip set, one table
  per octave band). This matters because an operator can run at ratio 16 on a high
  note, and a naive saw there would alias badly. Sine stays analytic (cheap and
  exact); the other three use the tables.
- **`op{n}on` = the operator outputs silence**, so it stops modulating its
  destinations and stops contributing if it is a carrier. That is the least
  surprising reading of "off" and it falls out of the existing routing with no
  special cases.
- Keep the existing per-op envelope and feedback paths untouched.

### UI

- FM tab: an **On** toggle per operator (next to the Op header).
- FM tab: a **Wave** knob per operator, labelled with the nearest shape name so it
  reads as a continuum, not a mystery dial.
- Wave and On are **not** modulation destinations (matching how Ratio/Level are the
  only per-op destinations today); stated here so it is a decision, not an oversight.

### Cost

Medium. The table generation is the bulk of it. Roughly: one new header/source pair
for the shape tables, edits to `FmVoice.{h,cpp}`, 12 params, FM-tab controls, and
smoke tests asserting each shape is finite, bounded and audible, and that morphing
is continuous (no discontinuity between adjacent knob positions).

---

## Workstream 2 — FM tab: zeroed defaults + dice

### Defaults

Every knob in the FM tab starts at 0:

| Param | Now | Proposed |
|---|---|---|
| `fmIndex` | 3.0 | **0.0** |
| `op{n}ratio` | 1.0 | **0.0** |
| `op{n}level` | 0.8 | **0.0** |
| `op{n}feedback` | 0.0 | 0.0 (already) |
| `op{n}attack/decay/sustain/release` | 0.01 / 0.10 / 0.90 / 0.20 | **0.0** |

**This makes the default patch silent.** That is what "all knobs at 0" means
literally, and it is a real decision - see the open question below. Everything else
in the plugin (master gain, macros, shapes, FX) keeps its own default, so the synth
loads silent but not *broken*, and the dice button is the intended way out.

Note `op{n}ratio` at 0 is below its 0.25 minimum, so either the range opens up to
0.0, or ratio 0 is clamped to 0.25 and the knob lies about its range. Ratio 0 is
musically meaningless, so I would rather let the knob sit at 0 and have the DSP
treat `ratio <= 0` as "operator produces no pitch" than misrepresent the range.

### Dice button

- One button in the FM tab header.
- Randomises every knob in the FM tab: `fmIndex`, and per operator `ratio`,
  `level`, `feedback`, `attack`, `decay`, `sustain`, `release`, plus `wave`. Each
  within a musically sensible sub-range rather than 0..max:
  - `ratio`: snap to a small set of musical ratios (0.5, 1, 1.5, 2, 3, 4, 5, 7)
  - `level`: 0..1, biased high enough that something sounds
  - `feedback`: 0..0.7 (above that it screams)
  - `attack`: 0..0.5 s, `decay`: 0.05..2 s, `sustain`: 0..1, `release`: 0.05..3 s
  - `fmIndex`: 0..12, `wave`: 0..1
- It must **guarantee audibility**: force at least one operator to be a carrier
  with a non-zero level, otherwise a roll can land on silence and read as a bug.
- It writes through the normal parameter API (`beginChangeGesture` /
  `setValueNotifyingHost` / `endChangeGesture`) so the host sees it as one
  undoable edit per parameter.
- The random source is seedable so a smoke test can assert the invariants (every
  value in range, at least one audible carrier, no NaN).

### Cost

Small once the default question is answered.

---

## Workstream 1 — LFO and ENV as add/remove modules

### What changes

Today `kNumShapes = 7` is a compile-time constant (4 LFOs + 3 ENVs), the sidebar
builds exactly 7 strips, and the mod source enum hard-codes `srcLfo1..4` and
`srcEnv1..3`.

It becomes a **dynamic list**:

- `ModShape` gains a `kind` (LFO or ENV). The two default shapes already exist
  (`ModShape::defaultLfo()` / `defaultEnv()`); the sidebar just stops assuming a
  fixed count.
- The **shape list lives in the state `ValueTree`** under `shapes`, exactly where
  the shapes already live, so this needs no new serialisation mechanism - just a
  variable-length child list instead of a fixed array.
- **Default: one LFO and one ENV.**
- Each strip gets a **`-` button** to remove it. A **`+ LFO`** and **`+ ENV`**
  button sit under the last strip of each kind, so new modules appear underneath
  their own family, as asked.
- The sidebar keeps its position (sibling of the tabs, always visible) and keeps
  scrolling; only the content height changes.

### The hard part: modulation source indices

The mod matrix stores `source` as an **int index into a fixed enum**, and 32 slots
may reference `srcLfo3`. With a dynamic list, indices shift when you remove a
module, which would silently repoint every slot.

Three options:

| Option | Behaviour | Cost |
|---|---|---|
| **A. Stable IDs** | Each module keeps a permanent ID; slots store the ID, not the position | Correct, more plumbing |
| **B. Fixed pool, hidden** | Keep 8 LFO + 8 ENV slots always; `+`/`-` only show/hide. Indices never move | Simple, wastes params, "add" is cosmetic |
| **C. Renumber + remap** | On removal, rewrite every slot that referenced the removed module to `Off`, and shift the rest | Simple, but editing someone's patch as a side effect |

**Recommendation: A.** Slots store a module ID. This is the only option where
adding and removing modules cannot silently corrupt a patch, and it is the same
mechanism the FX rack will need, so it pays for itself twice.

Source naming then comes from the module list (`"LFO 1"`, `"LFO 2"`, `"Env 1"`),
numbered **by position within its own kind**, so removing LFO 1 relabels LFO 2 as
"LFO 1" in the UI while its ID (and every slot pointing at it) is untouched.

### Open questions

- Maximum modules (proposed: 8 LFO + 8 ENV).
- Can you remove down to **zero** of a kind, or is a minimum of 1 enforced?
  Proposed: minimum 0 for LFOs and ENVs alike, since the `+` button is right there.
- Do the sidebar's performance chips (Macro 1-4, Velocity, ...) stay fixed? They
  are not modules, so proposed: yes, unchanged.

### Cost

Large-ish. The DSP side is small (the shape engine is already per-voice and
list-driven); the work is the ID plumbing, the dynamic UI, and re-pointing the
mod source choice lists, which are APVTS `AudioParameterChoice` with a **fixed**
list of names. That last point is the real constraint: a choice parameter's items
cannot change at runtime, so the choice list must either be the fixed maximum
(`"LFO 1".."LFO 8"`, `"Env 1".."Env 8"`) with unused entries shown greyed, or the
matrix UI must move off choice parameters. Proposed: **fixed maximum-length choice
lists**; that keeps the matrix fully automatable and host-visible, and the sidebar
simply controls which entries are meaningful.

---

## Workstream 4 — FX rack

### Architecture

The rack sits at the **end of `processBlock`**, after the voice mix and before the
output — there is currently no post-processing at all, so this is greenfield.

**Fixed slot pool, not a growable array.** APVTS parameters are created once at
construction and cannot be added later, so a dynamic rack must be a **fixed pool of
slots** with a per-slot `type` parameter. Proposed: **8 slots**.

Per slot:

| Param | Purpose |
|---|---|
| `fx{n}type` | choice: `Off`, `Reverb`, ... (grows as modules are written) |
| `fx{n}bypass` | bool |
| `fx{n}p1..fx{n}p8` | 8 generic float params, meaning assigned by the module |

Generic `p1..p8` is the pragmatic choice: a module declares how it maps its own
controls onto them, and the UI labels them per type. The alternative — named
per-module parameters — needs a param per control per module, and the pool has to
be sized for the worst case anyway.

**Default: all slots `Off`, so the rack starts empty**, as asked.

### UI

- An empty rack shows just a `+` button and a short line of text.
- `+` opens a menu of available modules (initially just `Reverb`).
- Each module renders as a **card**: title, bypass toggle, its own knobs, and two
  arrows plus a `-`.
- **Rearrange with `^` / `v` buttons** rather than drag-and-drop. Reason: drag
  reordering inside a scrollable viewport with a live audio graph behind it is a
  lot of fiddly hit-testing for no functional gain, and the arrows are
  unambiguous. (If you want drag, say so and I will do drag.)
- Moving a module swaps the whole slot's contents, so a module's settings travel
  with it.

### Signal flow

```
voices -> [slot 0] -> [slot 1] -> ... -> [slot 7] -> master -> out
```

Each slot is skipped when its type is `Off` or bypassed, so an empty rack is a
pure passthrough (asserted in the smoke test).

### Cost

Large. This is the biggest single piece of plumbing: slot pool, module base class,
generic param mapping, the card UI, reordering, and serialisation (which rides in
the APVTS because the params are real parameters).

---

## Workstream 5 — blueSky-style reverb module

### Research: what the pedal actually is

From Strymon's own product page and manual for the blueSky V2:

- **Three reverb types: `plate`, `room`, `spring`.** Strymon's words: "each type
  can produce everything from stunning realism to expanded sounds far beyond what
  would be physically possible, while always remaining musical."
- **Decay** — "controls the duration of the reverberated signal's decay."
- **Mix** — "the balance of the dry and wet signals from 100% dry at minimum to
  100% wet at maximum, and a 50/50 split when the knob is at 3 o'clock."
- **Pre-Delay** — pre-delay before the reverb.
- **Tone** — "controls the frequencies within the reverb, with a damped high end at
  minimum to no damping at maximum." So Tone is a **high-frequency damping**
  control, not a tilt EQ.
- **Mod** — on the hardware this is a **3-position switch: off / light / deep**.
- Also on the hardware, **not requested**: a dedicated **Shimmer** knob, and a
  Boost/Cut live-edit function.
- 32-bit float DSP, 20 Hz–20 kHz.

**Deviation from hardware, deliberate:** the request asks for a **Mod knob**
("a mod knob for adjusting the mod of the reverb"), where the pedal has a
3-position switch. A continuous knob is strictly more capable and matches the rest
of the panel, so the module gets a continuous Mod control.

**Not implemented:** Shimmer (not requested). The mode knob covers
plate / room / spring. Shimmer is the obvious next addition, since it is the
pedal's signature extra and it is a pitch-shifted feedback path.

### Algorithms

Strymon do not publish their DSP, so these are **independent implementations of the
classical algorithms each reverb type is built from**, not clones:

| Mode | Basis | Character |
|---|---|---|
| **Plate** | Dattorro-style plate (JAES 1997, "Effect Design Part 2"), which is published and freely usable: input diffusion (allpass chain) -> a tank of modulated delay lines -> allpass diffusion -> damping -> output taps | Dense, bright, immediate, minimal early reflections |
| **Room** | Feedback delay network: 8 delay lines, lossless (Hadamard) mixing matrix, per-line damping and delay-length spread | Size-controllable, audible early reflections, natural |
| **Spring** | Dispersive delay line: a chain of allpass filters giving the chirp, plus a modulated delay line with feedback, band-limited to the spring's range | Boingy, mid-focused, distinctive chirp |

All three share one skeleton so the controls behave consistently:

```
in -> pre-delay -> input diffusion -> [mode-specific tank] -> tone -> mix -> out
```

- **Decay** scales the tank's feedback gain. **At maximum it becomes a true
  infinite tail**: feedback is set to exactly 1.0 with the lossless mixing matrix,
  which conserves energy, so the tail sustains rather than decaying. A DC blocker
  and a safety clamp sit in the loop so it cannot run away.
- **Pre-Delay** is a delay line before the diffusion stage, 0–250 ms.
- **Tone** is a one-pole lowpass in the feedback path, from heavily damped to open.
- **Mod** modulates the tank's delay-line lengths with a slow, decorrelated LFO per
  line, which is what stops a long tail ringing metallically. At 0 it is off.
- **Mix** is a proper dry/wet crossfade, with 50/50 at 3 o'clock to match the pedal.

### Module mapping onto the rack's generic params

| Knob | `p` |
|---|---|
| Mode | `p1` (choice 0..2) |
| Decay | `p2` |
| Pre-Delay | `p3` |
| Tone | `p4` |
| Mod | `p5` |
| Mix | `p6` |

`p7`/`p8` stay spare for a future Shimmer knob.

### Cost

Large. A reverb is the most involved DSP in the project so far: 8 delay lines with
fractional interpolation, a mixing matrix, per-line damping and modulation, plus
three distinct tank configurations. Plan for a new `Source/Fx/` area with one file
pair per module and a shared `FxModule` base.

---

## Open questions

Numbered so they can be answered individually.

1. **FM defaults: silent on load?** "All knobs at 0" literally means the default
   patch makes no sound, because `fmIndex` 0 and every `op level` 0. Options:
   - (a) **Literally all zero** - silent default, dice is the way out. What was asked.
   - (b) **All zero except `op1 level`** - one operator sounds, so the synth is
     never dead on arrival.
   - (c) **Zero the modulation knobs only** (`fmIndex`, `feedback`, `ratio`) and
     leave levels and ADSR at their current defaults.
2. **Ratio range** - open it down to 0.0 so the knob can genuinely sit at 0, or
   clamp at 0.25 and let the knob read 0.25? (Proposed: open to 0.0.)
3. **LFO/ENV maximums** - proposed 8 LFO + 8 ENV. Enough?
4. **LFO/ENV minimums** - can you remove down to zero of a kind? (Proposed: yes.)
5. **Mod source stability** - do you accept the stable-ID approach (Option A) for
   the mod matrix? It is the only option that cannot corrupt a patch on removal.
6. **FX slot count** - proposed 8. Fine?
7. **FX reorder gesture** - `^`/`v` buttons (proposed) or drag-and-drop?
8. **Wave shape set** - sine / triangle / saw / square as the four stops? Should the
   knob also reach pulse-width or noise, or stop at four?
9. **Wave morph** - should the knob *morph* (continuous blend, proposed) or *step*
   between the four shapes?
10. **Dice scope** - FM tab only (proposed, as asked), or also the algorithm and the
    matrix routing?

## Verification strategy

Every workstream lands with the same gates as the existing code:

- **`MiXXynthSmoke`** for DSP: each op wave finite, bounded and audible; morph
  continuity; empty rack is a bit-exact passthrough; each reverb mode stable;
  infinite decay does not run away; pre-delay and mix behave.
- **`MiXXynthEditorAudit`** for wiring: new parameters reachable from the UI, new
  knobs wired to the right destinations, module add/remove/reorder preserves
  patches, no control left at zero bounds.
- **`pluginval --strictness-level 5`** on the VST3.
- Visual confirmation in the standalone.

## Status

- [x] 3 Operators: on/off + wave shape
- [ ] 2 FM tab: zeroed defaults + dice
- [ ] 1 Dynamic LFO/ENV modules
- [ ] 4 FX rack
- [ ] 5 blueSky-style reverb

### Workstream 3, as built

- `Source/OpWave.{h,cpp}`: band-limited tables, 4 shapes x 8 octave bands, 2048
  samples each, built in `prepareToPlay`. Morph is harmonic-domain, so it is
  phase-coherent and continuous (measured worst jump 0.003 for a 0.001 knob move).
- The mip band and morph pair are resolved **once per block** into an
  `OpWaveTables::Cursor`; the per-sample path is two table reads and a lerp, with
  no `log2` in the hot loop.
- One shared table set owned by the processor, pointed at by `VoiceParams`. Giving
  each of 64 voices its own copy would have been ~16 MB.
- `op{n}on` (bool, default true) and `op{n}wave` (0..1, default 0) added; a
  disabled operator outputs silence, so it stops modulating downstream too.
- FM tab: per-op On toggle plus a Wave knob, 8 knobs per column.
- 210 params, 127 knobs, audit OK, smoke OK (13 new checks), pluginval strictness 5
  SUCCESS.
