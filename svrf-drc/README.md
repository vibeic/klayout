# svrf-drc — SVRF-native DRC, built into KLayout (vibeic fork)

KLayout reads a **Calibre/SVRF-format DRC deck** (`.rule`) and executes every rule
**directly on its own DRC engine** — `pya.Region.*_check` for geometry and
`pya.LayoutToNetlist` for connectivity. There is **no transcoder / compiler and no
intermediate `.drc` file**: KLayout ingests the deck format and solves it natively.

## Why this is not a transcoder

Every SVRF measurement modifier maps **1:1 onto a native KLayout check parameter**
(the heavy geometry is KLayout's own C++ engine):

| SVRF / Calibre | KLayout native parameter |
|---|---|
| `EXTERNAL a b < d` | `a.separation_check(b, d)` |
| `EXTERNAL a < d` | `a.space_check(d)` |
| `INTERNAL / WIDTH a < d` | `a.width_check(d)` |
| `NOTCH a < d` | `a.notch_check(d)` |
| `ENCLOSURE a b < d` | `b.enclosing_check(a, d)` |
| `AREA a < v` | `a.with_area(None, v, false)` |
| `ABUT < angle` | `ignore_angle=angle` |
| `PROJECTING / PARALLEL` | `metrics=Projection` (+ `min/max_projection`) |
| `SQUARE` | `metrics=Square` |
| `OPPOSITE` | `opposite_filter=OnlyOpposite` |
| `WHOLE` | `whole_edges=True` |
| `SHIELDED / TRANSPARENT` | `shielded=True/False` |
| `AND / OR / NOT / XOR` | `&` `\|` `-` `^` |
| `SIZE / GROW / SHRINK` | `sized(±d)` |
| `INTERACT / INSIDE / OUTSIDE` | `interacting / inside / outside` |
| `CONNECT a b BY via` + `CONNECTED` / `NOT CONNECTED` | `LayoutToNetlist` + `property_constraint=Same/DifferentPropertiesConstraint` |

Operators with no native `pya.Region` equivalent (windowed `DENSITY`, `ANTENNA`,
`ANGLE`) are **honestly reported as SKIP**, never silently guessed.

## Layout

```
svrf-drc/
  svrf_klayout/svrf_parse.py       # pure-Python SVRF deck parser (unit-testable, no pya)
  svrf_klayout/run_svrf_drc.py     # the in-KLayout interpreter (Engine; uses pya)
  svrf_klayout/edge_pair_check.py  # from-first-principles reference geometry (dual-track cross-check)
  pymacros/svrf_drc.lym            # KLayout macro: Tools -> "SVRF-native DRC…" + run_svrf_deck()
  examples/demo.rule, conn.rule    # synthetic decks (NO vendor data)
  gen/                             # test-structure generators
  tests/                           # pytest (pure Python)
  proof.py                         # self-contained FAIL->PASS proof (run in KLayout)
```

## Run

```bash
# batch, no GUI:
klayout -b -r svrf_klayout/run_svrf_drc.py \
    -rd root=$PWD -rd deck=examples/demo.rule -rd layout=build/test.gds -rd report=out.txt

# self-proof (generates structures, asserts verdicts):
klayout -b -r proof.py -rd root=$PWD
# -> "SVRF-DRC PROOF: PASS"

# parser unit tests (no KLayout needed):
python3 -m pytest tests/ -q
```

## Dual-track honesty

`edge_pair_check.py` is an independent from-first-principles implementation of the
same standard geometry (Euclidian/Projection metric, `angle_limit`/ABUT gate). It
lets the interpreter's verdicts be **cross-checked without any commercial tool** —
turning a "golden reference run" from a semantic oracle into a regression test.

Contains **no vendor / foundry data** — only general SVRF semantics.
