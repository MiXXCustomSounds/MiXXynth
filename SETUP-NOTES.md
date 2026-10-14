# MiXXynth — Setup Notes (2026-10-14)

Separate project from MiXXFX. Location: `c:\Users\fixxe\OneDrive\Documents\MiXXynth\`.

## Installed

1. **impeccable.style** — `npx -y impeccable install --project -y`
   - Installed to `.claude/skills/impeccable`, `.claude/agents`, hooks in `.claude/settings.local.json`
   - Engine v0.1.14 windows-x64
   - Next in agent chat (not terminal): `/impeccable init` then `/impeccable hooks on`
   - Scope: project-only (isolated from MiXXFX)

2. **obra/superpowers** — shallow clone, copied `skills/` to `.github/skills/superpowers` + `AGENTS.md` to `SUPERPOWERS-AGENTS.md`
   - 15 skills: brainstorming, TDD, systematic-debugging, verification-before-completion, writing-plans, executing-plans, subagent-driven-development, etc.
   - Target: VS Code Copilot Chat / Agent mode (repo skills folder)
   - Telemetry: `SUPERPOWERS_DISABLE_TELEMETRY` not set yet — decide if you want opt-out (see Q below)
   - License: MIT

3. **netresearch/context7-skill** — copied `skills/context7` to `.github/skills/context7`
   - Script: `.github/skills/context7/scripts/context7.sh` (needs bash/curl/jq; on Windows use Git Bash)
   - API key: set in USER env `CONTEXT7_API_KEY` (len 43, prefix ctx7sk-3...). NOT committed. `.env.example` placeholder only.
   - Anonymous fallback OK; key gives higher rate limits.
   - Licenses: code MIT, content CC-BY-SA-4.0

4. **kitfunso/hippo-memory** — `npm install -g hippo-memory` (v2.0.0), `hippo init --no-hooks --no-schedule --no-learn`
   - Store: `.hippo/` (git-ignored, local-only SQLite 782KB)
   - Scope: MiXXynth-only, no global hooks, no daily scheduler, no git-history seeding (per your choice)
   - Prereqs: Node 24.20 OK. Python still missing (needed for some skill tests, not hippo core — hippo has zero runtime deps).
   - `hippo --version` = 2.0.0 verified.

## Toolchain

- Node v24.20.0, npm 11.19.0, CMake 4.4.4, Git 2.55.0 OK
- Python: MISSING — install Python 3.10+ via winget (`winget install Python.Python.3.12`) for skill tests
- npm global prefix `C:\Users\fixxe\AppData\Roaming\npm` was missing — created.

## Research

See `RESEARCH.md`: Pamplejuce (MIT, best starter), six-sines (MIT, top FM ref), DragAndDropModulationSystem (license unknown, study-only), sst-effects (GPL-3.0, study-only).

## Pending decisions (spec Q&A)

- Pamplejuce template vs bare JUCE FetchContent?
- Clone six-sines to `reference/` for study?
- SUPERPOWERS telemetry opt-out?
- Python install now?
- Full SPEC-MiXXynth.md sign-off before any Source/CMake.
