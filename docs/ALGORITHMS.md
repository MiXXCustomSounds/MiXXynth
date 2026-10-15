# MiXXynth — FM Algorithms and Routing (M2a)

## Two routing modes

1. **Algorithm mode** — `algorithm` parameter selects one of the 32 DX7 algorithms (values 1-32). The voice's routing matrix and carrier flags are loaded from the built-in table. The 30 matrix params are ignored in this mode.
2. **Free mode** — `algorithm` = "Free (matrix)". Routing comes from the parameters:
   - `mtx{src}{dst}` — 30 off-diagonal depths (0..1), "op src modulates op dst"
   - `carrier{1..6}` — which operators reach the output

Per-op `op{n}feedback` always applies (0..0.95), in both modes. DX7 hardware allows only one feedback operator per algorithm; MiXXynth deliberately allows all six, so an algorithm plus extra feedback is a superset of DX7 behaviour.

## Operator numbering

OP1..OP6 as in DX7. `mtx12` means OP1 modulates OP2. `carrier1` is OP1.

## Evaluation order

Operators are evaluated from OP6 down to OP1. Every DX7 edge runs from a higher
operator number to a lower one (6>5, 5>4, ... 2>1), so those edges resolve within
the same sample — no delay. Any other edge (e.g. OP1 > OP6 in free mode) uses the
previous sample, which keeps arbitrary matrices stable and cycle-free at runtime.

## The 32 algorithms

Topology listing, `>` meaning "modulates". Carriers are the operators sent to the
output. "fb" is the feedback operator designated by the DX7 chart (informational:
MiXXynth lets you set feedback per operator instead).

| # | Topology | Carriers | fb |
|---|---|---|---|
| 1 | 6>5>4>3, 2>1 | 3,1 | 6 |
| 2 | 6>5>4>3, 2>1 | 3,1 | 2 |
| 3 | 6>5>4, 3>2>1 | 4,1 | 6 |
| 4 | 6>5>4, 3>2>1 | 4,1 | 4 |
| 5 | 6>5, 4>3, 2>1 | 5,3,1 | 6 |
| 6 | 6>5, 4>3, 2>1 | 5,3,1 | 5 |
| 7 | 6>5>3, 4>3, 2>1 | 3,1 | 6 |
| 8 | 6>5>3, 4>3, 2>1 | 3,1 | 4 |
| 9 | 6>5>3, 4>3, 2>1 | 3,1 | 2 |
| 10 | 6>4, 5>4, 3>2>1 | 4,1 | 3 |
| 11 | 6>4, 5>4, 3>2>1 | 4,1 | 6 |
| 12 | 6>3, 5>3, 4>3, 2>1 | 3,1 | 2 |
| 13 | 6>3, 5>3, 4>3, 2>1 | 3,1 | 6 |
| 14 | 6>4, 5>4, 4>3, 2>1 | 3,1 | 6 |
| 15 | 6>4, 5>4, 4>3, 2>1 | 3,1 | 2 |
| 16 | 6>5>1, 4>3>1, 2>1 | 1 | 6 |
| 17 | 6>5>1, 4>3>1, 2>1 | 1 | 2 |
| 18 | 6>5>4>1, 3>1, 2>1 | 1 | 3 |
| 19 | 6>5, 6>4, 3>2>1 | 5,4,1 | 6 |
| 20 | 3>2, 3>1, 6>4, 5>4 | 4,2,1 | 3 |
| 21 | 6>5, 6>4, 3>2, 3>1 | 5,4,2,1 | 3 |
| 22 | 6>5, 6>4, 6>3, 2>1 | 5,4,3,1 | 6 |
| 23 | 6>5, 6>4, 3>2 | 5,4,2,1 | 6 |
| 24 | 6>5, 6>4, 6>3 | 5,4,3,2,1 | 6 |
| 25 | 6>5, 6>4 | 5,4,3,2,1 | 6 |
| 26 | 6>4, 5>4, 3>2 | 4,2,1 | 6 |
| 27 | 6>4, 5>4, 3>2 | 4,2,1 | 3 |
| 28 | 5>4>3, 2>1, 6 | 6,3,1 | 5 |
| 29 | 6>5, 4>3 | 5,3,2,1 | 6 |
| 30 | 5>4>3, 6 | 6,3,2,1 | 5 |
| 31 | 6>5 | 5,4,3,2,1 | 6 |
| 32 | (none, all additive) | 6,5,4,3,2,1 | 6 |

## Provenance and verification

The 32 topologies were encoded from the explicit textual listing of the DX7
algorithms published by Sean Bolton (author of hexter) on the linux-audio-user
mailing list, and cross-checked against the DX7 algorithm chart. Algorithm
topologies are factual descriptions of the DX7 hardware; this encoding is
MiXXynth's own (MIT).

Because the operator numbering in third-party sources varies, the table is worth
an A/B check against a reference DX7 implementation (Dexed, six-sines) for any
algorithm you rely on. Corrections are one-line edits in `Source/Algorithms.h`
(`makeAlgo` edges/carriers/feedback), and `tests/Smoke.cpp` re-verifies all 32
render sanely after any change. In Free mode the matrix is authoritative, so any
disagreement can be reproduced and fixed visually.
