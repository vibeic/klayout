#!/usr/bin/env bash
# run_densgrad_engine_test.sh -- UNFAKEABLE gate for the CMP density-GRADIENT check
# (fork feature #49): the ABSOLUTE metal-density DIFFERENCE between edge-adjacent
# WINDOW/STEP tiles (a planarity / CMP-dishing proxy), NOT the per-window density.
#
# Deck (svrf-drc/examples/densgrad.rule):
#   DG.M1        { DENSITY met1 WINDOW 10 STEP 10 GRADIENT > 0.50 }
#   DG.M1.NOGRAD { DENSITY met1 WINDOW 10 STEP 10          > 0.50 }   <- plain density
#
# Fixture (gen_densgrad_gds.py): a fixed frame 100/0 rectangle [0,0..20,10] pins the
# extent so WINDOW 10/STEP 10 always yields exactly two tiles T0=[0,0..10,10],
# T1=[10,0..20,10]; the modes differ ONLY in the met1 fill. A met1 fill [0,0..10,H]
# makes density(T0)=H/10, density(T1)=0, so the gradient is EXACTLY H/10.
#
# Proves, against the fork's REAL native C++ engine and KLayout's OWN rdb reader:
#   1. viol H=6.0 -> gradient 0.60 > 0.50 -> DG.M1 FAIL 1; marker = T0+T1 merged.
#   2. HAND-COMPUTED marker geometry: the .lyrdb marker bbox is exactly [0,0,20,10].
#   3. BOUNDARY to the DBU: H=5.0 -> 0.50 == limit -> PASS; H=5.001 (1 DBU) -> FAIL.
#   4. PROVEN-NEGATIVE: no met1 -> gradient 0 -> PASS 0, NITEMS 0.
#   5. DISTINGUISHER (gradient is NOT a relabelled density): a UNIFORM 0.60 fill makes
#      BOTH tiles 0.60 -> gradient 0 -> DG.M1 PASS, while the PLAIN DG.M1.NOGRAD
#      density>0.50 FAILs on the identical geometry. No fixed per-window density rule
#      can produce a PASS here; only a window-to-window DIFFERENCE can.
#   6. 0 regressions: the frozen engine goldens stay byte-identical.
#
# NO vendor data. Skips (exit 0) when the db build inputs or a container are absent.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
SVRF="$(cd "$HERE/.." && pwd)"
ROOT="$(cd "$SVRF/.." && pwd)"
DBP="$ROOT/src/plugins/tools/svrf_drc/db_plugin"
RULE="$SVRF/examples/densgrad.rule"
KSRC="${KLAYOUT_SRC:-$HOME/kbuild}"
KBLD="${KLAYOUT_BLD:-$HOME/kbuild-out/bld}"
KBIN="${KLAYOUT_BIN:-$HOME/kbuild-out/bin}"
IMAGE="${EDA_IMAGE:-ghcr.io/vibeic/vibeic-eda:0.2.18}"

if [ ! -d "$KSRC/src/db/db" ] || [ ! -f "$KBIN/libklayout_db.so" ]; then
  echo "SKIP run_densgrad_engine_test: no KLayout db build (set KLAYOUT_SRC/BLD/BIN)"; exit 0
fi
if ! command -v docker >/dev/null 2>&1; then
  echo "SKIP run_densgrad_engine_test: no docker for GDS generation / rdb load"; exit 0
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
cp "$HERE/gen_densgrad_gds.py" "$HERE/rve_load.py" "$WORK/"
docker run --rm --entrypoint bash -v "$WORK":/work "$IMAGE" -lc "
  export PATH=/foss/tools/klayout:\$PATH
  for m in viol edge justin uniform empty; do
    DG_MODE=\$m DG_OUT=/work/\$m.gds klayout -b -r /work/gen_densgrad_gds.py >/dev/null 2>&1
  done
"
for g in viol edge justin uniform empty; do
  [ -s "$WORK/$g.gds" ] || fail "fixture $g generation produced no GDS"
done

run() { SVRFDRC_RVE_OUT="${2:-}" "$BIN" "$RULE" "$WORK/$1.gds" "$WORK/$1.rpt" "KLayout 0.30.9" 2>/dev/null; }
verdict() { awk -v n="$2" '$2==n {print $1}' "$WORK/$1.rpt"; }
count()   { awk -v n="$2" '$2==n {print $NF}' "$WORK/$1.rpt"; }
load() { docker run --rm --entrypoint bash -v "$WORK":/work "$IMAGE" -lc "
  export PATH=/foss/tools/klayout:\$PATH
  LYRDB=/work/$1 klayout -b -r /work/rve_load.py 2>&1 | grep -E '^(CAT|BBOX|NITEMS|ERROR) '
"; }

for m in viol edge justin uniform empty; do run "$m" "$WORK/$m.lyrdb"; done

echo "== 1. viol: density STEP 0.60 across the two tiles -> FAIL =="
grep -E '^(PASS|FAIL|SKIP) +DG' "$WORK/viol.rpt" | sed 's/^/   /'
[ "$(verdict viol DG.M1)" = "FAIL" ] || fail "viol DG.M1 not FAIL"
[ "$(count   viol DG.M1)" = "1" ]    || fail "viol DG.M1 count != 1"
echo "  [1] gradient 0.60 > 0.50 -> FAIL 1"

echo "== 2. HAND-COMPUTED marker geometry (KLayout's own rdb reader) =="
#  the DG.M1 GRADIENT marker is the two merged tiles; the co-resident plain-density
#  control DG.M1.NOGRAD also FAILs on viol and marks its own T0, so we assert on the
#  DG.M1 category specifically rather than the global item count.
D="$(load viol.lyrdb)"; echo "$D" | sed 's/^/   /'
echo "$D" | grep -qE '^ERROR'                     && { echo "$D"; fail "KLayout refused to load the .lyrdb"; }
echo "$D" | grep -qxE 'BBOX DG.M1:0,0,20,10'      || { echo "$D"; fail "GRADIENT marker != the two merged tiles [0,0,20,10]"; }
[ "$(echo "$D" | grep -cE '^BBOX DG\.M1:')" = "1" ] || { echo "$D"; fail "DG.M1 must mark exactly one merged cluster"; }
echo "  [2] GRADIENT marker spans exactly the two adjacent tiles T0+T1 = [0,0,20,10] (one cluster)"

echo "== 3. BOUNDARY to the DBU: 0.500 PASSes, 0.5001 (1 DBU) FAILs =="
[ "$(verdict edge   DG.M1)" = "PASS" ] || fail "gradient == 0.50 must PASS (check is 'above', not 'at or above')"
[ "$(verdict justin DG.M1)" = "FAIL" ] || fail "gradient 1 DBU over 0.50 must FAIL"
[ "$(count   justin DG.M1)" = "1" ]    || fail "justin count != 1"
J="$(load justin.lyrdb)"
echo "$J" | grep -qxE 'BBOX DG.M1:0,0,20,10'      || { echo "$J"; fail "justin GRADIENT marker != [0,0,20,10]"; }
[ "$(echo "$J" | grep -cE '^BBOX DG\.M1:')" = "1" ] || { echo "$J"; fail "justin DG.M1 must mark exactly one cluster"; }
echo "  [3] H=5.000 -> PASS, H=5.001 -> FAIL: the limit is exactly a 0.50 density step"

echo "== 4. PROVEN-NEGATIVE: no metal -> gradient 0 -> PASS, empty marker DB =="
[ "$(verdict empty DG.M1)" = "PASS" ] || fail "empty DG.M1 not PASS"
C="$(load empty.lyrdb)"
echo "$C" | grep -qxE 'NITEMS 0' || { echo "$C"; fail "empty NITEMS != 0"; }
echo "  [4] empty: PASS, NITEMS=0 (not a constant-FAIL)"

echo "== 5. DISTINGUISHER: uniform 0.60 fill -> GRADIENT PASS but PLAIN DENSITY FAIL =="
grep -E '^(PASS|FAIL|SKIP) +DG' "$WORK/uniform.rpt" | sed 's/^/   /'
[ "$(verdict uniform DG.M1)" = "PASS" ] \
  || fail "a UNIFORM density has zero gradient -> DG.M1 must PASS"
[ "$(verdict uniform DG.M1.NOGRAD)" = "FAIL" ] \
  || fail "the plain per-window density>0.50 must FAIL on the 0.60 uniform fill (the distinguisher)"
echo "  [5] identical geometry: window-to-window DIFFERENCE=0 PASSes while per-window density=0.60 FAILs"
echo "      -> GRADIENT is a genuinely different check, not a relabelled DENSITY"

echo "== 6. 0 regressions: frozen goldens byte-identical =="
EDA_IMAGE="$IMAGE" bash "$HERE/run_engine_parity.sh" >/dev/null 2>&1 || fail "engine parity regression FAILED"
echo "  [6] run_engine_parity.sh: all frozen goldens byte-identical"

echo "PASS run_densgrad_engine_test (6/6)"
