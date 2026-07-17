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
   `<regex>` only, no db:: deps). Cross-checked byte-for-byte against the frozen
   parser-dump goldens (`demo`/`conn`/`coverage`) via `tests/dump_parse_cpp.cc` +
   `tests/run_parse_parity.sh`. **[DONE]**
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
   Python sparse-checkout. Rewire the plugin touchpoints (`phase3_one_shot_runner
   ._try_svrf_native_drc` → run `svrfdrc`; `_svrfdrc_bin_container` → probe the native
   binary; `drc-fix` SKILL + 2 tests reword). **Verify byte-parity native-vs-Python on the
   the commercial deck BEFORE deleting Python.** Then remove `svrf_klayout/*.py` + `.lym`. **[DONE]**
   - **Image `vibeic-eda:0.2.11`** ships the buddy at `/foss/tools/bin/svrfdrc`. Stage-7
     Python source-checkout retired; the runtime install is a **wrapper** (not a bare
     symlink): the buddy's ELF carries `DT_RUNPATH=/foss/tools/klayout-vibeic`, but the
     runtime env sets `LD_LIBRARY_PATH=/foss/tools/klayout` and **DT_RUNPATH is searched
     AFTER LD_LIBRARY_PATH** — a bare symlink therefore loads the STOCK
     `/foss/tools/klayout/libklayout_bd.so` (which lacks the `svrfdrc` symbol + engine) →
     `undefined symbol: svrfdrc(int, char**)`. The wrapper prepends the fork lib dir to
     `LD_LIBRARY_PATH` so all klayout libs resolve from the fork build. The build-time
     self-test now runs under `LD_LIBRARY_PATH=/foss/tools/klayout` to reproduce the
     runtime condition (catches the bug at build time).
   - **★ IN-IMAGE SAFETY GATE PASSED:** the baked wrapper `svrfdrc` (resolved via
     `command -v svrfdrc` exactly as the plugin does, under the real runtime env) run on
     the REAL commercial deck + spm sign-off GDS produces a report **byte-identical** to the
     retired Python golden — 4,533 verdict lines, all 10 FAIL lines match, tally
     `{'PASS': 4523, 'FAIL': 10}` (only the argv path-echo header line differs, not a
     result). 0.92 s. This cleared Python for deletion.
   - **Python deleted** from the fork: `svrf_klayout/{__init__,edge_pair_check,run_svrf_drc,
     svrf_parse}.py`, `pymacros/svrf_drc.lym`, the interpreter-dependent dev/proof scripts
     (`audit_realdeck.py`, `proof.py`, `proof2.py`), and the Python-only tests
     (`tests/{test_svrf_parse,test_edge_pair_check,dump_parse}.py`). Kept: the pya GDS/
     test-structure builders (`gen/`, `tests/gen_*_gds.py`) — those use `pya.Layout`, not
     the SVRF interpreter.
   - **Regression preserved without Python:** `tests/run_parse_parity.sh` (C++ parser dump
     vs frozen `demo`/`conn`/`coverage` goldens) + `tests/run_engine_parity.sh` (C++ engine
     vs frozen `coverage`/`coverage2`/`opdiff`/`empty` goldens, now diffing the COMMITTED
     goldens instead of live-generating them via Python). Both pass byte-identical
     post-deletion.

   **★ REAL-DECK PARITY GATE PASSED (the load-bearing safety check for step 4).**
   The native `svrfdrc` buddy binary is **byte-identical** to the reference
   `run_svrf_drc.py` on the REAL commercial (NDA) deck
   (the commercial DRC rule deck, 87,620 lines → 224 layers / 15,897 derivations /
   4,533 rules) run on the spm sign-off GDS — final tally `{'PASS': 4523, 'FAIL': 10}`
   on both sides, empty diff. Buddy: 1.03 s, 100 MB. (Deck is NDA — verified locally,
   never committed; test fixtures here are all SYNTHETIC.)
   - **Two real engine bugs the synthetic corpora missed, found by this gate + fixed:**
     1. **net-area-ratio copy-vs-reference:** `net_area_ratio` took the registered
        operand region BY COPY; `LayoutToNetlist::shapes_of_net` resolves the layer via
        the DSS `layer_for_flat` map keyed on delegate identity, so a copy threw
        "Non-hierarchical layers cannot be used in netlist extraction" → the derivation
        went `unmodeled` → COPY reported SKIP instead of PASS. Fix: hold the registered
        regions by reference (mirrors the reference `reg.get(nm)`).
     2. **boolean-param key-vs-value (general):** the C++ parser stores boolean select/
        derivation flags (`negate`/`inside`/`outside`/`inner`/`outer`) as the string
        `"1"`/`"0"` with the KEY ALWAYS PRESENT; the engine tested `params.find(k)!=end()`
        (key existence → always true) instead of the value. So e.g. `INTERACT a b` was
        executed as `NOT INTERACT` → `a interacting <empty> = a` instead of `= {}` and a
        whole fuse/mom chain fired. Fix: a `pflag()` helper testing value `== "1"`, wired
        into all 5 flag sites. (Value-bearing params w/h/aspect/cmp/thr are set
        conditionally, so key-presence stays correct there.)
   - Regression fixtures added (SYNTHETIC): `examples/opdiff.rule` (select/prefix-bool/
     net-ratio) + `examples/empty.rule` (ops with an empty operand — the pflag bug);
     `tests/run_engine_parity.sh` now diffs all 4 corpora.

   All step-4 work complete — the native C++ buddy is the shipped DRC path; the Python
   interpreter is fully retired.

## Effort / risk

~1.5–3 engineer-weeks (parser mechanical; engine + RegionCheckOptions mapping +
DeepShapeStore/tiling for scale is the bulk; deps well-specified by the tested Python
reference + `proof.py`/`proof2.py` oracle). Migration is native-first → verify → delete;
the working Python engine is NEVER removed before parity is proven.

Vendor hygiene: all decks/tests here are SYNTHETIC (no foundry/NDA content).
