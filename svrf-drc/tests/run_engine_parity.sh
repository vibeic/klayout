#!/usr/bin/env bash
#
# run_engine_parity.sh -- prove the NATIVE C++ engine (db::SVRFEngine) is
# byte-for-byte identical to the reference Python interpreter (run_svrf_drc.py)
# on SYNTHETIC decks + layouts. NO vendor data anywhere in this test.
#
# It:
#   1. builds engine_smoke (dbSVRFDeck.cc + dbSVRFEngine.cc + engine_smoke.cc)
#      against a KLayout db build (KLAYOUT_SRC + KLAYOUT_BLD + KLAYOUT_BIN),
#   2. generates the synthetic GDS fixtures + Python golden inside the
#      vibeic-eda container (which ships klayout + this same svrf_klayout),
#   3. runs the native engine on the same inputs and diffs against the golden.
#
# Env (override as needed):
#   KLAYOUT_SRC   klayout source tree      (default ~/kbuild)
#   KLAYOUT_BLD   klayout build dir        (default ~/kbuild-out/bld)
#   KLAYOUT_BIN   klayout installed libs   (default ~/kbuild-out/bin)
#   EDA_IMAGE     container w/ klayout     (default ghcr.io/vibeic/vibeic-eda:0.2.10)
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

cp "$SVRF/examples/coverage.rule"  "$WORK/"
cp "$SVRF/examples/coverage2.rule" "$WORK/"
cp "$HERE/gen_synth_gds.py"        "$WORK/"
cp "$HERE/gen_synth2_gds.py"       "$WORK/"

echo "== 2. generate synthetic GDS + Python golden (container) =="
docker run --rm --entrypoint bash -v "$SVRF":/myfork -v "$WORK":/work "$IMAGE" -lc '
  export PATH=/foss/tools/klayout:$PATH
  klayout -b -r /work/gen_synth_gds.py  >/dev/null 2>&1
  klayout -b -r /work/gen_synth2_gds.py >/dev/null 2>&1
  klayout -b -r /myfork/svrf_klayout/run_svrf_drc.py -rd root=/myfork \
     -rd deck=/work/coverage.rule  -rd layout=/work/synth.gds  -rd report=/work/golden.txt  >/dev/null 2>&1
  klayout -b -r /myfork/svrf_klayout/run_svrf_drc.py -rd root=/myfork \
     -rd deck=/work/coverage2.rule -rd layout=/work/synth2.gds -rd report=/work/golden2.txt >/dev/null 2>&1
'

echo "== 3. run native engine + diff =="
rc=0
for n in "coverage synth golden" "coverage2 synth2 golden2"; do
  set -- $n
  deck="$1"; gds="$2"; gold="$3"
  "$BIN" "$WORK/$deck.rule" "$WORK/$gds.gds" "$WORK/${deck}_cpp.txt" \
     "KLayout 0.30.9" "/work/$deck.rule" "/work/$gds.gds"
  if diff "$WORK/$gold.txt" "$WORK/${deck}_cpp.txt" >/dev/null; then
    echo "  $deck: PASS (byte-identical)"
  else
    echo "  $deck: FAIL"; diff "$WORK/$gold.txt" "$WORK/${deck}_cpp.txt" || true; rc=1
  fi
done

rm -rf "$WORK"
exit $rc
