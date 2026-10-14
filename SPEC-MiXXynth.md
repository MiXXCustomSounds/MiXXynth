# SPEC-MiXXynth — Draft v0.1 (for Q&A, no build)

Date: 2026-10-14. Status: DRAFT — no Source/, no CMakeLists, no DSP/UI code until sign-off.
Separate project from MiXXFX. Bundle ID proposal: `com.mixx.mixxynth`, plugin code `Mxs1`.

## 1. Positioning
- Modern FM-forward synth with drag-drop modulation + built-in multi-FX, finished sounds in one plugin.
- References: Vital/Serum (mod UX), Phase Plant (rack), Dexed/six-sines (FM), Surge (FX depth).
- QUESTION S1: primary user — preset tweaker, performing producer, or deep FM programmer? Main rival to beat: Vital / Serum / Phase Plant / Dexed / other?

## 2. Formats & MIDI
- Proposal: VST3 + Standalone on Windows (mirror MiXXFX), JUCE 8, C++20, `IS_SYNTH TRUE`, `NEEDS_MIDI_INPUT TRUE`.
- QUESTIONS S2: add CLAP (via clap-wrapper like six-sines)? MIDI out? MPE support v1 or later?

## 3. FM engine
- Proposal A (default): 6-op FM/PM hybrid a la six-sines (MIT-safe), 32 DX-style algorithms + free-matrix option, per-op ratio/level/feedback/key-scale, anti-aliased sine + oversample option with latency report.
- Proposal B: 4-op lite (simpler UI, faster v1). Proposal C: 8-op + wavetable/warp-FM hybrid (Serum-style, biggest scope).
- QUESTIONS S3: 4 / 6 / 8 ops? Fixed algorithms vs free matrix vs both? DX7 patch import (GPL-risky — avoid or clean-room)? Feedback + key-scaling depth?

## 4. Subtractive layer
- Proposal: per-voice VA osc (saw/square/sub/noise) -> multimode filter (LP/HP/BP + drive) -> amp, routable alongside FM ops.
- QUESTIONS S4: include VA layer in v1 or FM-only v1? Filter topology (ladder/SEM/Sallen-Key)? Mono/poly filter behavior?

## 5. Modulation (drag-and-drop)
- Proposal: 4 LFOs (sync/free, shapes + warp), 3 ADSR + 1 MSEG-lite, 4 macros, velocity/modwheel/aftertouch/MPE sources; drag chip -> knob, depth ring, bipolar, right-click clear; 32-slot matrix, smoothed per-block, lock-free.
- QUESTIONS S5: LFO/env/macro counts? Poly vs mono LFOs? MSEG full editor or shapes only? MIDI-learn + MPE in v1?

## 6. Multi-FX rack
- Proposal: synth-tailored tail — Hyper/Unison, Chorus, Delay (sync/free), Reverb, Distortion, EQ, Compressor/limiter — fixed order, per-FX on/off + mix, global dry/wet. Full reorderable rack (MiXXFX-style UX, fresh code) only if you demand it.
- QUESTIONS S6: which FX in v1 (pick from above)? Fixed tail vs reorderable rack? Per-preset FX lock?

## 7. Voices & performance
- Proposal: 16-voice poly, legato/mono/unison (up to 8, detune/spread), steal oldest/lowest-level, 2x/4x oversample toggle, target <25% CPU @48k on 8-core for typical pad.
- QUESTIONS S7: max voices/unison? Mono/legato/portamento needs? CPU budget + supported sample rates (44.1/48/96)?

## 8. Presets & state
- Proposal: JSON presets + factory bank 64 (bass/keys/pads/leads/plucks/FX), DAW state via APVTS, host automation for all performance params, A/B + init patch.
- QUESTIONS S8: JSON vs XML (MiXXFX-style)? Factory count + categories? A/B, morph, randomize?

## 9. UI/UX (impeccable will refine)
- Proposal: resizable 900x640 min, 1440x900 default; pages: FM/Filter/Mod/FX/Presets; transient values on drag; waveform/filter/LFO visualizers (off audio thread); retro-futurist + walnut continuity with MiXXFX vs new identity (TBD with `/impeccable init`).
- QUESTIONS S9: keep MiXXFX identity or new? Min/default size? Visualizers priority? Keyboard/tooltips/a11y bar?

## 10. Build/test/release
- Proposal: Pamplejuce-style layout (MIT) — CMake 3.25+, Catch2 tests, pluginval, GH Actions Win build, `VST3/` staging like MiXXFX. Install Python 3.12 via winget for skill tests.
- QUESTIONS S10: Pamplejuce template files now? Clone six-sines to `reference/` for study? Python install now? Telemetry opt-out (`SUPERPOWERS_DISABLE_TELEMETRY=1`)? Open-source (MIT, allows GPL FX) vs closed-source (permissive-only FX)?

## 11. Milestones (proposal)
- M1: spec sign-off + template + CI green + empty synth voice passing pluginval
- M2: FM core (1 algo -> N algos) + filter + envelopes, unit-tested
- M3: drag-drop mod matrix + macros
- M4: FX tail + presets + UI polish (impeccable audit)
- M5: beta bank + release VST3 + manual

No code until you answer S1–S10 (at least S2/S3/S10 to unblock scaffolding).
