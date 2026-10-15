# MiXXynth — Performance Bar and MPE (Tasks 2 & 3)

## Performance bar

A bar at the bottom of the editor holding an on-screen keyboard and two wheels.

### Keyboard
`juce::MidiKeyboardComponent` + `juce::MidiKeyboardState` — both first-party JUCE, so
no dependency was needed. The processor owns the `MidiKeyboardState` and calls
`processNextMidiBuffer (midi, 0, numSamples, true)` at the top of `processBlock`,
which does two things at once:

- updates the displayed key state from incoming MIDI, and
- injects the on-screen keyboard's own note-on/off events into the stream, so the
  keyboard is playable with the mouse in both the standalone and a host.

Range 21–108 with scroll buttons, 20 px keys, middle C at C4.

### Wheels
Neither JUCE nor sst-jucegui ships a pitch/mod wheel (sst only has mouse-*wheel*
calibration helpers), so `WheelComponent` is hand-rolled (~130 lines, no dependency):
a vertical wheel with an orange grip line.

- **Pitch** — bipolar, centred, spring-loaded: releasing the mouse animates it back to
  centre at 60 Hz.
- **Mod** — unipolar, bottom-anchored, stays where it is left.

Both **mirror incoming MIDI** (polled at 30 Hz by the editor timer, since the
processor stores the latest pitch bend and CC1) and **drive it** when dragged: a drag
writes to the processor's internal pitch-bend / mod-wheel state, so no MIDI is sent
out and there is no conflict with a host.

Wheels sit to the **left** of the keys, as on a real synth. They were originally on the
right, which pushed them off-screen in narrower host windows.

### Pitch bend was previously unimplemented
Grepping `Source/` for `pitchBend` returned nothing before this work: a physical pitch
wheel did nothing at all. Now:

- `isPitchWheel()` is captured into a −1..+1 value;
- a new **Bend Range** parameter (0–24 semitones, default 2) scales it;
- `MiXXVoiceManager::setPitchBendSemitones` re-derives every sounding voice's detune
  from its unison position, so bends do not accumulate and unison spread is preserved.

Verified by test: a +12 semitone bend gives a zero-crossing ratio of **2.000958**
(i.e. an exact octave).

## MPE

### Host declaration
`AudioProcessor::supportsMPE()` is overridden to return `true`. JUCE requires this to be
a constant (it can be called on the audio thread), so it always reports `true`; the
**MPE Enabled** parameter decides how incoming MIDI is interpreted.

JUCE does not implement VST3 note-expression, so MPE travels as ordinary per-channel
MIDI — no extra plugin-format flags are needed beyond `supportsMPE()`.

### Signal path
When **MPE Enabled** is on, the block is processed by a `juce::MPEInstrument` with a
lower-zone layout of *N* member channels (**MPE Member Channels**, 1–15, default 15).
An `MPEInstrument::Listener` routes each per-note event:

| MPE callback | Routed to |
|---|---|
| `noteAdded` | `startNoteOnChannel (channel, note, velocity, …)` |
| `noteReleased` | `releaseNoteOnChannel (channel, note, …)` |
| `notePitchbendChanged` | `setNotePitchBend` → per-voice detune |
| `notePressureChanged` | `setNotePressure` → per-voice `Aftertouch` mod source |
| `noteTimbreChanged` | `setNoteTimbre` → new `Timbre` mod source |

Voices are keyed by **(channel, note)**, which is exactly MPE's note identity. With MPE
off, the plain path is used: notes keyed by channel + note as normal, channel pitch bend
applied globally, channel pressure feeding `Aftertouch`.

`enableLegacyMode()` is enabled at construction so the instrument is sane before a zone
layout is applied.

### Modulation
`Timbre` is a new (18th) modulation source, so MPE slide can be patched to anything —
FM index, operator ratios/levels, filter cutoff, matrix depths. `Aftertouch` now reads
the **per-note** pressure when MPE is active and falls back to the global channel
pressure otherwise, so both modes work.

### Known limitation
MPE events are applied at **block start** rather than sample-accurately. This is fine for
expression (continuous), but a fast note-on burst in a single large block would be
quantised. Sample-accurate MPE would need the voice manager to expose sub-block
rendering.

## Tests

| Test | Result |
|---|---|
| Pitch bend +12 st doubles pitch | 2.000958 |
| Bend keeps unison voices alive | 4 voices |
| MPE bend affects only its own channel | 1.999045 |
| MPE pressure/timbre are safe | 1 voice, finite |
| Editor audit: source list matches enum | 18 vs 18 |
| Editor audit: all parameters reachable | 248 params, 142 knobs |
