#!/usr/bin/env bash
# run_mask_engine_test.sh -- UNFAKEABLE gate for multi-patterning colorability (fork
# feature #25): a same-mask CONFLICT graph (edge = two shapes closer than the rule
# spacing) is decomposable into two masks iff it is 2-colourable (bipartite); a
# component with an ODD conflict cycle is undecomposable and every shape in it is
# reported. This is REAL graph 2-colouring, not "any near pair fails".
#
# Deck (svrf-drc/examples/mask.rule):  MP.DECOMP { MASK mp SPACING 0.30 }
#
# Fixture (gen_mask_gds.py): every conflict is a FACING-EDGE orthogonal gap of 0.25 um
# (< 0.30, unambiguous for the Euclidean space check); every non-edge is >= 0.30 apart.
#
# Proves, against the fork's REAL native C++ engine and KLayout's OWN rdb reader:
#   1. tri (A,B,C mutually conflicting = a TRIANGLE / odd cycle) -> FAIL 3, and the
#      three markers are exactly A=[0,0,2,0.5], B=[0,0.75,0.5,2], C=[0.75,0.75,2,1.3].
#   2. quad (a 4-CYCLE ring P-Q-R-S with all four ring edges conflicting but the two
#      diagonals 0.354 um > 0.30 apart) -> PASS 0. THE KEY PROOF: an even cycle IS
#      2-colourable, so four real conflicts still decompose -- a check that merely
#      flagged any near pair would wrongly FAIL this.
#   3. BOUNDARY to the DBU: widening ONLY the B-C gap to exactly 0.30 drops that one
#      edge, leaving a path A-B, A-C -> 2-colourable -> PASS; one DBU inside (0.299)
#      restores the triangle -> FAIL 3.
#   4. PROVEN-NEGATIVE: three shapes all > 0.30 apart -> 0 conflicts -> PASS 0, NITEMS 0.
#   5. honest-SKIP: a single shape -> nothing to decompose -> SKIP (never a vacuous PASS).
#   6. 0 regressions: the frozen engine goldens stay byte-identical.
#
# NO vendor data. Skips (exit 0) when the db build inputs or a container are absent.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
SVRF="$(cd "$HERE/.." && pwd)"
ROOT="$(cd "$SVRF/.." && pwd)"
DBP="$ROOT/src/plugins/tools/svrf_drc/db_plugin"
RULE="$SVRF/examples/mask.rule"
KSRC="${KLAYOUT_SRC:-$HOME/kbuild}"
KBLD="${KLAYOUT_BLD:-$HOME/kbuild-out/bld}"
KBIN="${KLAYOUT_BIN:-$HOME/kbuild-out/bin}"
IMAGE="${EDA_IMAGE:-ghcr.io/vibeic/vibeic-eda:0.2.18}"

if [ ! -d "$KSRC/src/db/db" ] || [ ! -f "$KBIN/libklayout_db.so" ]; then
  echo "SKIP run_mask_engine_test: no KLayout db build (set KLAYOUT_SRC/BLD/BIN)"; exit 0
fi
if ! command -v docker >/dev/null 2>&1; then
  echo "SKIP run_mask_engine_test: no docker for GDS generation / rdb load"; exit 0
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
cp "$HERE/gen_mask_gds.py" "$HERE/rve_load.py" "$WORK/"
docker run --rm --entrypoint bash -v "$WORK":/work "$IMAGE" -lc "
  export PATH=/foss/tools/klayout:\$PATH
  for m in tri edgeBC justBC quad clean single; do
    MP_MODE=\$m MP_OUT=/work/\$m.gds klayout -b -r /work/gen_mask_gds.py >/dev/null 2>&1
  done
"
for g in tri edgeBC justBC quad clean single; do
  [ -s "$WORK/$g.gds" ] || fail "fixture $g generation produced no GDS"
done

run() { SVRFDRC_RVE_OUT="${2:-}" "$BIN" "$RULE" "$WORK/$1.gds" "$WORK/$1.rpt" "KLayout 0.30.9" 2>/dev/null; }
verdict() { awk -v n="$2" '$2==n {print $1}' "$WORK/$1.rpt"; }
count()   { awk -v n="$2" '$2==n {print $NF}' "$WORK/$1.rpt"; }
load() { docker run --rm --entrypoint bash -v "$WORK":/work "$IMAGE" -lc "
  export PATH=/foss/tools/klayout:\$PATH
  LYRDB=/work/$1 klayout -b -r /work/rve_load.py 2>&1 | grep -E '^(CAT|BBOX|NITEMS|ERROR) '
"; }

for m in tri edgeBC justBC quad clean single; do run "$m" "$WORK/$m.lyrdb"; done

echo "== 1. tri: a TRIANGLE of mutual conflicts (odd cycle) -> undecomposable -> FAIL 3 =="
grep -E '^(PASS|FAIL|SKIP) +MP' "$WORK/tri.rpt" | sed 's/^/   /'
[ "$(verdict tri MP.DECOMP)" = "FAIL" ] || fail "tri MP.DECOMP not FAIL"
[ "$(count   tri MP.DECOMP)" = "3" ]    || fail "tri MP.DECOMP count != 3 (all three odd-cycle shapes)"
D="$(load tri.lyrdb)"; echo "$D" | sed 's/^/   /'
echo "$D" | grep -qE '^ERROR'                        && { echo "$D"; fail "KLayout refused to load the .lyrdb"; }
echo "$D" | grep -qxE 'BBOX MP.DECOMP:0,0,2,0.5'     || { echo "$D"; fail "missing marker A [0,0,2,0.5]"; }
echo "$D" | grep -qxE 'BBOX MP.DECOMP:0,0.75,0.5,2'  || { echo "$D"; fail "missing marker B [0,0.75,0.5,2]"; }
echo "$D" | grep -qxE 'BBOX MP.DECOMP:0.75,0.75,2,1.3' || { echo "$D"; fail "missing marker C [0.75,0.75,2,1.3]"; }
echo "$D" | grep -qxE 'NITEMS 3'                     || { echo "$D"; fail "tri NITEMS != 3"; }
echo "  [1] odd cycle -> all 3 shapes reported at their exact bboxes"

echo "== 2. quad: a 4-CYCLE ring (even) with FOUR real conflicts -> 2-colourable -> PASS 0 =="
grep -E '^(PASS|FAIL|SKIP) +MP' "$WORK/quad.rpt" | sed 's/^/   /'
[ "$(verdict quad MP.DECOMP)" = "PASS" ] \
  || fail "an even 4-cycle IS 2-colourable and must PASS -- a 'any near pair fails' check would wrongly FAIL"
Q="$(load quad.lyrdb)"
echo "$Q" | grep -qxE 'NITEMS 0' || { echo "$Q"; fail "quad NITEMS != 0"; }
echo "  [2] four ring conflicts, diagonals 0.354um>0.30 -> bipartite -> PASS (proves genuine 2-colouring)"

echo "== 3. BOUNDARY to the DBU: B-C gap 0.30 drops the edge -> PASS; 0.299 -> FAIL 3 =="
[ "$(verdict edgeBC MP.DECOMP)" = "PASS" ] || fail "B-C gap == 0.30 must drop the conflict edge -> path -> PASS"
[ "$(verdict justBC MP.DECOMP)" = "FAIL" ] || fail "B-C gap 0.299 (1 DBU inside) must restore the triangle -> FAIL"
[ "$(count   justBC MP.DECOMP)" = "3" ]    || fail "justBC count != 3"
echo "  [3] one DBU in the B-C gap flips the whole component decomposable<->undecomposable"

echo "== 4. PROVEN-NEGATIVE: three shapes all > 0.30 apart -> PASS, empty marker DB =="
[ "$(verdict clean MP.DECOMP)" = "PASS" ] || fail "clean MP.DECOMP not PASS"
C="$(load clean.lyrdb)"
echo "$C" | grep -qxE 'NITEMS 0' || { echo "$C"; fail "clean NITEMS != 0"; }
echo "  [4] no conflicts -> PASS, NITEMS=0 (not a constant-FAIL)"

echo "== 5. honest-SKIP: a single shape -> nothing to decompose =="
[ "$(verdict single MP.DECOMP)" = "SKIP" ] || fail "one shape must SKIP, never a vacuous PASS"
echo "  [5] fewer than two shapes -> SKIP"

echo "== 6. 0 regressions: frozen goldens byte-identical =="
EDA_IMAGE="$IMAGE" bash "$HERE/run_engine_parity.sh" >/dev/null 2>&1 || fail "engine parity regression FAILED"
echo "  [6] run_engine_parity.sh: all frozen goldens byte-identical"

echo "PASS run_mask_engine_test (6/6)"
