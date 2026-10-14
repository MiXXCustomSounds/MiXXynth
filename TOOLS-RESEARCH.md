# MiXXynth — Tooling Research & Handoff Plan (WIP)

Date: 2026-10-14. Purpose: research + install every useful tool BEFORE a different agent programs. No `Source/` or DSP/UI code in this phase.
Spec: MIT, 6-op FM (32 algo + matrix) + VA + filter, drag-drop 4LFO/3ADSR/MSEG/4macro 32-slot, fixed FX tail, 32-voice/u16, Serum bank + CORE randomize, black-metal + dark-oak + glow, VST3+Standalone/CLAP-later.

## How to read this file
- S1 = already installed. S2 = candidates. S3 = install log. S4 = handoff checklist.
- Reference clones -> `reference/` (study-only, never linked yet). Skills -> `.github/skills/` or `.claude/skills/`.

## 1. Already installed (verified 2026-10-14)
- impeccable.style project scope, engine v0.1.14 — UI design skill. Use: `/impeccable init` + audit before UI polish.
- obra/superpowers, 15 skills in `.github/skills/superpowers` — TDD, plans, debugging for programming agent.
- netresearch/context7-skill in `.github/skills/context7`, key in user env — live JUCE/CMake docs, no MCP overhead.
- kitfunso/hippo-memory v2.0.0, isolated `.hippo/` — local memory + handoff.
- Toolchain: Node 24.20, npm 11.19, CMake 4.4.4, Git 2.55. Python 3.12.10 installed via winget 2026-10-14.

## 2. Candidates (researched 2026-10-14)
### A. Build / framework
- Pamplejuce (MIT): JUCE 8/9 + C++23 + CMake 3.25 + Catch2 + Benchmarks + Melatonin Inspector + pluginval + GH Actions + signing. M1 scaffold choice. NOT scaffolded yet per scope (references + Python + validation only).
- JUCE 8.0.6 (GPLv3-or-commercial): core framework. MIT open-source project can use JUCE under GPLv3. No install yet (build agent scaffolds).
- Melatonin Inspector (MIT via Pamplejuce): runtime JUCE UI inspector for black-metal layout debug.
### B. FM / DSP core (MIT-safe first)
- six-sines (MIT): 6-sine FM/PM/AM, sst-libs, CLAP wrapper, factory patches + manual. TOP reference — clone to reference/.
- Signalsmith DSP (MIT, header-only, C++11): delay lines, Lagrange/polyphase/Kaiser-sinc interpolators, envelope tools, FFT/STFT with measured quality tests. Primitives for chorus/delay/reverb/mod. Clone to reference/.
- chowdsp_utils (per-module BSD/GPL): BSD modules safe (parameters, presets, DSP utils, CLAP ext, rhythm, version); GPLv3 modules (plugin_base, plugin_utils, GUI, visualizers) STUDY ONLY. Usefulness: oversampling, params, preset system, visualizers pattern.
- sst-* (GPL-3.0): Surge engine benchmark. STUDY ONLY for MIT project, do not vendor.
- dexed (GPL): DX7 32-algo maps. STUDY ONLY. Algo diagrams + param ranges for clean-room re-implementation.
- hsetlik DragAndDrop + HexFm (license TBD): STUDY ONLY until license confirmed. Mod UX pattern.
### C. Validation / QA
- pluginval (GPL-3.0, Tracktion): cross-platform VST3/AU validator, GUI + headless `--strictness-level 5` for CI, separate-process testing. Install Windows binary to `tools/pluginval/` (tool, not vendored source).
- Catch2 (BSL-1.0): unit tests via Pamplejuce/CPM at scaffold time. Not installed standalone now.
- CPM.cmake (MIT): dep manager used by Pamplejuce/sst. Scaffold time.
### D. Presets + randomize (S8 CORE)
- Serum-1 bank structure (categories, macros) adapted to FM. six-sines patches + manual as patch language. Randomize: per-section locks + musical ratios + undo.
### E. Assets (S9 black metal + dark oak + glow)
- Procedural metal/glow in JUCE (gradients, shadows) preferred over raster. Dark oak texture only if CC0/OFL with attribution file.

## 3. Install log (2026-10-14)
- Python 3.12.10 via `winget install Python.Python.3.12`. Verified: `python --version` = 3.12.10, pip 25.0.1. Path: `%LOCALAPPDATA%\Programs\Python\Python312\`. NOTE: fresh shells need PATH refresh or absolute path.
- reference/six-sines (MIT, shallow): `git clone --depth 1 https://github.com/baconpaul/six-sines.git`. Verified: src/{clap,dsp,infra,presets,synth,ui}, doc/manual.md + screenshots, LICENSE.md = MIT. HOW USEFUL: FM/PM voice topology, mod approach, preset/patch language, CLAP-later path, factory bank design for S8.
- reference/signalsmith-dsp (MIT, header-only C++11): `git clone https://signalsmith-audio.co.uk/code/dsp.git`. Verified: delay.h, envelopes.h, filters.h, fft.h, spectral.h, mix.h, curves.h, rates.h, LICENSE.txt = MIT. HOW USEFUL: delay-line + interpolation (chorus/delay), envelope followers, filter helpers — permissive primitives for S5/S6 without GPL risk.
- reference/chowdsp_utils (per-module BSD/GPL, shallow): `git clone --depth 1 https://github.com/Chowdhury-DSP/chowdsp_utils.git`. Verified: modules/{common,dsp,gui,music,plugin}. HOW USEFUL: BSD parts (parameters, presets v1/v2, DSP utils, CLAP ext, version) are vendoring-safe; GPLv3 parts (plugin_base/utils, gui, visualizers, foleys) STUDY ONLY. Preset system directly relevant to Serum-style bank + randomize undo.
- tools/pluginval (GPL-3.0 tool, binary only): downloaded `pluginval_Windows.zip` from Tracktion releases, extracted to `tools/pluginval/pluginval.exe`. Verified: `--version` = 1.0.4. HOW USEFUL: `pluginval --strictness-level 5 <VST3>` headless in CI + GUI for debugging; catches host-compat + state/automation bugs before beta. NOTE: had landed in `New folder/tools` due to shell drift — moved to MiXXynth/tools.
- Skills re-verified: superpowers (15 skills), context7 (REST, key in user env), impeccable engine, hippo 2.0.0. No JUCE-specific third-party skill found worth vendoring (awesome-claude-skills is generic index only).
- NOT installed per scope: Pamplejuce scaffold, JUCE, Catch2/CPM (scaffold time), sst-*/dexed (GPL study-only, no clone), hsetlik (license TBD).
- Telemetry: `SUPERPOWERS_DISABLE_TELEMETRY=1` set in user env 2026-10-14 (privacy opt-out).
- Repo leanness: `reference/` + `tools/pluginval*` git-ignored (re-cloneable). Re-create via:
  `git clone --depth 1 https://github.com/baconpaul/six-sines.git reference/six-sines`
  `git clone https://signalsmith-audio.co.uk/code/dsp.git reference/signalsmith-dsp`
  `git clone --depth 1 https://github.com/Chowdhury-DSP/chowdsp_utils.git reference/chowdsp_utils`
  pluginval: download `pluginval_Windows.zip` from Tracktion releases to `tools/pluginval/`.

## 4. Handoff checklist (for programming agent)
- [ ] SPEC-MiXXynth.md v1.0 + SPEC-ANSWERS.md read (MIT, 6-op+VA, drag-drop 32-slot, FX tail, 32v/u16, Serum bank + randomize CORE, black-metal/dark-oak/glow, VST3+Standalone/CLAP-later)
- [ ] This file + SETUP-NOTES.md + RESEARCH.md read
- [ ] `hippo handoff latest` read (created 2026-10-14, see below)
- [ ] Toolchain: VS2026 C++ + CMake 4.4.4 + Python 3.12.10 + Node 24.20 verified
- [ ] JUCE license path: MIT open-source => JUCE GPLv3 route open, no commercial license needed
- [ ] reference/ re-cloned if missing (commands above); GPL = study-only, MIT/BSD = vendoring-safe
- [ ] Scaffold: Pamplejuce-style CMake 3.25+ + Catch2 + pluginval (`tools/pluginval/pluginval.exe --strictness-level 5`) + GH Actions Win, CI green before DSP
- [ ] Tests first (superpowers TDD): FM voice, VA/filter, mod matrix smoothing, FX tail, randomize determinism (seeded)
- [ ] `/impeccable init` + hooks before UI work (black-metal + dark-oak + glow)

