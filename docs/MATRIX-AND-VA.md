# MiXXynth — Matrix and VA/Filter fixes

Two tabs that looked broken. One was genuinely inert; the other was working but
silent by default and never said so.

## 1. Matrix: the routing knobs did nothing (real bug)

### What was wrong

`processBlock` chose between two mutually exclusive sources for the routing:

```cpp
const int algoIndex = static_cast<int> (getParam (apvts, "algorithm"));

if (algoIndex >= 0 && algoIndex < mixxynth::kNumAlgorithms)
{
    // DX7 algorithm mode: routing comes from the table.
    vp.mod[src][dst] = algo.mod[src][dst];   // <- overwrites the matrix
    vp.carrier[i]    = algo.carrier[i];      // <- overwrites the switches
}
else
{
    // Free mode: routing comes from the 30 matrix depths + 6 carriers.
}
```

The `algorithm` parameter defaults to index 0 (DX7 algorithm "1"), which is
`< kNumAlgorithms`, so the plugin was **always** in algorithm mode on startup and
the 30 routing knobs plus the 6 carrier switches were **never read**. You could
drag them, watch them move, and hear nothing change. The old hint line mentioned
it in passing; the UI gave no other signal.

### The fix

- A **banner** at the top of the Matrix tab now states which mode is in charge:
  - amber: *"Algorithm N defines the routing. Move a knob or switch to copy it
    here and go Free."*
  - green: *"Free (matrix) routing: these depths and switches define the patch."*
- Two buttons: **Copy algorithm to matrix** and **Free mode**.
- **Editing any routing knob or carrier switch while an algorithm is in charge now
  works.** The page seeds the matrix from the algorithm, re-applies your edit on
  top, and switches the algorithm to Free. So the knob you just moved actually
  does something, and the patch does not jump to silence.
- Selecting "Free (matrix)" from the FM tab's algorithm combo with an empty matrix
  also seeds it, for the same reason.

The order matters and is deliberate: **copy, then re-apply, then go Free** — the
other order would wipe the edit that triggered the switch.

### State loads are not user edits

The page watches the routing parameters to detect edits. A preset or project load
replaces every parameter at once, which looked identical to "the user moved 30
knobs". That would have switched the patch into Free mode behind your back on every
load.

`MiXXynthProcessor::setStateInformation` now bumps an atomic `stateLoadCount`, and
the page resyncs its snapshot when it sees the counter move instead of reacting to
it. The audit regression-tests this ("loading a state does not force Free mode").

> One observed oddity, stated plainly: with a state file written by a build from
> before this guard, the banner read "Algorithm 30" instead of "Algorithm 1". I
> could not reproduce it from a clean state, the only code path that writes
> `algorithm` writes `kNumAlgorithms` (Free), and the audit asserts that path lands
> on Free. The state-load guard removes the whole class of problem, but I have not
> proven what produced that value.

### Also

Routing knob labels read `Op1 > Op2` instead of `Op1>2`, matching the modulation
destination names exactly.

## 2. VA/Filter: working, but silent and unexplained

The DSP was fine — the smoke tests already proved the VA oscillator, the filter and
the modulation of all of them. The page was unusable because of defaults and
signage:

| Problem | Fix |
|---|---|
| `filterOn` defaulted to **false**, so Cutoff/Reso/Drive did nothing until you found the toggle | Defaults to **true** with the cutoff wide open (20 kHz), so the filter is transparent until you move it and audible the moment you do |
| `filterCutoff` defaulted to 12 kHz, which audibly dulls a bright patch even when "open" | Now **20 kHz** |
| `vaLevel` defaults to 0 = FM only, so the VA oscillator contributes nothing | Kept (FM-first is the right default patch) but the page now **says so** |
| Nothing on the page reported what was happening | A live readout under the controls: *"FM only (VA Level is 0) | Filter: Low Pass 20000 Hz, Q 0.20"* |
| `vaPulseWidth` was wired to `dstOff` while the page's own text claimed "all of these are modulation destinations" | Pulse Width is now a real destination (`dstVaPulseWidth`), applied in `applyModulation` |
| `std::tan()` recomputed per sample per voice | Coefficients cached and recomputed only when cutoff or resonance move — matters now that the filter is on by default |

`kNumModDests` went 50 → 51 and the modulatable knob count 49 → 50; the audit's
per-parameter destination expectations were updated to match, which is exactly how
that check is supposed to catch a change like this.

## Verification

- `MiXXynthSmoke`: unchanged suite still passes (the VA/filter DSP tests were
  already covering the audio path).
- `MiXXynthEditorAudit`, new checks:
  - `setModDepthFor` creates the pair, sets the amount, goes negative, and does not
    duplicate an existing slot
  - `removeModSlot` clears only its own pair
  - `VA Pulse Width` is a modulation destination
  - a routing edit switches the algorithm to Free mode, the triggering edit
    survives, and the algorithm was copied into the matrix
  - loading a state does **not** force Free mode
- `pluginval --strictness-level 5`: SUCCESS.
- Visually confirmed in the standalone on a clean state: the Matrix banner reads
  "Algorithm 1" with the knobs and switches live, and the VA page reports
  "FM only (VA Level is 0) | Filter: Low Pass 20000 Hz, Q 0.20".
