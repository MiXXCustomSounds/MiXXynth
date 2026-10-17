# MiXXynth

MiXXynth is an **entirely separate project from MiXXFX**. No shared source, no shared CMake, no shared git history.

- Type: modern VST synth (VST3 + Standalone planned, JUCE-based)
- Focus: modern framework, drag-and-drop LFOs / modulation, complex built-in multi-FX rack, complex FM synthesis
- Workspace: `MiXXynth.code-workspace`
- Status: **a working synth.** Six-operator FM with a free routing matrix, a VA layer with its
  own filter, per-operator filters, 32 modulation slots over editable shapes, twelve FX modules,
  an output safety limiter, a categorized preset bank with a smart randomizer, and an editor of
  three tabs. The visual pass has not happened yet.

## Gates

Every claim above is checked by one of these, and CI runs all of them:

| Gate | What it covers |
|---|---|
| `MiXXynthSmoke` | DSP, offline: audible output, stability, every module's own behaviour |
| `MiXXynthEditorAudit` | Every control wired to the parameter and modulation destination it should be |
| `MiXXynthPatchAudit` | The patch: the preset bank, partial-over-init loads, the host programs, the roll |
| `pluginval --strictness-level 5` | The plugin in a host |

```sh
cmake -S . -B build-vs2026 -G "Visual Studio 18 2026" -A x64
cmake --build build-vs2026 --config Release --target MiXXynth_VST3 MiXXynth_Standalone
cmake --build build-vs2026 --config Release --target MiXXynthSmoke MiXXynthEditorAudit MiXXynthPatchAudit
```

## Requirements

- Windows 10/11
- Visual Studio 2026 with Desktop development with C++ workload
- CMake 3.25+
- Git (JUCE and sst-jucegui are fetched on first configure, isolated to this repo's build dir)
- Node 18+ (only to regenerate the factory bank: `node tools/make-factory-presets.mjs`)

## Layout

- `MiXXynth.code-workspace` — VS Code workspace
- `Source/` — the plugin. `Fx/` holds the rack's modules; `PresetStore`, `PresetManager` and
  `Randomizer` are the patch; `PatchBar` is the browser and the roll
- `presets/` — the factory bank, as text. Embedded in the binary at build time
- `tests/` — the three gates
- `tools/` — pluginval, and the factory bank's generator
- `docs/` — the design record: `PLAN-M4B`, `PLAN-M5`, `PLAN-M6`, `PLAN-M7`, `PRESETS`,
  `ALGORITHMS`, `MOD-SHAPES`, `MATRIX-AND-VA`, `PERFORMANCE-AND-MPE`, `EDITOR-AUDIT`
- `VST3/` — the staged installable bundle, written by the build

## Separation from MiXXFX

- MiXXFX lives at `c:\Users\fixxe\OneDrive\Documents\New folder` (JUCE FX rack)
- MiXXynth lives at `c:\Users\fixxe\OneDrive\Documents\MiXXynth\` — fresh `git init`, no remote, no subtree, no copy of `Source/`
- Ideas may be referenced in docs; code reuse (if any) will be deliberate manual port with license check.
