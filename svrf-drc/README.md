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
| `SIZE / GROW / SHRINK` (+ `OVERUNDER` / `UNDEROVER` / `STEP`) | `sized(±d)` (composed) |
| `INTERACT / INSIDE / OUTSIDE` | `interacting / inside / outside` |
| `CONNECT a b BY via` + `CONNECTED` / `NOT CONNECTED` | `LayoutToNetlist` + `property_constraint=Same/DifferentPropertiesConstraint` |

### Edge pipeline (result is a `pya.Edges` layer; checks dispatch to the Edges engine)

| SVRF / Calibre | KLayout native |
|---|---|
| `a EDGE` / `INNER EDGE` / `OUTER EDGE` | `a.edges()` / `holes().edges()` / `hulls().edges()` |
| `a INSIDE EDGE b` / `a OUTSIDE EDGE b` | `a.edges().inside_part(b)` / `outside_part(b)` |
| `a COINCIDENT [INSIDE\|OUTSIDE] EDGE b` | `a.edges() & b.edges()` (+ direction split) |
| `a TOUCH EDGE b` | `a.edges().interacting(b)` |
| `e LENGTH <rel> n` / `e ANGLE <rel> n` | `with_length(...)` / `with_angle(...)` |
| `EXPAND EDGE a [INSIDE\|OUTSIDE] BY n` | `edges().extended_in/out(n)` (→ Region strip) |
| `EXTERNAL / INTERNAL / ENCLOSURE` on an edge layer | `Edges.separation_check / width_check / enclosing_check` |

### Windowed density & per-net area ratio (native)

| SVRF / Calibre | KLayout native |
|---|---|
| `DENSITY a [b] <rel> d WINDOW w STEP s` | sliding-window `area / window-area` (`Region.area()`) |
| `a NET AREA RATIO b <rel> v` | `LayoutToNetlist` per-net `shapes_of_net(net,·).area()` ratio |

`ANTENNA` (charge-accumulation ratio) is routed to the antenna checker
(OpenROAD / magic fork), not the geometric DRC core — an honest cross-tool route,
not a silent guess. Every geometric / connectivity / density / net-ratio rule in
the Calibre format executes natively here.

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
  proof.py                         # FAIL->PASS proof: geometric + connectivity core (run in KLayout)
  proof2.py                        # FAIL->PASS proof: edge pipeline / density / net-ratio (run in KLayout)
  audit_realdeck.py                # dispatch-coverage auditor for ANY SVRF deck (-rd deck=<path>)
```

## Coverage

Every statement of a real production Calibre DRC deck (a commercial 180 nm foundry
deck, ~87 k lines / ~7.4 k executable rules) dispatches to a native KLayout check —
**0 SKIP, 0 unmodeled** (measured by `audit_realdeck.py`). Dispatch coverage proves
each rule *runs*; `proof.py` + `proof2.py` prove each check *discriminates* (FAILs on
a real violation, PASSes when clean) on targeted geometry.

## Run

```bash
# batch, no GUI:
klayout -b -r svrf_klayout/run_svrf_drc.py \
    -rd root=$PWD -rd deck=examples/demo.rule -rd layout=build/test.gds -rd report=out.txt

# self-proofs (generate structures, assert FAIL<->PASS discrimination):
klayout -b -r proof.py  -rd root=$PWD    # -> "SVRF-DRC PROOF: PASS"
klayout -b -r proof2.py -rd root=$PWD    # -> "SVRF-DRC PROOF2 ... : PASS"

# dispatch-coverage audit of any SVRF deck:
klayout -b -r audit_realdeck.py -rd root=$PWD -rd deck=<your.rule>

# parser unit tests (no KLayout needed):
python3 -m pytest tests/ -q
```

## Dual-track honesty

`edge_pair_check.py` is an independent from-first-principles implementation of the
same standard geometry (Euclidian/Projection metric, `angle_limit`/ABUT gate). It
lets the interpreter's verdicts be **cross-checked without any commercial tool** —
turning a "golden reference run" from a semantic oracle into a regression test.

Contains **no vendor / foundry data** — only general SVRF semantics.
