#!/usr/bin/env bash
#
# run_engine_parity.sh -- prove the NATIVE C++ engine (db::SVRFEngine) is
# byte-for-byte identical to the FROZEN reference goldens on SYNTHETIC decks +
# layouts. NO vendor data anywhere in this test.
#
# The goldens (tests/engine_<name>.golden) were produced by the reference Python
# interpreter (run_svrf_drc.py) and COMMITTED as the frozen oracle. The Python
# interpreter has since been RETIRED (the native C++ buddy is the shipped path),
# so this test no longer regenerates them live -- it diffs the native engine's
# output against the committed golden. To re-freeze after an intentional engine
# change, delete the golden and re-run once against a trusted reference.
#
# It:
#   1. builds engine_smoke (dbSVRFDeck.cc + dbSVRFEngine.cc + engine_smoke.cc)
#      against a KLayout db build (KLAYOUT_SRC + KLAYOUT_BLD + KLAYOUT_BIN),
#   2. generates the synthetic GDS fixtures inside the vibeic-eda container
#      (klayout `pya` Layout builders -- NOT the SVRF interpreter),
#   3. runs the native engine on those inputs and diffs against the frozen golden.
#
# Env (override as needed):
#   KLAYOUT_SRC   klayout source tree      (default ~/kbuild)
#   KLAYOUT_BLD   klayout build dir        (default ~/kbuild-out/bld)
#   KLAYOUT_BIN   klayout installed libs   (default ~/kbuild-out/bin)
#   EDA_IMAGE     container w/ klayout     (default ghcr.io/vibeic/vibeic-eda:0.2.11)
#
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
SVRF="$(cd "$HERE/.." && pwd)"                       # svrf-drc/
DBP="$SVRF/../src/plugins/tools/svrf_drc/db_plugin"  # dbSVRFDeck/Engine sources

KSRC="${KLAYOUT_SRC:-$HOME/kbuild}"
KBLD="${KLAYOUT_BLD:-$HOME/kbuild-out/bld}"
KBIN="${KLAYOUT_BIN:-$HOME/kbuild-out/bin}"
IMAGE="${EDA_IMAGE:-ghcr.io/vibeic/vibeic-eda:0.2.10}"
WORK="$(mktemp -d)"
BIN="$WORK/engine_smoke"

echo "== 1. build engine_smoke =="
g++ -std=c++17 -O1 -DHAVE_PYTHON \
  -I"$DBP" -I"$KSRC/src/db/db" -I"$KBLD/db/db" -I"$KSRC/src/tl/tl" -I"$KBLD/tl/tl" -I"$KSRC/src/gsi/gsi" \
  "$HERE/engine_smoke.cc" "$DBP/dbSVRFDeck.cc" "$DBP/dbSVRFEngine.cc" \
  -L"$KBIN" -lklayout_db -lklayout_tl -lklayout_gsi \
  -Wl,--no-as-needed -L"$KBIN/db_plugins" -lgds2 -Wl,--as-needed \
  -Wl,-rpath,"$KBIN" -Wl,-rpath,"$KBIN/db_plugins" -o "$BIN"

#  corpus rows: "<deck-basename> <gds-basename> <gen-script>"
#    coverage   -- every dispatch branch (4 FAIL + 4 PASS + 1 SKIP)
#    coverage2  -- separation/enclosure/notch/width/bool-COPY forced non-zero
#    opdiff     -- select(INTERACT/CUT/NOT-INTERACT) + prefix OR/NOT/XOR + NET AREA RATIO
#    empty      -- boolean/select ops with an EMPTY operand (the pflag key-vs-value bug)
#    antenna    -- native in-engine ANTENNA op (#20): 2-layer staged charge-ratio FAIL +
#                  PASS + one-layer honest-SKIP. Deep gate: run_antenna_engine_test.sh.
#    property   -- eqDRC PROPERTY op (#8): (PERIMETER*PERIMETER)/AREA sliver detector;
#                  square 16.0 PASS-shape + sliver 88.2 FAIL-shape -> FAIL 1. Deep gate
#                  (hand-computed per-shape value): run_property_engine_test.sh.
CORPORA=(
  "coverage  synth  gen_synth_gds.py"
  "coverage2 synth2 gen_synth2_gds.py"
  "opdiff    opdiff gen_opdiff_gds.py"
  "empty     empty  gen_empty_gds.py"
  "corner    corner gen_corner_gds.py"
  "antenna   antenna gen_antenna_gds.py"
  "property  property gen_property_gds.py"
)
for row in "${CORPORA[@]}"; do
  set -- $row
  cp "$SVRF/examples/$1.rule" "$WORK/"
  cp "$HERE/$3"               "$WORK/"
done

echo "== 2. generate synthetic GDS fixtures (container klayout pya) =="
GEN=""
for row in "${CORPORA[@]}"; do
  set -- $row
  GEN="$GEN klayout -b -r /work/$3 >/dev/null 2>&1;"
done
docker run --rm --entrypoint bash -v "$WORK":/work "$IMAGE" -lc "
  export PATH=/foss/tools/klayout:\$PATH
  $GEN
"

echo "== 3. run native engine + diff vs FROZEN golden =="
rc=0
for row in "${CORPORA[@]}"; do
  set -- $row
  deck="$1"; gds="$2"
  golden="$HERE/engine_${deck}.golden"
  if [ ! -f "$golden" ]; then
    echo "  $deck: FAIL (no frozen golden at $golden)"; rc=1; continue
  fi
  "$BIN" "$WORK/$deck.rule" "$WORK/$gds.gds" "$WORK/${deck}_cpp.txt" \
     "KLayout 0.30.9" "/work/$deck.rule" "/work/$gds.gds"
  if diff "$golden" "$WORK/${deck}_cpp.txt" >/dev/null; then
    echo "  $deck: PASS (byte-identical to frozen golden)"
  else
    echo "  $deck: FAIL"; diff "$golden" "$WORK/${deck}_cpp.txt" || true; rc=1
  fi
done

rm -rf "$WORK"
exit $rc
