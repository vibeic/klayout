# svrf-drc — SVRF-native DRC, built into KLayout (vibeic fork)

KLayout reads a **Calibre/SVRF-format DRC deck** (`.rule`) and executes every rule
**directly on its own DRC engine** — `db::Region::*_check` for geometry and
`db::LayoutToNetlist` for connectivity. There is **no transcoder / compiler, no
intermediate `.drc` file, and no scripting interpreter**: the deck is parsed and
solved **natively in C++**, shipped as the `svrfdrc` command-line buddy.

> **Native C++, not a Python interpreter.** The parser (`db::parse_deck`) and the
> engine (`db::SVRFEngine`) live in `src/plugins/tools/svrf_drc/db_plugin/`
> (`dbSVRFDeck.{h,cc}` + `dbSVRFEngine.{h,cc}`), compiled into the fork's
> `libklayout_bd.so`; the `svrfdrc(int, char**)` entry is in
> `src/buddies/src/bd/svrfdrc.cc`. The earlier Python reference implementation
> (`svrf_klayout/*.py` run via `klayout -b -r`) has been **retired** — its report
> output is reproduced byte-for-byte by the C++ engine (proven on the real ~87 k-line
> foundry deck and on the synthetic corpora; see *Testing*), and the frozen goldens
> it produced remain the regression oracle.

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
| `EXPAND EDGE a [INSIDE\|OUTSIDE] BY n` | `edges().extended(...,n,...)` (→ Region strip) |
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
src/plugins/tools/svrf_drc/db_plugin/
  dbSVRFDeck.{h,cc}     # native C++ SVRF deck parser  (db::parse_deck)
  dbSVRFEngine.{h,cc}   # native C++ engine            (db::SVRFEngine) — byte-identical report
src/buddies/src/bd/svrfdrc.cc         # the svrfdrc(argc,argv) buddy entry (BD_TARGET dispatch)
src/buddies/src/svrfdrc/svrfdrc.pro   # per-buddy .pro (linked into the `svrfdrc` CLI)

svrf-drc/
  examples/*.rule       # synthetic decks (NO vendor data): demo, conn, coverage(2), opdiff, empty
  gen/                  # pya test-structure generators (no interpreter)
  tests/
    *.golden            # FROZEN oracle (parser dumps + engine reports; regression source of truth)
    dump_parse_cpp.cc   # C++ parse-dump tool (diffed vs demo/conn/coverage.golden)
    engine_smoke.cc     # C++ engine driver (diffed vs engine_*.golden)
    gen_*_gds.py        # pya GDS-fixture builders (klayout -b -r; NOT the SVRF interpreter)
    run_parse_parity.sh # build dump_parse_cpp + diff vs frozen parser goldens
    run_engine_parity.sh# build engine_smoke + gen fixtures + diff vs frozen engine goldens
```

## Coverage

Every statement of a real production Calibre DRC deck (a commercial 180 nm foundry
deck, ~87 k lines) dispatches to a native KLayout check — **0 SKIP for any modeled
rule** (`ANTENNA` is the sole honest cross-tool route). The native engine reproduces
the retired Python reference **byte-for-byte** on that deck (identical tally + every
rule line), which is the load-bearing correctness gate for the cutover.

## Run

```bash
# native buddy — batch, no GUI, no Python:
svrfdrc <deck.rule> <layout.gds> <report.txt> [--cell=<TOP>]
#   e.g. svrfdrc examples/demo.rule build/test.gds out.txt --cell=TOP

# (shipped on PATH inside the vibeic-eda image as /foss/tools/bin/svrfdrc)
svrfdrc --help
```

## Testing (no Python / no commercial tool)

The regression oracle is the set of **frozen goldens** in `tests/` — parser dumps
(`demo`/`conn`/`coverage`) and engine reports (`coverage`/`coverage2`/`opdiff`/`empty`)
originally produced by the reference Python implementation and now reproduced
byte-for-byte by the C++ parser and engine:

```bash
# parser parity: C++ db::parse_deck dump == frozen golden
bash tests/run_parse_parity.sh

# engine parity: C++ db::SVRFEngine report == frozen golden
#   (generates the synthetic GDS via klayout `pya` builders, then runs the native engine)
EDA_IMAGE=ghcr.io/vibeic/vibeic-eda:0.2.11 bash tests/run_engine_parity.sh
```

Both need a KLayout db build (`KLAYOUT_SRC`/`KLAYOUT_BLD`/`KLAYOUT_BIN`, default
`~/kbuild` + `~/kbuild-out`) to compile the C++ test drivers. No SVRF interpreter is
involved on either side — the goldens are static, so the comparison is a pure
regression test rather than a live semantic oracle.

Contains **no vendor / foundry data** — only general SVRF semantics.
