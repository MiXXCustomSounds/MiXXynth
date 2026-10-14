# MiXXynth

MiXXynth is an **entirely separate project from MiXXFX**. No shared source, no shared CMake, no shared git history.

- Type: modern VST synth (VST3 + Standalone planned, JUCE-based)
- Focus: modern framework, drag-and-drop LFOs / modulation, complex built-in multi-FX rack, complex FM synthesis
- Workspace: `MiXXynth.code-workspace`
- Status: scaffolding + research + tooling setup + spec (no DSP/UI build yet)

## Requirements (planned)

- Windows 10/11
- Visual Studio 2026 with Desktop development with C++ workload
- CMake 3.22+
- Git (JUCE fetched on first configure, isolated to this repo's build dir)
- Node 18+ / npm (for impeccable + skills)
- Python 3.10+ (for skill tests + hippo-memory) — **currently missing, to install**

## Layout (planned)

- `MiXXynth.code-workspace` — VS Code workspace
- `RESEARCH.md` — modern VST synth design research + helpful repos table
- `SPEC-MiXXynth.md` — spec sheet (draft then Q&A before build)
- `Source/` — created only after spec sign-off
- `CMakeLists.txt` — created only after spec sign-off (own JUCE FetchContent, IS_SYNTH TRUE)

## Separation from MiXXFX

- MiXXFX lives at `c:\Users\fixxe\OneDrive\Documents\New folder` (JUCE FX rack)
- MiXXynth lives at `c:\Users\fixxe\OneDrive\Documents\MiXXynth\` — fresh `git init`, no remote, no subtree, no copy of `Source/`
- Ideas may be referenced in docs; code reuse (if any) will be deliberate manual port with license check.
