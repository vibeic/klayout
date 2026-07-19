#!/usr/bin/env bash
# run_critarea_engine_test.sh -- UNFAKEABLE gate for shorts Critical-Area Analysis
# (fork feature #46): the locus of particle-defect centres that bridge two conductors
# at a probe radius rho = union over pairs of (P_i (+)rho) INTERSECT (P_j (+)rho). Its
# area (um^2) is a CLOSED-FORM geometric quantity, hand-checkable to the DBU.
#
# Deck (svrf-drc/examples/critarea.rule):
#   CA.M1.PASS { CRITAREA met1 RADIUS 1.5 > 13.0   }
#   CA.M1.FAIL { CRITAREA met1 RADIUS 1.5 > 12.999 }
#
# Fixture (gen_critarea_gds.py): two parallel rails L=10, width 2, gap s, at rho=1.5.
# KLayout's default mode-2 sizing grows each orthogonal rail to an EXACT rectangle, so
#     CA = (2*rho - s) * (L + 2*rho) = (3 - s) * 13   [um^2].
#
# Proves, against the fork's REAL native C++ engine and KLayout's OWN rdb reader:
#   1. base s=2.000 -> CA == 13.000000 exactly (SVRFDRC_CAAVAL), so CA.M1.PASS (>13.0)
#      PASSes at the limit and CA.M1.FAIL (>12.999) FAILs; the FAIL marker is the
#      shorts locus itself, bbox exactly [2.5,0.5,3.5,13.5].
#   2. HAND-COMPUTED value: the engine prints area=13.000000 = (3-2)*13.
#   3. BOUNDARY to the DBU: moving rail B one DBU closer (s=1.999) -> CA=13.013000 ->
#      CA.M1.PASS (>13.0) flips to FAIL. So s=2.000 PASS, s=1.999 FAIL.
#   4. VALUE TRACKS GEOMETRY: s=1.0 -> CA=26.000000 = (3-1)*13 = 2x the base -> FAIL.
#   5. PROVEN-NEGATIVE: s=4.0 -> 2*rho=3 < s -> the grown rails never meet -> CA=0.0
#      -> BOTH rules PASS, NITEMS 0. A defect of radius 1.5 cannot bridge a 4 um gap.
#   6. 0 regressions: the frozen engine goldens stay byte-identical.
#
# NO vendor data. Skips (exit 0) when the db build inputs or a container are absent.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
SVRF="$(cd "$HERE/.." && pwd)"
ROOT="$(cd "$SVRF/.." && pwd)"
DBP="$ROOT/src/plugins/tools/svrf_drc/db_plugin"
RULE="$SVRF/examples/critarea.rule"
KSRC="${KLAYOUT_SRC:-$HOME/kbuild}"
KBLD="${KLAYOUT_BLD:-$HOME/kbuild-out/bld}"
KBIN="${KLAYOUT_BIN:-$HOME/kbuild-out/bin}"
IMAGE="${EDA_IMAGE:-ghcr.io/vibeic/vibeic-eda:0.2.18}"

if [ ! -d "$KSRC/src/db/db" ] || [ ! -f "$KBIN/libklayout_db.so" ]; then
  echo "SKIP run_critarea_engine_test: no KLayout db build (set KLAYOUT_SRC/BLD/BIN)"; exit 0
fi
if ! command -v docker >/dev/null 2>&1; then
  echo "SKIP run_critarea_engine_test: no docker for GDS generation / rdb load"; exit 0
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
cp "$HERE/gen_critarea_gds.py" "$HERE/rve_load.py" "$WORK/"
docker run --rm --entrypoint bash -v "$WORK":/work "$IMAGE" -lc "
  export PATH=/foss/tools/klayout:\$PATH
  for m in base justin near far; do
    CA_MODE=\$m CA_OUT=/work/\$m.gds klayout -b -r /work/gen_critarea_gds.py >/dev/null 2>&1
  done
"
for g in base justin near far; do
  [ -s "$WORK/$g.gds" ] || fail "fixture $g generation produced no GDS"
done

#  run with the RVE marker DB and the CAAVAL area trace, capturing stderr to <mode>.err
run() { SVRFDRC_RVE_OUT="$WORK/$1.lyrdb" SVRFDRC_CAAVAL=1 \
        "$BIN" "$RULE" "$WORK/$1.gds" "$WORK/$1.rpt" "KLayout 0.30.9" 2>"$WORK/$1.err"; }
verdict() { awk -v n="$2" '$2==n {print $1}' "$WORK/$1.rpt"; }
caaval()  { awk -v n="$2" '$1=="CAAVAL" && $2==n {for(i=1;i<=NF;i++) if($i ~ /^area=/){sub("area=","",$i); print $i}}' "$WORK/$1.err"; }
load() { docker run --rm --entrypoint bash -v "$WORK":/work "$IMAGE" -lc "
  export PATH=/foss/tools/klayout:\$PATH
  LYRDB=/work/$1 klayout -b -r /work/rve_load.py 2>&1 | grep -E '^(CAT|BBOX|NITEMS|ERROR) '
"; }

for m in base justin near far; do run "$m"; done

echo "== 1. base s=2.000: CA == 13.0 -> PASS at the limit, FAIL one hair under =="
grep -E '^(PASS|FAIL|SKIP) +CA' "$WORK/base.rpt" | sed 's/^/   /'
grep -E '^CAAVAL' "$WORK/base.err" | sed 's/^/   /'
[ "$(verdict base CA.M1.PASS)" = "PASS" ] || fail "base CA 13.0 must PASS the >13.0 rule (not above the limit)"
[ "$(verdict base CA.M1.FAIL)" = "FAIL" ] || fail "base CA 13.0 must FAIL the >12.999 rule"
echo "  [1] 13.0 is exactly on the >13.0 limit (PASS) and above >12.999 (FAIL)"

echo "== 2. HAND-COMPUTED value = (2*1.5 - 2)*(10 + 2*1.5) = 13.000000 =="
[ "$(caaval base CA.M1.PASS)" = "13.000000" ] || { echo "got '$(caaval base CA.M1.PASS)'"; fail "base CA area != 13.000000"; }
echo "  [2] engine area == 13.000000 (closed form to the DBU)"

echo "== 3. HAND-COMPUTED marker geometry (KLayout's own rdb reader) =="
D="$(load base.lyrdb)"; echo "$D" | sed 's/^/   /'
echo "$D" | grep -qE '^ERROR'                          && { echo "$D"; fail "KLayout refused to load the .lyrdb"; }
echo "$D" | grep -qxE 'BBOX CA.M1.FAIL:2.5,0.5,3.5,13.5' || { echo "$D"; fail "marker != the shorts locus [2.5,0.5,3.5,13.5]"; }
echo "$D" | grep -qxE 'NITEMS 1'                        || { echo "$D"; fail "base NITEMS != 1 (CA.M1.PASS PASSes, only CA.M1.FAIL marks)"; }
echo "  [3] marker = grown railA right edge 3.5 meets grown railB left edge 2.5, y=[0.5,13.5]"

echo "== 4. BOUNDARY to the DBU: s=1.999 -> CA=13.013 -> the >13.0 rule FAILs =="
[ "$(verdict justin CA.M1.PASS)" = "FAIL" ] || fail "one DBU closer (s=1.999) must push CA over 13.0"
[ "$(caaval justin CA.M1.PASS)" = "13.013000" ] || { echo "got '$(caaval justin CA.M1.PASS)'"; fail "justin CA area != 13.013000"; }
echo "  [4] s=2.000 PASS, s=1.999 -> 13.013000 -> FAIL: the requirement is pinned at 13.0 to the DBU"

echo "== 5. VALUE TRACKS GEOMETRY: s=1.0 -> CA=26.0 = 2x base -> FAIL =="
[ "$(verdict near CA.M1.PASS)" = "FAIL" ] || fail "near s=1.0 must FAIL"
[ "$(caaval near CA.M1.PASS)" = "26.000000" ] || { echo "got '$(caaval near CA.M1.PASS)'"; fail "near CA area != 26.000000"; }
echo "  [5] halving the gap doubles the critical area (26.0 = (3-1)*13) -> read from geometry"

echo "== 6. PROVEN-NEGATIVE: s=4.0 (> 2*rho) -> CA=0 -> BOTH rules PASS, empty marker DB =="
[ "$(verdict far CA.M1.PASS)" = "PASS" ] || fail "far CA.M1.PASS not PASS"
[ "$(verdict far CA.M1.FAIL)" = "PASS" ] || fail "far CA.M1.FAIL not PASS (CA=0 is not > 12.999)"
[ "$(caaval far CA.M1.PASS)" = "0.000000" ] || { echo "got '$(caaval far CA.M1.PASS)'"; fail "far CA area != 0.000000"; }
F="$(load far.lyrdb)"
echo "$F" | grep -qxE 'NITEMS 0' || { echo "$F"; fail "far NITEMS != 0"; }
echo "  [6] a radius-1.5 defect cannot bridge a 4um gap -> CA=0, NITEMS=0 (not a constant-FAIL)"

echo "== 7. 0 regressions: frozen goldens byte-identical =="
EDA_IMAGE="$IMAGE" bash "$HERE/run_engine_parity.sh" >/dev/null 2>&1 || fail "engine parity regression FAILED"
echo "  [7] run_engine_parity.sh: all frozen goldens byte-identical"

echo "PASS run_critarea_engine_test (7/7)"
