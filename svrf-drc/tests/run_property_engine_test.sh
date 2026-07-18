#!/usr/bin/env bash
# run_property_engine_test.sh -- UNFAKEABLE gate for the eqDRC PROPERTY operator
# (fork feature #8): equation-based DRC evaluated DIRECTLY on db::SVRFEngine.
#
# Proves, against the fork's real native C++ engine (no mocks, no Python interp):
#   1. a 1x20 um sliver has isoperimetric ratio (PERIMETER*PERIMETER)/AREA == 88.2
#      (HAND-COMPUTED) and is FLAGGED by COMPACT (report line FAIL COMPACT -> 1),
#      while the co-resident 5x5 um square (ratio 16.0) is NOT flagged;
#   2. the engine's per-shape computed value equals the hand-computed value to
#      the bit (SVRFDRC_PROPVAL diagnostic: one shape val=16.0 -> ok, one 88.2 -> FAIL);
#   3. a CLEAN layout (square only) PASSes (COMPACT -> 0);
#   4. PROVEN-NEGATIVE: raising the threshold above 88.2 makes the sliver PASS too
#      (the check reproduces the real value, it is not a constant "always-fail");
#   5. the 6 frozen engine goldens stay byte-identical (0 regressions).
#
# NO vendor data. GDS fixtures are built by container klayout `pya` (NOT the SVRF
# interpreter). Skips (exit 0) when the db build inputs or a container are absent.
#
# Env: KLAYOUT_SRC/BLD/BIN (default ~/kbuild, ~/kbuild-out/bld, ~/kbuild-out/bin)
#      EDA_IMAGE container w/ klayout (default vibeic/vibeic-eda:0.2.18)
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
SVRF="$(cd "$HERE/.." && pwd)"
ROOT="$(cd "$SVRF/.." && pwd)"
DBP="$ROOT/src/plugins/tools/svrf_drc/db_plugin"
RULE="$SVRF/examples/property.rule"
KSRC="${KLAYOUT_SRC:-$HOME/kbuild}"
KBLD="${KLAYOUT_BLD:-$HOME/kbuild-out/bld}"
KBIN="${KLAYOUT_BIN:-$HOME/kbuild-out/bin}"
IMAGE="${EDA_IMAGE:-vibeic/vibeic-eda:0.2.18}"

if [ ! -d "$KSRC/src/db/db" ] || [ ! -f "$KBIN/libklayout_db.so" ]; then
  echo "SKIP run_property_engine_test: no KLayout db build (set KLAYOUT_SRC/BLD/BIN)"; exit 0
fi
if ! command -v docker >/dev/null 2>&1; then
  echo "SKIP run_property_engine_test: no docker for GDS generation"; exit 0
fi

WORK="$(mktemp -d)"; trap 'rm -rf "$WORK"' EXIT
export LD_LIBRARY_PATH="$KBIN:$KBIN/db_plugins:${LD_LIBRARY_PATH:-}"
fail() { echo "FAIL: $1"; exit 1; }

echo "== build engine_smoke =="
BIN="$WORK/engine_smoke"
g++ -std=c++17 -O1 -DHAVE_PYTHON \
  -I"$DBP" -I"$KSRC/src/db/db" -I"$KBLD/db/db" -I"$KSRC/src/tl/tl" -I"$KBLD/tl/tl" -I"$KSRC/src/gsi/gsi" \
  "$HERE/engine_smoke.cc" "$DBP/dbSVRFDeck.cc" "$DBP/dbSVRFEngine.cc" \
  -L"$KBIN" -lklayout_db -lklayout_tl -lklayout_gsi \
  -Wl,--no-as-needed -L"$KBIN/db_plugins" -lgds2 -Wl,--as-needed \
  -Wl,-rpath,"$KBIN" -Wl,-rpath,"$KBIN/db_plugins" -o "$BIN" || fail "engine_smoke build failed"

echo "== generate synthetic fixtures (container klayout pya) =="
cp "$HERE/gen_property_gds.py" "$WORK/"
docker run --rm --entrypoint bash -v "$WORK":/work "$IMAGE" -lc "
  export PATH=/foss/tools/klayout:\$PATH
  PROP_MODE=viol  PROP_OUT=/work/viol.gds  klayout -b -r /work/gen_property_gds.py  >/dev/null 2>&1
  PROP_MODE=clean PROP_OUT=/work/clean.gds klayout -b -r /work/gen_property_gds.py >/dev/null 2>&1
"
[ -s "$WORK/viol.gds" ] && [ -s "$WORK/clean.gds" ] || fail "fixture generation produced no GDS"

echo "== 1. sliver FLAGGED, square NOT; COMPACT -> 1 =="
SVRFDRC_PROPVAL=1 "$BIN" "$RULE" "$WORK/viol.gds" "$WORK/viol.rpt" "KLayout 0.30.9" 2> "$WORK/viol.diag"
grep -qE '^FAIL +COMPACT .* -> 1$' "$WORK/viol.rpt" || { echo "--- report ---"; cat "$WORK/viol.rpt"; fail "COMPACT should FAIL with exactly 1 violation"; }
echo "  [1] $(grep -E '^FAIL +COMPACT' "$WORK/viol.rpt")"

echo "== 2. per-shape value == hand-computed (16.0 ok, 88.2 FAIL) =="
grep -qE 'PROPVAL COMPACT #[0-9]+ val=16\.000000 cmp > thr=40\.000000 -> ok'   "$WORK/viol.diag" || { cat "$WORK/viol.diag"; fail "square ratio 16.0 not reproduced"; }
grep -qE 'PROPVAL COMPACT #[0-9]+ val=88\.200000 cmp > thr=40\.000000 -> FAIL' "$WORK/viol.diag" || { cat "$WORK/viol.diag"; fail "sliver ratio 88.2 not reproduced"; }
echo "  [2] $(grep -E 'val=88.200000' "$WORK/viol.diag")"

echo "== 3. clean layout (square only) PASSes =="
"$BIN" "$RULE" "$WORK/clean.gds" "$WORK/clean.rpt" "KLayout 0.30.9" 2>/dev/null
grep -qE '^PASS +COMPACT .* -> 0$' "$WORK/clean.rpt" || { cat "$WORK/clean.rpt"; fail "COMPACT should PASS on the clean layout"; }
echo "  [3] $(grep -E '^PASS +COMPACT' "$WORK/clean.rpt")"

echo "== 4. PROVEN-NEGATIVE: threshold above 88.2 makes the sliver PASS =="
sed 's/> 40.0/> 90.0/' "$RULE" > "$WORK/property_hi.rule"
"$BIN" "$WORK/property_hi.rule" "$WORK/viol.gds" "$WORK/hi.rpt" "KLayout 0.30.9" 2>/dev/null
grep -qE '^PASS +COMPACT .* -> 0$' "$WORK/hi.rpt" || { cat "$WORK/hi.rpt"; fail "with threshold 90 (>88.2) the sliver must PASS -- constant-fail would fail this"; }
echo "  [4] threshold 90 -> $(grep -E '^(PASS|FAIL) +COMPACT' "$WORK/hi.rpt")"

echo "== 5. 0 regressions: 6 frozen goldens byte-identical =="
EDA_IMAGE="$IMAGE" bash "$HERE/run_engine_parity.sh" >/dev/null 2>&1 || fail "engine parity regression FAILED"
echo "  [5] run_engine_parity.sh: all frozen goldens byte-identical"

echo "PASS run_property_engine_test (5/5)"
