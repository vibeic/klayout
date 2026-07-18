#!/usr/bin/env bash
# run_vspace_engine_test.sh -- UNFAKEABLE gate for voltage-aware / net-voltage-
# dependent spacing (fork feature #12): the spacing REQUIRED between two DIFFERENT
# nets grows with the voltage difference across the gap, with each net's voltage
# read from GEOMETRY (VOLTAGE marker layers) via db::LayoutToNetlist.
#
# Deck (svrf-drc/examples/vspace.rule):
#   VOLTAGE vdd18 1.8 ; VOLTAGE vdd50 5.0
#   V.SP.M1    { VSPACE met1 < 0.20 PER_VOLT 0.05 }
#   V.SP.M1.K0 { VSPACE met1 < 0.20 PER_VOLT 0    }   <- control
#   V.SP.NOK   { VSPACE met1 < 0.20               }   <- no voltage term
#
# Fixture (gen_vspace_gds.py): three independent met1 nets --
#   A [0,0..2,1] @1.8V, B [bx,0..bx+2,1] @5.0V, C [0,1.30..2,2.30] @1.8V.
# HAND-COMPUTED requirements: A-C |dV|=0.0 -> 0.20 um ; A-B |dV|=3.2 -> 0.20 +
# 0.05*3.2 = 0.36 um. The A-C gap is 0.30 um in EVERY mode, so the same 0.30 um
# that is legal inside one domain is illegal across the two -- a result NO single
# fixed-distance EXTERNAL rule can produce.
#
# Proves, against the fork's REAL native C++ engine and KLayout's OWN rdb reader:
#   1. viol (A-B gap 0.300 < 0.360) -> V.SP.M1 FAIL 1, and it is the A-B pair:
#      the identical 0.30 um A-C gap inside one domain is NOT flagged.
#   2. HAND-COMPUTED GEOMETRY: the emitted .lyrdb marker spans exactly the failing
#      gap, bbox [2,0,2.3,1] (A's right edge x=2 to B's left edge x=2.3).
#   3. BOUNDARY, to the DBU: gap 0.360 == the requirement -> PASS (not below it);
#      gap 0.359, ONE DBU inside, -> FAIL 1, marker bbox [2,0,2.359,1]. The
#      requirement is therefore exactly 0.360 um, i.e. 0.20 + 0.05*3.2.
#   4. PROVEN-NEGATIVE (clean): gap 0.500 -> PASS 0.
#   5. THE VOLTAGE TERM IS REAL (control): V.SP.M1.K0, the same rule with
#      PER_VOLT 0, PASSES in EVERY mode -- 0.300 and 0.359 both clear the 0.20 um
#      base. So every failure above is attributable to the voltage term alone and
#      cannot come from the geometry.
#   6. THE VOLTAGE IS READ FROM GEOMETRY: with the met1 geometry and the layer
#      inventory UNCHANGED, moving the vdd50 marker into empty space so it touches
#      no net drops B to the unmarked 0 V default -> requirement 0.20+0.05*1.8 =
#      0.29 um -> the same 0.300 um gap becomes legal -> PASS 0. A hardcoded
#      voltage could not follow the marker.
#   7. honest-SKIP: a VSPACE with no PER_VOLT modifier SKIPs, never PASSes.
#   8. 0 regressions: the frozen engine goldens stay byte-identical.
#
# NO vendor data. Skips (exit 0) when the db build inputs or a container are absent.
# Env: KLAYOUT_SRC/BLD/BIN + EDA_IMAGE (default vibeic/vibeic-eda:0.2.18)
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
SVRF="$(cd "$HERE/.." && pwd)"
ROOT="$(cd "$SVRF/.." && pwd)"
DBP="$ROOT/src/plugins/tools/svrf_drc/db_plugin"
RULE="$SVRF/examples/vspace.rule"
KSRC="${KLAYOUT_SRC:-$HOME/kbuild}"
KBLD="${KLAYOUT_BLD:-$HOME/kbuild-out/bld}"
KBIN="${KLAYOUT_BIN:-$HOME/kbuild-out/bin}"
IMAGE="${EDA_IMAGE:-vibeic/vibeic-eda:0.2.18}"

if [ ! -d "$KSRC/src/db/db" ] || [ ! -f "$KBIN/libklayout_db.so" ]; then
  echo "SKIP run_vspace_engine_test: no KLayout db build (set KLAYOUT_SRC/BLD/BIN)"; exit 0
fi
if ! command -v docker >/dev/null 2>&1; then
  echo "SKIP run_vspace_engine_test: no docker for GDS generation / rdb load"; exit 0
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
cp "$HERE/gen_vspace_gds.py" "$HERE/rve_load.py" "$WORK/"
docker run --rm --entrypoint bash -v "$WORK":/work "$IMAGE" -lc "
  export PATH=/foss/tools/klayout:\$PATH
  for m in viol edge justin clean nomark; do
    VSP_MODE=\$m VSP_OUT=/work/\$m.gds klayout -b -r /work/gen_vspace_gds.py >/dev/null 2>&1
  done
"
for g in viol edge justin clean nomark; do
  [ -s "$WORK/$g.gds" ] || fail "fixture $g generation produced no GDS"
done

run() { SVRFDRC_RVE_OUT="${2:-}" "$BIN" "$RULE" "$WORK/$1.gds" "$WORK/$1.rpt" "KLayout 0.30.9" 2>/dev/null; }
verdict() { awk -v n="$2" '$2==n {print $1}' "$WORK/$1.rpt"; }
count()   { awk -v n="$2" '$2==n {print $NF}' "$WORK/$1.rpt"; }
load() { docker run --rm --entrypoint bash -v "$WORK":/work "$IMAGE" -lc "
  export PATH=/foss/tools/klayout:\$PATH
  LYRDB=/work/$1 klayout -b -r /work/rve_load.py 2>&1 | grep -E '^(CAT|BBOX|NITEMS|ERROR) '
"; }

for m in viol edge justin clean nomark; do run "$m" "$WORK/$m.lyrdb"; done

echo "== 1. viol: 0.300 um gap FAILs ACROSS domains but the same gap inside one is legal =="
grep -E '^(PASS|FAIL|SKIP) +V\.SP' "$WORK/viol.rpt" | sed 's/^/   /'
[ "$(verdict viol V.SP.M1)" = "FAIL" ] || fail "viol V.SP.M1 not FAIL"
[ "$(count   viol V.SP.M1)" = "1" ]    || fail "viol V.SP.M1 count != 1 (the same-domain A-C 0.30 um gap must NOT be flagged)"
echo "  [1] exactly 1 violation -- the 1.8V/5.0V pair, not the 1.8V/1.8V pair at the same 0.30 um"

echo "== 2. HAND-COMPUTED marker geometry (KLayout's own rdb reader) =="
D="$(load viol.lyrdb)"; echo "$D" | sed 's/^/   /'
echo "$D" | grep -qE '^ERROR'                  && { echo "$D"; fail "KLayout refused to load the .lyrdb"; }
echo "$D" | grep -qxE 'BBOX V.SP.M1:2,0,2.3,1' || { echo "$D"; fail "marker != the failing gap [2,0,2.3,1]"; }
echo "$D" | grep -qxE 'NITEMS 1'               || { echo "$D"; fail "viol NITEMS != 1"; }
echo "  [2] marker spans exactly A's right edge x=2 to B's left edge x=2.3"

echo "== 3. BOUNDARY to the DBU: 0.360 PASSes, 0.359 (1 DBU inside) FAILs =="
[ "$(verdict edge V.SP.M1)"   = "PASS" ] || fail "gap == the 0.36 um requirement must PASS (check is 'below', not 'at or below')"
[ "$(verdict justin V.SP.M1)" = "FAIL" ] || fail "gap 1 DBU inside the 0.36 um requirement must FAIL"
[ "$(count   justin V.SP.M1)" = "1" ]    || fail "justin count != 1"
J="$(load justin.lyrdb)"
echo "$J" | grep -qxE 'BBOX V.SP.M1:2,0,2.359,1' || { echo "$J"; fail "justin marker != [2,0,2.359,1] (coords not read from geometry)"; }
echo "  [3] 0.360 -> PASS, 0.359 -> FAIL @ [2,0,2.359,1]: requirement is exactly 0.20+0.05*3.2 = 0.360 um"

echo "== 4. PROVEN-NEGATIVE: 0.500 um gap -> PASS, empty marker DB =="
[ "$(verdict clean V.SP.M1)" = "PASS" ] || fail "clean V.SP.M1 not PASS"
C="$(load clean.lyrdb)"
echo "$C" | grep -qE 'CAT '     && { echo "$C"; fail "clean layout must yield NO categories"; }
echo "$C" | grep -qxE 'NITEMS 0' || { echo "$C"; fail "clean NITEMS != 0"; }
echo "  [4] clean: PASS, 0 categories, NITEMS=0 (not a constant-FAIL)"

echo "== 5. CONTROL: PER_VOLT 0 PASSes in every mode -> the failure is the voltage term =="
for m in viol edge justin clean nomark; do
  [ "$(verdict $m V.SP.M1.K0)" = "PASS" ] \
    || fail "$m: PER_VOLT 0 control FAILED -- the geometry alone violates the 0.20 um base, so the test cannot attribute the failure to voltage"
done
echo "  [5] V.SP.M1.K0 PASS 0 on all 5 fixtures: 0.300/0.359 clear the 0.20 um base"

echo "== 6. the voltage is READ FROM GEOMETRY: move the marker, flip the verdict =="
#  identical met1 geometry + identical layer inventory as `viol`; only the vdd50
#  marker moved into empty space -> B is unmarked (0 V) -> requirement 0.29 um.
cmp -s "$WORK/viol.gds" "$WORK/nomark.gds" && fail "nomark fixture is identical to viol -- the marker did not move"
[ "$(verdict nomark V.SP.M1)" = "PASS" ] \
  || fail "moving the vdd50 marker off net B did not relax the requirement -> the voltage is NOT read from the marker geometry"
echo "  [6] marker off-net -> B=0V -> requirement 0.29 um -> the same 0.300 um gap PASSes"

echo "== 7. honest-SKIP: VSPACE without PER_VOLT =="
[ "$(verdict viol V.SP.NOK)" = "SKIP" ] || fail "VSPACE without PER_VOLT must SKIP, never PASS"
echo "  [7] no voltage term -> SKIP (never a false PASS)"

echo "== 8. 0 regressions: frozen goldens byte-identical =="
EDA_IMAGE="$IMAGE" bash "$HERE/run_engine_parity.sh" >/dev/null 2>&1 || fail "engine parity regression FAILED"
echo "  [8] run_engine_parity.sh: all frozen goldens byte-identical"

echo "PASS run_vspace_engine_test (8/8)"
