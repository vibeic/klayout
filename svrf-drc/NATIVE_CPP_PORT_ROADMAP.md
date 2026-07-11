# Native C++ SVRF/Calibre DRC — port roadmap (kill the Python interpreter)

**Directive (owner, 2026-07-12):** SVRF/Calibre `.rule` DRC-deck reading must be
**native C++ inside the KLayout fork** — NOT a Python interpreter invoked via
`klayout -b -r run_svrf_drc.py`. Delete the `svrf_klayout/*.py` interpreter + the
`.lym` macro. "Just run KLayout's own native tool."

## Architecture decision — a native KLayout **buddy** executable

KLayout's *own* DRC engine is Ruby (`src/drc/drc/built-in-macros/_drc_engine.rb`)
driving GSI-exposed `db::Region` C++ checks; there is **no** native C++ DRC-deck
reader upstream. Two native templates exist in-tree:

- **(A) db_plugin + GSI class** (`src/plugins/tools/net_tracer`) — native C++ in the
  klayout binary, but still needs a *script* to call `RBA::…run()` → reintroduces a
  Python/Ruby launcher. ✗ (violates "no interpreter/script").
- **(B) buddy standalone executable** (`src/buddies/src/bd/strmxor.cc`) — own
  `main()` + `tl::CommandLineParser`, links `db::` directly, ships with klayout.
  **No script, no pya, no `-r`.** ✓ **CHOSEN.**

Target invocation (replaces `klayout -b -r run_svrf_drc.py -rd deck=… -rd layout=…`):

```
svrfdrc <deck.rule> <layout.gds> <report.txt> [--cell TOP]
```

The parser/engine are reusable C++ classes (`db::SVRFDeck`, `db::SVRFEngine`) so they
can *also* be GSI-exposed later; but the plugin's invocation path is the native
buddy binary only.

## HARD CONTRACTS (must not break)

1. **Report format — byte-identical** to `run_svrf_drc.py::main()`:
   ```
   # SVRF-native DRC via KLayout <ver>
   # deck=<...>  layout=<...>  dbu=<...>
   # <N> layers, <M> derivations, <K> rules  |  {tally}
   <blank>
   <VERDICT:5s> <name:18s> <op> <l1>[/<l2>] <cmp> <value> [tag] -> <count>
   ...
   <blank>
   # tally: {...}
   ```
   Because the vibe-ic plugin's `_parse_svrf_tally` + `_classify_svrf_fails` (and the
   MARKER_ABSENT/DENSITY_FILL/GEOMETRY classifier tests) parse these lines + the
   `COPY __marker__ … -> 0` empty-marker lines. Freeze it; do not reword.
2. **honest-SKIP, never false-PASS** — ANTENNA (charge ratio → dedicated checker),
   edge-typed inputs with no polygon-DRC equivalent, connectivity with no CONNECT
   stack, net layers with no extracted shapes, unmodeled derivations → SKIP. §4.05
   no-cheat: a rewrite that turns a SKIP into a PASS is a silicon-DOA hazard.

## Increments (each committed on branch `vibeic/svrf-native-drc`)

1. **C++ parser `db::SVRFDeck`** — port `svrf_klayout/svrf_parse.py` (self-contained,
   `<regex>` only, no db:: deps). Cross-checked byte-for-byte against the Python
   parser via `tests/dump_parse.py` goldens (`demo`/`conn`/`coverage`). *[in progress]*
2. **C++ engine `db::SVRFEngine`** — port `run_svrf_drc.py`; dispatch each rule onto
   `db::Region::{space,separation,width,overlap,notch,enclosing}_check` (+
   `RegionCheckOptions`), `db::Edges`, `db::LayoutToNetlist` (CONNECT / NET AREA
   RATIO), windowed DENSITY, boolean/size/select/metric/rectangle/vertex derivations.
   Emits the frozen report format. Needs the KLayout build to compile. **[DONE]**
   - `db_plugin/dbSVRFEngine.{h,cc}` — full port. pya→raw db:: mapping: convenience
     methods (`with_area`/`rectangles`/`interacting`/`extents`/edge `with_length`/…)
     rebuilt via `db::Region::filtered(db::RegionAreaFilter/RectangleFilter/
     RegionBBoxFilter/RegionRatioFilter)`, `selected_interacting/inside/outside`,
     `processed(extents_processor)`, `db::Edges::filtered(EdgeLengthFilter/
     EdgeOrientationFilter)`, `extended(out,…)`. Connectivity via the REAL
     `db::Region::nets(l2n, NPM_NetQualifiedNameOnly, "net")` + `SamePropertiesConstraint`
     / `DifferentPropertiesConstraint`. Report float/dict formatting reproduces Python
     `str(float)` (`std::to_chars` + re-added `.0`) and `dict(Counter)` (first-appearance
     order) EXACTLY.
   - **Name clash fix:** the parser enum `Connectivity` was renamed `SVRFConnectivity`
     (KLayout's `db` namespace already has a `class Connectivity`); the parser dump
     goldens (demo/conn/coverage) still pass unchanged.
   - **Byte-parity PROVEN, two synthetic corpora, C++ ≡ Python `run_svrf_drc.py`**
     (identical KLayout 0.30.9 both sides): `tests/run_engine_parity.sh` builds
     `engine_smoke`, generates the fixtures + Python golden in the vibeic-eda container,
     runs the native engine, and diffs. corpus-1 (`coverage.rule`) exercises every
     dispatch branch (4 FAIL: space/width/density/COPY, 4 PASS, 1 ANTENNA SKIP); corpus-2
     (`coverage2.rule`) forces separation/enclosure/notch/width/boolean-COPY to non-zero
     (5 FAIL). Both **byte-identical**. Fixtures: `examples/coverage2.rule`,
     `tests/gen_synth{,2}_gds.py`, `tests/engine_coverage{,2}.golden`, `tests/engine_smoke.cc`.
   - **Side-finding:** the vibeic-eda:0.2.10 image ships a STALE `svrf_parse.py` (416 lines
     vs the fork's current 522) — moot, since increment 4 re-bakes the image to the native
     C++ binary and drops the Python entirely.
3. **buddy `svrfdrc`** — `src/buddies/src/bd/svrfdrc.cc` (mirror `strmxor.cc`), add to
   `bd.pro` SOURCES + `src/buddies/src/svrfdrc/svrfdrc.pro` + `src.pro`. **[DONE]**
   - `bd/svrfdrc.cc`: `BD_PUBLIC int svrfdrc(argc,argv)` — `tl::CommandLineOptions`
     parses `svrfdrc <deck> <layout> <report> [--cell=TOP]`, reads the deck,
     `db::parse_deck` → `db::SVRFEngine` → `write_report`. Version string
     "KLayout <KLAYOUT_VERSION>" built from the compile macro (NOT version.h, whose
     file-scope globals would multiply-define in klayout_bd).
   - **Build wiring:** `bd.pro` compiles `svrfdrc.cc` + the two engine sources
     (`$$PWD/../../../plugins/tools/svrf_drc/db_plugin/dbSVRF{Deck,Engine}.cc`) into
     `libklayout_bd` (+ that dir on INCLUDEPATH); `svrfdrc/svrfdrc.pro` =
     `include(buddy_app.pri)` (TARGET auto = svrfdrc, main from bd/main.cc via
     `BD_TARGET`); `src.pro` SUBDIRS += svrfdrc, `svrfdrc.depends += bd`. The
     `src/plugins/tools/svrf_drc/svrf_drc.pro` is an EMPTY `TEMPLATE=subdirs` project
     so the `tools.pro` auto-glob (`$files($$PWD/*)`) accepts the folder without
     building a module of its own (the sources ship via klayout_bd).
   - **Engine ported to `-std=c++11`** (the KLayout build standard, not c++17):
     `py_float_str` reimplemented without `<charconv>` (shortest round-trip via
     increasing-precision `%f`/`%g`, Python fixed-vs-scientific threshold at
     exp∈[-4,16); unit-checked against Python `str()` over 25 values incl. 400.0/4000.0);
     `SVRFStatement` given explicit ctors (default-member-initializer disqualifies
     aggregate init under c++11).
   - **VERIFIED end-to-end:** built via `build.sh -without-qt -noruby` (KLayout 0.30.9);
     `bin/svrfdrc` runs as `svrfdrc <deck> <layout> <report>` (NO script, NO `-r`, NO
     pya) and is **byte-identical** to the reference Python on both synthetic corpora.
4. **container re-bake + plugin rewire + delete Python** — bump vibeic-eda
   `ARG KLAYOUT_REF`; Stage-6 recompiles klayout (buddy ships in `bld`); drop Stage-7
   Python sparse-checkout. Rewire the **6** plugin touchpoints (`phase3_one_shot_runner
   ._try_svrf_native_drc` → run `svrfdrc`; `_svrf_drc_root*` → probe the native binary;
   `drc-fix` SKILL + 2 tests reword). **Verify byte-parity native-vs-Python on the
   commercial-PDK deck BEFORE deleting Python.** Then remove `svrf_klayout/*.py` + `.lym`.

## Effort / risk

~1.5–3 engineer-weeks (parser mechanical; engine + RegionCheckOptions mapping +
DeepShapeStore/tiling for scale is the bulk; deps well-specified by the tested Python
reference + `proof.py`/`proof2.py` oracle). Migration is native-first → verify → delete;
the working Python engine is NEVER removed before parity is proven.

Vendor hygiene: all decks/tests here are SYNTHETIC (no foundry/NDA content).
