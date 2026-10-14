# SPEC-MiXXynth — v1.0 LOCKED (2026-10-14)

Status: LOCKED for scaffolding. No DSP/UI build until M1 scaffold green.
Separate project from MiXXFX. Location: `c:\Users\fixxe\OneDrive\Documents\MiXXynth\`.
License: **MIT** (maximum freedom: use, modify, disassemble, reassemble, free — attribution only).

## Locked decisions (S1–S10)

- S1: Deep FM programmer first. Beat Dexed/six-sines on depth + usability.
- S2: VST3 + Standalone now, CLAP later. MIDI in v1; MPE/MIDI-out deferred.
- S3: 6-op FM/PM hybrid, 32 DX-style algorithms + free matrix. Clean-room params, no DX7 sysex import v1. Per-op ratio/level/feedback/key-scale, oversample + latency report.
- S4: FM + full VA osc (saw/square/sub/noise) + multimode filter (LP/HP/BP + drive) in v1.
- S5: Drag-drop mod: 4 LFOs + 3 ADSR + MSEG-lite + 4 macros, 32 slots, depth ring, bipolar, right-click clear, smoothed per-block, lock-free.
- S6: Fixed FX tail: Hyper + Chorus + Delay + Reverb + Distortion + EQ + Limiter, per-FX on/off + mix.
- S7: 32 voices, unison to 16, oldest/lowest steal, 2x/4x oversample, 44.1/48/96k. Mono/legato/portamento in build spec.
- S8: Serum-1-style categorized bank. **Randomization CORE**: global smart randomize + per-section locks (FM/VA/Filter/Mod/FX), musical defaults, undo.
- S9: Identity: black metallic + very dark oak + futuristic glow lights. Resizable, min 900x640, default 1440x900. Pages FM/VA-Mod/FX/Presets. Visualizers off audio thread.
- S10: MIT open-source/freeware. Pamplejuce-style CMake 3.25+, Catch2, pluginval, GH Actions Win. Python 3.12 via winget. Telemetry opt-out decision at scaffold.

## Still to confirm at scaffold (yes/no)

1. Scaffold Pamplejuce template files now? (recommended yes)
2. Clone six-sines to `reference/` for study (not linked)? (recommended yes)
3. Install Python 3.12 via winget now? (recommended yes)
4. Set `SUPERPOWERS_DISABLE_TELEMETRY=1`? (recommended yes for privacy)
5. `hippo init` already done isolated — keep as-is? (yes)

## Milestones

- M1: template + CI green + empty synth voice passing pluginval
- M2: FM core (1 algo -> 32 + matrix) + VA + filter + envs, unit-tested
- M3: drag-drop matrix + macros + MSEG-lite
- M4: FX tail + Serum-style bank + randomize + UI polish (impeccable audit)
- M5: beta bank + release VST3 + manual

Bundle ID: `com.mixx.mixxynth`, code `Mxs1`. Formats: VST3 + Standalone (`IS_SYNTH TRUE`, `NEEDS_MIDI_INPUT TRUE`).
