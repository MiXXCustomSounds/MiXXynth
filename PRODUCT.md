# Product

<!-- impeccable:product-schema 1 -->

## Platform

native desktop - a JUCE audio plugin (VST3 / AU / Standalone).

Recorded as what it is rather than as one of the schema's web/ios/android/adaptive values, because
none of those describe a plugin that runs inside a host's window. Anything downstream that assumes
a browser, a viewport, or a device class is wrong for this product.

## Users

The author and a small circle of friends and collaborators. Musicians who already play synths and
will be shown the instrument in person.

Some teaching is warranted; discoverability is not the binding constraint. The people using this
can be told where a thing lives. What they cannot be told is how to enjoy using it - so the bar is
*pleasure of use* rather than *zero-instruction*.

## Product Purpose

Complex FM without the UI that makes complex FM unapproachable.

The sound of a six-operator FM engine with a free routing matrix, a virtual-analogue layer,
per-operator filters and a deep modulation system - reachable by someone who has bounced off a
DX7-style interface before. Success is that the depth is still there and the interface stops being
the reason people give up.

The second half of the purpose: taking old hardware and putting it through modern effects. The
vintage character is the raw material; the modern effects are what you do with it.

## Positioning

**Retro-futurism: it looks and sounds both old and new, on purpose.**

That is the claim a neighbouring product could not truthfully copy without becoming this one. A
vintage emulation plugin can copy the materials. A modern synth can copy the depth. What it cannot
copy is a product that is genuinely both and treats the combination as the point rather than as a
compromise - vintage materials wrapped around an engine that no vintage instrument had.

The mechanism behind it: an unusually deep engine (six-operator FM, free matrix, VA layer,
per-operator filters, 32-slot modulation matrix with editable breakpoint shapes, twelve effects)
presented with the confidence of hardware. Depth is not hidden; it is *housed*.

## Operating Context

A DAW, on a desktop, by a musician at a keyboard. The plugin is loaded as an instrument and played.

Repository facts that shape how it can be built:

- MIT licensed. No raster assets sourced from the internet - provenance code review cannot verify,
  and one CC-BY-NC texture would end the MIT claim. Materials must be generated procedurally.
- Preset compatibility is explicitly **not** a constraint. Nothing shipped is depended upon.
- Development and verification run on Windows with MSVC; the build has a smoke suite, an editor
  audit, and pluginval at strictness 5.

## Capabilities and Constraints

**Capabilities.** Six-operator FM with a free routing matrix; a VA layer with its own envelope and
sub/noise; per-operator filters (cutoff, resonance, drive, own envelope, Out/Mod/Both routing) that
are modulation destinations; a 32-slot modulation matrix over four editable LFOs, three editable
envelopes, four macros and the usual performance sources; MPE; twelve effect modules (WeirdVerb,
Convolution, Juno Chorus, Distortion, Ping-Pong Delay, Flanger, Phaser, Compressor, Limiter,
Multiband, Graphic EQ); an output safety limiter; a Snap/Free switch that decides whether controls
land on musical values or move freely.

**Constraints.** Everything drawn procedurally in JUCE; no external art. Four parameters per FX
slot, fixed at construction, so an FX module declares a bounded set.

**Undecided.** Presets are designed but not built. Undo/redo is not wired. The graphical EQ's curve
is drawn but not draggable.

## Brand Commitments

The name **MiXXynth**.

The identity, in the author's words: *a synth for people who want to take old hardware and put it
through modern FX. The sounds of complex FM, without the impossible to approach UI. The epitome of
retro-futurism. It looks and sounds both old and new.*

Binding material constraints the author named directly, recorded as given and not expanded:

- the left scrollable panel slightly darker than the rest, with rounded dark-oak edges
- square buttons with slightly rounded corners and a darker border so they lift off the ground
- a dark metallic texture behind everything
- decorative silver screws at the edges of faceplate panels
- FX tab: silver metal rack rails with holes, modules sitting on them, dark cloth behind
- the VA section and the ADSR section stylised as each other

## Evidence on Hand

The repository itself: working DSP, a smoke suite, an editor audit, and a pluginval-clean build.
`docs/` holds the design record across M4b, M5 and M6.

**No** testimonials, press, benchmarks, user counts, pricing or release claims exist. Future work
must not fabricate any of them.

## Product Principles

1. **Depth is the point; the interface is the door.** Never simplify the engine to make the UI
   easier. Make the UI better instead.
2. **Old materials, new capability.** The vintage references are load-bearing, not decoration - and
   they house an engine no vintage instrument had. Neither half is apologised for.
3. **Every control earns its place by being usable.** A parameter with no control is a bug, not an
   oversight; the audit refuses one, and it should stay that way.
4. **Own it like hardware.** The visual language is equipment built to be kept and repaired, not a
   screen that will be replaced. Permanence is the feeling being bought.
5. **Prove it, do not assert it.** Claims about behaviour come with a check that fails when the
   claim stops being true.
