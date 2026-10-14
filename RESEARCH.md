# MiXXynth — Research: Modern VST Synth Design

Research date: 2026-10-14. Separate project from MiXXFX. No code vendored yet — evaluation only.

## 1. Modern frameworks (verified 2026-10-14)

- `sudara/pamplejuce` — MIT. JUCE 8 (tracking 9.x develop as submodule), C++23, CMake 3.25+, Catch2 v3.8.1 Tests + Benchmarks, Melatonin Inspector, GH Actions (build/test/pluginval 1.x), macOS notarization + Azure Trusted Signing stubs. Use via "Use this template" (not fork); CLAUDE.md/AGENTS.md wizard for naming/CI/signing. **Best starter for MiXXynth** vs bare `juce_add_plugin` (MiXXFX pattern).

| Candidate | What | License | Verdict |
|---|---|---|---|
| `juce-framework/JUCE` 8.0.6 via FetchContent | Full plugin framework, VST3+Standalone, APVTS automation, resizable editor | JUCE 7+ commercial-or-GPLv3 (note for closed VST3) | **Default pick** — matches your MiXXFX toolchain (VS2026, CMake 3.22+, CI pattern) |
| `sudara/pamplejuce` | Modern JUCE CMake template, tests, CI, versioning | MIT/ISC mix | **Starter template candidate** — best practice for new synth repo |
| `Chowdhury-DSP/JUCEPluginTemplate`, `KristofferKarlAxelEkstrand/juce-project-template`, `anthonyalfimov/JUCE-CMake-Plugin-Template` | Minimal JUCE+CMake starters | MIT/BSD | Alternatives if pamplejuce feels heavy |
| `iPlug2/iPlug2` | Alternative framework, web-canvas UI, CLAP/VST3 | MIT-ish (WDL) | Only if we want NanoVG/web UI over JUCE editor |
| `hollance/synth-plugin-book` | JUCE synth tutorial book code | MIT | Learning reference, not a dependency |

Decision needed in spec: JUCE 8 solo vs JUCE + pamplejuce template. No code installed yet.

## 2. Drag-and-drop LFOs / modulation

Pattern (Vital/Serum/Phase Plant): LFO/Env/Macro source chips -> drag onto any knob -> depth ring + bipolar depth -> right-click clear. Lock-free mod matrix, smoothed per-block, off audio-thread UI.

| Candidate | What | Notes |
|---|---|---|
| `hsetlik/DragAndDropModulationSystem` | Direct JUCE drag-drop mod example | Primary study target |
| `hsetlik/Octane`, `hsetlik/Hex`, `hsetlik/HexFm`, `hsetlik/FmSynthesisFramework` | Same author's FM + synth work | Architecture reference for FM + mod combined |
| `juce::DragAndDropContainer` docs | JUCE built-in drag-drop | Mechanism we will build on |
| JUCE forum threads 24350 / 36660 / 16793, `cheapsynth01/ModulationComponent.cpp` | Mod-matrix UI patterns | UX fallback if full drag-drop deferred to v2 |

Spec questions: mono vs poly mod, max slots (e.g. 32?), macros count, MIDI-learn overlap, MPE?

## 3. Complex built-in multi-FX racks

License-first: MIT/BSD/Apache OK for closed-source VST3. GPLv3 (Surge, Dexed, ChowMatrix) = study-only unless MiXXynth goes open-source.

| Candidate | What | License (verify before vendoring) | Verdict |
|---|---|---|---|
| `surge-synthesizer/sst-effects` | Surge XT FX (reverb, delay, chorus, distortion, etc.) | GPLv3 | Benchmark only unless open-source |
| `surge-synthesizer/conduit` + `sst-filters` | Filters + shared DSP | GPLv3 | Benchmark only |
| `Chowdhury-DSP/ChowMatrix`, `chowdsp_utils` | Modular delay + DSP utils | Check per-module (mostly MIT/BSD, some GPL) | Selective MIT parts possible |
| `signaldustry?/Signalsmith DSP` (signalsmith-audio) | Header-only primitives | MIT | **Install candidate** — safe |
| `juandagilc/Audio-Effects` | Classic FX collection | MIT (verify) | Study / port patterns |

Plan: synth-tailored tail (chorus/delay/reverb/distortion/hyper-unison) first; full reorderable rack only if spec demands (reuse of MiXXFX UX ideas, not code).

## 4. Complex FM synthesis

| Candidate | What | License | Verdict |
|---|---|---|---|
| `asb2m10/dexed` | 6-op DX7-accurate FM, 32 algorithms | GPLv3 | Algorithm maps + param model study only |
| `baconpaul/six-sines` | Modern 6-op (dual 3-op), open modulation | MIT | **Top install candidate** — permissive, modern JUCE/CLAP |
| `hsetlik/HexFm`, `hsetlik/FmSynthesisFramework` | JUCE FM synth + framework | Check repo (likely MIT/GPL mix) | Architecture reference |
| DecentSampler `fm6op` docs, Cherry `F_DEXZ`, `Fire 6-op` | Algorithm/feedback/key-scale conventions | Docs | UX spec input |

Topology decision for spec: pure 6-op DX-style vs hybrid (FM + subtractive + wavetable/sub like Serum/Vital). Big DSP+UI scope split.

## 5. Next: installs (pending your per-tool answers)

- `https://impeccable.style/` — `npx impeccable install`, scope: MiXXynth-only vs global? Tool: VS Code Copilot / Claude / Cursor / Codex? Hooks on?
- `https://github.com/obra/superpowers` — which agent plugin(s)? Telemetry opt-out? TDD+brainstorm default?
- `https://github.com/netresearch/context7-skill` — API key now or anonymous? Repo-local vs global?
- `https://github.com/kitfunso/hippo-memory` — needs Python 3.10+ (currently missing). Targets to hook? MiXXynth-only memory? Allow local server :6789 + dashboard :3333?
- Helpful DSP repos above: **none installed** until licenses approved in spec.

## 6. Sources

- JUCE starters: pamplejuce, JUCE-Plugin-Starter, juce-project-template, audio-synth-example, jucestarter, JUCE-CMake-Plugin-Template
- Mod: hsetlik/DragAndDropModulationSystem, Octane, HexFm, FmSynthesisFramework, JUCE DragAndDropContainer docs
- FX: sst-effects, conduit, sst-filters, surge, ChowMatrix lineage
- FM: six-sines (+manual), dexed, dexex, linuxmusicians six-sines thread
