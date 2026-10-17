# FX measurement harness

Status: **in progress**. The analysis core is written and its self-test passes all 21 checks; the
VST3 host works and has measured SerumFX. No MiXXynth FX has been characterised yet, and the
recipes that would drive a plugin's controls do not exist.

## What this is for

The goal is Serum-grade effects in MiXXynth. There are two ways to get there and only one of
them is open to us:

1. **Read someone else's implementation.** Closed for two reasons. Serum is proprietary, and
   MiXXynth's MIT licence is load-bearing - reproducing protected code would end that claim.
   This is settled, not a preference.
2. **Measure behaviour and rebuild it from the literature.** Open, and it is what M5 already
   did: the Juno chorus was built from measured hardware values, and those measurements
   **corrected the plan three times**. That is the precedent this harness generalises.

So the harness is a measurement instrument. It plays known signals through a plugin, reads the
audio back, and reports numbers. It never disassembles, decompiles, or inspects the memory of
anything it measures. That line is what makes measuring a commercial plugin legitimate, and it
is enforced by construction: the harness can only feed audio in and read audio out.

**The rule that follows from it:** nothing goes into MiXXynth's `Source/` because it was seen
in a measured plugin. Measurements tell us what the target behaviour *is*; the implementation
comes from the published literature and from the permissively licensed references in
`TOOLS-RESEARCH.md`. A measurement is a specification, not a source.

## Why a sine sweep and not an impulse or noise

An exponential sine sweep, deconvolved against itself, gives the system's impulse response with
absolute gain preserved. Three reasons it is the right probe here:

- **It separates the linear response from the nonlinearity.** In an exponential sweep the
  harmonics arrive *before* the fundamental's own response, so after deconvolution the linear
  impulse response sits at its own offset and the distortion products land somewhere else
  entirely. A window around the linear response then contains the linear system alone. An
  impulse cannot do this, and neither can a noise burst - both leave the distortion on top of
  what you are trying to measure.
- **It has the energy where the ear is.** Equal energy per octave, so the top two octaves are
  not measured through a 20 dB deficit the way they are with white noise.
- **Absolute gain comes out of it.** The deconvolution is a regularised inverse in the frequency
  domain, not a correlation, so the recovered response is the transfer function itself rather
  than a scaled copy of it. The self-tests pin this down (see below), because "plausible but
  wrong by 6 dB" is the failure mode that would silently poison every later decision.

The deconvolution is `H(f) = Y(f)·conj(X(f)) / (|X(f)|² + ε)`. The regularisation term `ε` is
what keeps it finite outside the sweep's band, at the cost of a biased reading there - so
measurements outside 20 Hz to 20 kHz are not trustworthy and the harness does not report them.

## What it measures

| Measurement | What it tells us | How |
|---|---|---|
| Frequency response | The filter shape, and where the character lives | Sweep deconvolution, magnitude at log-spaced points |
| Impulse response | The same thing in the time domain, plus the tail | The deconvolved buffer itself |
| Latency | The plugin's reported delay, in samples | Index of the impulse response peak |
| Null depth | How much of the dry signal survives | RMS of (wet - dry) against RMS of dry |
| Saturation transfer curve | Where a waveshaper stops being linear, and how hard | Sine at a ladder of levels; level of the **fundamental** that comes back |
| Gain reduction | A dynamics processor's actual gain over time | Wet/dry envelope ratio in dB, windowed |
| Modulation rate and depth | A chorus, flanger or phaser's LFO | Delay tracked by cross-correlation, then read off the track |

Two notes on the less obvious ones.

**The transfer curve measures the fundamental, not the RMS.** RMS includes the harmonics a
waveshaper adds, so a hard clipper and a gentle compressor can present the same RMS curve. The
fundamental separates them, and it is the number that says where the device starts to bend.

**Gain reduction is a wet/dry envelope ratio**, so it reads the processor's real gain including
any make-up. A compressor with 6 dB of make-up gain will not read -6 dB, and that is the honest
answer rather than a bug: the measurement reports what the processor does, not what its knob
says.

**Modulation is measured as a delay trajectory.** Cross-correlating short windows of wet
against dry gives the delay as it moves; the rate and depth are then read off that track rather
than inferred from the sound. This is what would have found the Juno's real numbers - two delay
lines, one LFO, inverted on the right channel, sweeping 1.66 to 5.35 ms - none of which is
visible in a spectrum.

## First measurement: SerumFX 1.3.6.8 — Serum **1**'s FX plugin

**Not to be confused with Serum 2 FX**, which is a different product (see the next section). This
is the standalone FX-only plugin that shipped with Serum 1.

Taken 2026-10-17 against Xfer Records SerumFX 1.3.6.8, VST3, 48 kHz, stereo, in whatever state
it loads in.

| | |
|---|---|
| Channels | 2 in / 2 out |
| Reported latency | 0 samples |
| Parameters | 2397 |
| Null depth at 440 Hz | **-133.92 dB** |

**SerumFX passes audio through untouched until an effect is switched on.** -133.9 dB is below
anything audible, so in its default state it is transparent - which is a useful result in
itself, because it means every later measurement is of an effect rather than of the shell
around it.

The frequency response is flat to within 0.02 dB from 50 Hz to 20 kHz, with -0.97 dB at 20 Hz.
That low-end figure sits at the edge of the sweep and should not be read as a filter.

### The FX section, as the plugin exposes it

Every FX parameter is named `FX <effect> Level`, and every one of them defaults to 0.0:

| Index | Parameter | Default |
|---|---|---|
| 289 | FX Dist Level | 0.0 |
| 290 | FX Flg Level | 0.0 |
| 291 | FX Phaser Level | 0.0 |
| 292 | FX Chorus Level | 0.0 |
| 293 | FX Delay Level | 0.0 |
| 294 | FX Comp Level | 0.0 |
| 295 | FX Reverb Level | 0.0 |
| 296 | FX DimExp Level | 0.0 |
| 297 | FX Filter Level | 0.0 |
| 298 | FX Hyper Level | 0.0 |
| 141-146, 162, 268 | FX Fil Wet / Type / Freq / Reso / Drive / Var / Enable / Pan | the filter |

`FX Fil Enable` defaults to `off`, which agrees with the null test.

### What this says about the next step

**The chorus's own rate and depth are not published as VST3 parameters.** Searching all 2397
names finds exactly one match for "Depth" - parameter 87, `Spin Depth`, which belongs to the
synth's noise section - and nothing that names a chorus rate. The only chorus control the plugin
exposes is the mix level.

So the harness can switch an effect on and measure it as it ships, but it cannot sweep that
effect's controls through the parameter interface. Doing that would mean writing the plugin's
state chunk - its own saved-state format, through `getStateInformation` and `setStateInformation`
- which is still black box, but is a different mechanism from parameters and is not implemented.

That is the boundary, and it is worth knowing before recipes are written: **for SerumFX the
parameter interface buys a measurement of each effect at its factory settings, not a
characterisation across its range.**

## Serum 2 FX — a second class in the same binary

`Serum2.vst3` declares **two plugin classes**, and they are not interchangeable:

| Index | Class | Buses |
|---|---|---|
| 0 | `Serum 2` | instrument, no audio input |
| 1 | `Serum 2 FX` | effect, 2 in / 2 out |

Loading index 0 and measuring it gives silence — which is exactly what it did the first time,
because the harness was taking whichever class the format reported first. **A wrong class looks
identical to a silent plugin**, so the harness now lists classes (`--list`) and requires one to be
named (`--class`), and `--all` refuses to guess.

Measured through the effect class, at 48 kHz stereo:

| | |
|---|---|
| Version | 2.1.2 |
| Parameters | 2623 |
| Reported latency | 0 samples |
| Null depth at 440 Hz | **-300.00 dB** |

So Serum 2 FX is also bit-exact pass-through until something is switched on.

### Its FX section is not reachable through parameters

The FX parameters are three slots of sixteen anonymous knobs:

| Indices | Parameters |
|---|---|
| 471-486 | `FX Main Param 1..16` |
| 487-502 | `FX Bus 1 Param 1..16` |
| 503-518 | `FX Bus 2 Param 1..16` |

**Nothing selects which effect is in a slot, or turns it on.** Searching all 2623 names: `dist`
matches nothing, `Type` matches only `Filter 1/2 Type`, `Slot` and `Mix` match nothing. The
plugin does expose 128 VST3 programs, but every one is named `Prog N` — and loading all 128 in
turn and measuring each shows that **all 128 are empty** (-300 dB, 0 of 128 change the signal).

So the effect type lives in the plugin's **state**, not its parameters.

### How that is handled: the state is opaque

The harness can open the plugin's **own window** (`--configure`), so a preset is loaded by the
plugin's own browser and its own format reader. What comes back is captured through the standard
VST3 state calls (`--save-state` / `--load-state`) and replayed untouched.

That is deliberate: **the preset's file format is never parsed here.** A Serum preset is
`XferJson` magic, a JSON header, then a binary payload — and understanding it would mean reverse
engineering a proprietary format for no benefit, when the plugin will read it itself. The state
blob is opaque to this tool, exactly as it is to a host.

Once a slot has the distortion loaded, its controls map onto `FX Main Param 1..16` — which the
harness *can* drive — so the parameter interface becomes useful again from that point on.

## How correctness is established

The analysis is the instrument, so the instrument is checked before it is pointed at anything.
`--selftest` runs the whole analysis against signals whose answers are known in closed form,
and every check is a PASS/FAIL with a tolerance. The point is specifically to catch a *wrong*
answer rather than a crash:

- **Absolute level.** A known +6.02 dB gain must read 6.02 dB, and a -6.02 dB gain must read
  -6.02 dB. This is the check that catches a deconvolution normalisation error, which would
  otherwise scale every measurement ever taken.
- **A known filter.** A first-order low-pass at 1 kHz must read -3.01 dB at 1 kHz, ~0 dB at
  20 Hz, and -20 dB/decade above it.
- **Two independent methods must agree.** The sweep method is cross-checked against a
  steady-state per-frequency tone measurement. They share no code, so agreement is evidence
  rather than a tautology.
- **Known latency.** A 128-sample delay must put the impulse response peak at 128.
- **Known nulls.** Identical signals must null below -120 dB; a half-amplitude copy must read
  -6.02 dB.
- **A known nonlinearity.** A linear gain must produce a transfer curve of slope 1; a `tanh`
  must be monotonic and must compress at high level.
- **A known gain envelope.** A -6 dB step must read -6 dB.
- **A known modulation.** A synthetic 2.5 Hz, ±1.5 ms delay modulation must read back as
  2.5 Hz and 1.5 ms.

If any of those fail, no measurement the harness produces means anything, so the self-test is
the gate rather than a nicety.

## How to run it

```sh
cmake --build build-vs2026 --config Release --target MiXXynthFxMeasure

# the instrument's own checks - run this first, always
build-vs2026/Release/MiXXynthFxMeasure.exe --selftest

# measure a plugin by absolute path
build-vs2026/Release/MiXXynthFxMeasure.exe --plugin "C:/Program Files/Common Files/VST3/SerumFX.vst3"
```

Targets, in the order they are worth measuring:

1. `SerumFX.vst3` - the reference for what "Serum-grade" means.
2. `Serum2.vst3` - the same effects inside the full synth, which may or may not agree.
3. MiXXynth's own modules - the baseline, and the one case where we may look at the source,
   because it is ours.

## What it cannot do, and will not claim

- **It cannot tell us how an effect is built.** It reports behaviour. Turning behaviour into an
  implementation is a separate, human step that goes through the literature.
- **It cannot measure at the edges of the sweep's band.** The inverse has to be regularised
  where the sweep has no energy, so a frequency where the sweep is more than 60 dB down is
  reported at the floor rather than as a number. In practice that is below 20 Hz and above
  20 kHz.
- **It cannot drive a plugin's controls yet.** Recipes do not exist. SerumFX exposes its FX
  section as mix levels only - see below - so even with recipes, an effect whose own controls
  are not published would only be measurable at its factory settings.
- **It measures effects, not instruments.** MiXXynth is a synth: fed a sweep with no notes it
  produces silence, and the harness correctly reports that as nothing. There is no MIDI input
  yet.

## References

The methods are standard, and the literature is the source of any implementation that follows:

- Farina, *Simultaneous measurement of impulse response and distortion with a swept-sine
  technique* (AES 108, 2000) - the sweep-and-deconvolve method, and the harmonic-separation
  property that motivates it.
- Müller and Massarani, *Transfer-function measurement with sweeps* (JAES 49, 2001) - the
  practical treatment of the inverse filter and its regularisation.
- Zölzer, *DAFX: Digital Audio Effects* - the reference for the effect families this will be
  used to characterise.

