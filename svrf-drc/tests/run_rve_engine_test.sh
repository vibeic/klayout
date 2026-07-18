#!/usr/bin/env bash
# run_rve_engine_test.sh -- UNFAKEABLE gate for the RVE-style result database
# (fork feature #9): db::SVRFEngine emits a KLayout-loadable .lyrdb marker DB
# built from the SAME frozen per-rule error regions the report counts.
#
# Fixture: two 0.5x0.5 um squares (AREA 0.25 um^2 -> FAIL AREA<0.3) + one 5x5 um
# square (PASS). Proves, against the fork's real native C++ engine AND KLayout's
# OWN rdb reader (pya.ReportDatabase):
#   1. viol  -> the DB has exactly ONE category (SR_MINAREA) and the two markers
#      load back at the HAND-COMPUTED um bboxes [2,2,2.5,2.5] and [8,8,8.5,8.5];
#      the PASSing CLEAN_SP rule contributes NO category and NO item;
#   2. PROVEN-GEOMETRY: moving square B to [12,12,12.5,12.5] moves the reported
#      bbox to exactly there (the coordinates are read from the geometry, not a
#      constant -- a hardcoded emitter would still say 8,8);
#   3. PROVEN-NEGATIVE: a clean layout (no small squares) -> 0 categories, 0
#      items (the DB is not a constant-nonempty);
#   4. env unset -> NO .lyrdb written + the per-rule report is unchanged (parity);
#   5. the frozen engine goldens stay byte-identical (0 regressions).
#
# NO vendor data. Skips (exit 0) when the db build inputs or a container are absent.
# Env: KLAYOUT_SRC/BLD/BIN + EDA_IMAGE (default vibeic/vibeic-eda:0.2.18)
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
SVRF="$(cd "$HERE/.." && pwd)"
ROOT="$(cd "$SVRF/.." && pwd)"
DBP="$ROOT/src/plugins/tools/svrf_drc/db_plugin"
RULE="$SVRF/examples/rve.rule"
KSRC="${KLAYOUT_SRC:-$HOME/kbuild}"
KBLD="${KLAYOUT_BLD:-$HOME/kbuild-out/bld}"
KBIN="${KLAYOUT_BIN:-$HOME/kbuild-out/bin}"
IMAGE="${EDA_IMAGE:-vibeic/vibeic-eda:0.2.18}"

if [ ! -d "$KSRC/src/db/db" ] || [ ! -f "$KBIN/libklayout_db.so" ]; then
  echo "SKIP run_rve_engine_test: no KLayout db build (set KLAYOUT_SRC/BLD/BIN)"; exit 0
fi
if ! command -v docker >/dev/null 2>&1; then
  echo "SKIP run_rve_engine_test: no docker for GDS generation / rdb load"; exit 0
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
cp "$HERE/gen_rve_gds.py" "$HERE/rve_load.py" "$WORK/"
docker run --rm --entrypoint bash -v "$WORK":/work "$IMAGE" -lc "
  export PATH=/foss/tools/klayout:\$PATH
  RVE_MODE=viol  RVE_OUT=/work/viol.gds  klayout -b -r /work/gen_rve_gds.py >/dev/null 2>&1
  RVE_MODE=moved RVE_OUT=/work/moved.gds klayout -b -r /work/gen_rve_gds.py >/dev/null 2>&1
  RVE_MODE=clean RVE_OUT=/work/clean.gds klayout -b -r /work/gen_rve_gds.py >/dev/null 2>&1
"
for g in viol moved clean; do [ -s "$WORK/$g.gds" ] || fail "fixture $g generation produced no GDS"; done

#  load a .lyrdb through KLayout's own rdb reader -> canonical digest
load() { docker run --rm --entrypoint bash -v "$WORK":/work "$IMAGE" -lc "
  export PATH=/foss/tools/klayout:\$PATH
  LYRDB=/work/$1 klayout -b -r /work/rve_load.py 2>/dev/null | grep -E '^(CAT|BBOX|NITEMS) '
"; }

echo "== 1. viol: 1 category, 2 markers at hand-computed bboxes, PASS rule excluded =="
SVRFDRC_RVE_OUT="$WORK/viol.lyrdb" "$BIN" "$RULE" "$WORK/viol.gds" "$WORK/viol.rpt" "KLayout 0.30.9" 2>/dev/null
[ -s "$WORK/viol.lyrdb" ] || fail "no .lyrdb emitted for viol"
D="$(load viol.lyrdb)"; echo "$D" | sed 's/^/   /'
echo "$D" | grep -qxE 'CAT SR_MINAREA'                  || { echo "$D"; fail "missing SR_MINAREA category"; }
echo "$D" | grep -qE  'CAT CLEAN_SP'                     && { echo "$D"; fail "PASSing CLEAN_SP must NOT be a category"; }
echo "$D" | grep -qxE 'BBOX SR_MINAREA:2,2,2.5,2.5'      || { echo "$D"; fail "marker A bbox != hand-computed [2,2,2.5,2.5]"; }
echo "$D" | grep -qxE 'BBOX SR_MINAREA:8,8,8.5,8.5'      || { echo "$D"; fail "marker B bbox != hand-computed [8,8,8.5,8.5]"; }
echo "$D" | grep -qxE 'NITEMS 2'                         || { echo "$D"; fail "viol NITEMS != 2"; }
echo "  [1] viol: SR_MINAREA {[2,2,2.5,2.5],[8,8,8.5,8.5]}, CLEAN_SP absent, NITEMS=2 (native rdb load)"

echo "== 2. PROVEN-GEOMETRY: square B moved -> reported bbox moves to [12,12,12.5,12.5] =="
SVRFDRC_RVE_OUT="$WORK/moved.lyrdb" "$BIN" "$RULE" "$WORK/moved.gds" "$WORK/moved.rpt" "KLayout 0.30.9" 2>/dev/null
M="$(load moved.lyrdb)"
echo "$M" | grep -qxE 'BBOX SR_MINAREA:2,2,2.5,2.5'      || { echo "$M"; fail "moved: marker A should stay at [2,2,2.5,2.5]"; }
echo "$M" | grep -qxE 'BBOX SR_MINAREA:12,12,12.5,12.5'  || { echo "$M"; fail "moved: marker B bbox not tracked to [12,12,12.5,12.5] (coords hardcoded?)"; }
echo "$M" | grep -qE  'BBOX SR_MINAREA:8,8,8.5,8.5'      && { echo "$M"; fail "moved: stale [8,8,8.5,8.5] present -> coords NOT read from geometry"; }
echo "  [2] moved: B tracked to [12,12,12.5,12.5], no stale [8,8,...] -> geometry-derived"

echo "== 3. PROVEN-NEGATIVE: clean layout -> 0 categories, 0 items =="
SVRFDRC_RVE_OUT="$WORK/clean.lyrdb" "$BIN" "$RULE" "$WORK/clean.gds" "$WORK/clean.rpt" "KLayout 0.30.9" 2>/dev/null
C="$(load clean.lyrdb)"
echo "$C" | grep -qE 'CAT '                              && { echo "$C"; fail "clean layout must yield NO categories"; }
echo "$C" | grep -qxE 'NITEMS 0'                         || { echo "$C"; fail "clean NITEMS != 0"; }
echo "  [3] clean: 0 categories, NITEMS=0 (DB not a constant-nonempty)"

echo "== 4. env unset: no .lyrdb written + per-rule report unchanged (parity) =="
rm -f "$WORK/none.lyrdb"
"$BIN" "$RULE" "$WORK/viol.gds" "$WORK/none.rpt" "KLayout 0.30.9" 2>"$WORK/none.err"
[ -e "$WORK/none.lyrdb" ] && fail "a .lyrdb leaked with env unset (parity broken)"
grep -q 'SVRFDRC_RVE wrote' "$WORK/none.err" && fail "RVE output leaked with env unset"
diff "$WORK/viol.rpt" "$WORK/none.rpt" >/dev/null || fail "per-rule report differs RVE-on vs RVE-off (RVE must not move verdicts)"
echo "  [4] no .lyrdb emitted; per-rule report byte-identical RVE-on vs RVE-off OK"

echo "== 5. 0 regressions: frozen goldens byte-identical =="
EDA_IMAGE="$IMAGE" bash "$HERE/run_engine_parity.sh" >/dev/null 2>&1 || fail "engine parity regression FAILED"
echo "  [5] run_engine_parity.sh: all frozen goldens byte-identical"

echo "PASS run_rve_engine_test (5/5)"
