#!/usr/bin/env bash
# run_cheese_test.sh -- UNFAKEABLE gate for DRC-safe cheesing / slotting (#39).
#
# metal_cheese.py cuts a DRC-safe hole grid into a wide metal shape so it meets a
# MAX-density rule. Proves, against the fork's OWN KLayout Region engine (the tool)
# AND the fork's native svrfdrc DRC engine (the oracle) -- no mocks:
#   1. FAIL->PASS: a solid 20x20 um square has design-extent density 1.0 (svrfdrc
#      DENSITY>0.70 FAILs); cheesing with hole=4um/wall=2um cuts a 3x3 grid of 4x4
#      slots (144 um^2) and the SAME rule PASSes;
#   2. HAND-COMPUTED value: the engine-measured density is bracketed to exactly
#      0.64 -- DENSITY>0.6401 PASSes AND DENSITY>0.6399 FAILs (=256/400, the
#      hand-computed remaining fraction) -- and the tool self-reports 0.64;
#   3. DRC-safe: on the cheesed GDS min-width (INTERNAL<2.0) and min-notch
#      (NOTCH<2.0) both PASS (walls exactly 2um, slots 4um);
#   4. PROVEN-NEGATIVE density: an under-aggressive spec (hole=2/wall=6 -> 1 slot,
#      density 0.99) leaves DENSITY>0.70 FAILing and the tool says reached=false;
#   5. PROVEN-NEGATIVE DRC-safety: a too-thin wall (wall=1um) makes the width rule
#      FAIL (34 violations) -- so [3] is a real pass, the wall parameter matters;
#   6. IDEMPOTENT: re-cheesing an already-in-budget layer changes nothing.
#
# NO vendor data. Skips (exit 0) when the db build inputs or a container are absent.
# Env: KLAYOUT_SRC/BLD/BIN + EDA_IMAGE (default vibeic/vibeic-eda:0.2.18)
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
FILL="$(cd "$HERE/.." && pwd)"
ROOT="$(cd "$FILL/.." && pwd)"
DBP="$ROOT/src/plugins/tools/svrf_drc/db_plugin"
RULE="$HERE/cheese.rule"
SVRFT="$ROOT/svrf-drc/tests"
KSRC="${KLAYOUT_SRC:-$HOME/kbuild}"
KBLD="${KLAYOUT_BLD:-$HOME/kbuild-out/bld}"
KBIN="${KLAYOUT_BIN:-$HOME/kbuild-out/bin}"
IMAGE="${EDA_IMAGE:-vibeic/vibeic-eda:0.2.18}"

if [ ! -d "$KSRC/src/db/db" ] || [ ! -f "$KBIN/libklayout_db.so" ]; then
  echo "SKIP run_cheese_test: no KLayout db build (set KLAYOUT_SRC/BLD/BIN)"; exit 0
fi
if ! command -v docker >/dev/null 2>&1; then
  echo "SKIP run_cheese_test: no docker for GDS generation / cheese tool"; exit 0
fi

WORK="$(mktemp -d)"; trap 'rm -rf "$WORK"' EXIT
export LD_LIBRARY_PATH="$KBIN:$KBIN/db_plugins:${LD_LIBRARY_PATH:-}"
fail() { echo "FAIL: $1"; exit 1; }

echo "== build engine_smoke (svrfdrc oracle) =="
BIN="$WORK/engine_smoke"
g++ -std=c++17 -O1 -DHAVE_PYTHON \
  -I"$DBP" -I"$KSRC/src/db/db" -I"$KBLD/db/db" -I"$KSRC/src/tl/tl" -I"$KBLD/tl/tl" -I"$KSRC/src/gsi/gsi" \
  "$SVRFT/engine_smoke.cc" "$DBP/dbSVRFDeck.cc" "$DBP/dbSVRFEngine.cc" \
  -L"$KBIN" -lklayout_db -lklayout_tl -lklayout_gsi \
  -Wl,--no-as-needed -L"$KBIN/db_plugins" -lgds2 -Wl,--as-needed \
  -Wl,-rpath,"$KBIN" -Wl,-rpath,"$KBIN/db_plugins" -o "$BIN" || fail "engine_smoke build failed"

echo "== generate fixture + run cheese tool (container klayout pya) =="
cp "$HERE/gen_cheese_gds.py" "$FILL/metal_cheese.py" "$WORK/"
cat > "$WORK/cfg_good.json" <<'J'
{ "layers": [ {"name":"met1","layer":[10,0],"max":0.70,"hole":4.0,"wall":2.0} ] }
J
cat > "$WORK/cfg_weak.json" <<'J'
{ "layers": [ {"name":"met1","layer":[10,0],"max":0.70,"hole":2.0,"wall":6.0} ] }
J
cat > "$WORK/cfg_thin.json" <<'J'
{ "layers": [ {"name":"met1","layer":[10,0],"max":0.70,"hole":4.0,"wall":1.0} ] }
J
docker run --rm --entrypoint bash -v "$WORK":/work "$IMAGE" -lc "
  export PATH=/foss/tools/klayout:\$PATH
  CHEESE_FIX_OUT=/work/solid.gds klayout -b -r /work/gen_cheese_gds.py >/dev/null 2>&1
  CHEESE_GDS=/work/solid.gds  CHEESE_CONFIG=/work/cfg_good.json CHEESE_OUT=/work/cheesed.gds CHEESE_REPORT=/work/rep_good.json klayout -b -r /work/metal_cheese.py >/dev/null 2>&1
  CHEESE_GDS=/work/solid.gds  CHEESE_CONFIG=/work/cfg_weak.json CHEESE_OUT=/work/weak.gds    CHEESE_REPORT=/work/rep_weak.json klayout -b -r /work/metal_cheese.py >/dev/null 2>&1
  CHEESE_GDS=/work/solid.gds  CHEESE_CONFIG=/work/cfg_thin.json CHEESE_OUT=/work/thin.gds    CHEESE_REPORT=/work/rep_thin.json klayout -b -r /work/metal_cheese.py >/dev/null 2>&1
  CHEESE_GDS=/work/cheesed.gds CHEESE_CONFIG=/work/cfg_good.json CHEESE_OUT=/work/again.gds  CHEESE_REPORT=/work/rep_again.json klayout -b -r /work/metal_cheese.py >/dev/null 2>&1
"
for g in solid cheesed weak thin again; do [ -s "$WORK/$g.gds" ] || fail "fixture $g not produced"; done

drc() { "$BIN" "$RULE" "$WORK/$1.gds" "$WORK/$1.rpt" "KLayout 0.30.9" 2>/dev/null;
        grep -E "^(FAIL|PASS)  $2 " "$WORK/$1.rpt"; }
verdict() { grep -E "^(FAIL|PASS)  $2 " "$WORK/$1.rpt" | awk '{print $1}'; }
jget() { python3 -c "import json,sys;print(json.load(open('$1'))['layers'][0]['$2'])"; }

echo "== 1. FAIL->PASS: DENSITY>0.70 solid FAIL, cheesed PASS =="
"$BIN" "$RULE" "$WORK/solid.gds"   "$WORK/solid.rpt"   "KLayout 0.30.9" 2>/dev/null
"$BIN" "$RULE" "$WORK/cheesed.gds" "$WORK/cheesed.rpt" "KLayout 0.30.9" 2>/dev/null
[ "$(verdict solid DENS_MAX)" = FAIL ]   || { grep DENS_MAX "$WORK/solid.rpt"; fail "solid DENSITY>0.70 should FAIL"; }
[ "$(verdict cheesed DENS_MAX)" = PASS ] || { grep DENS_MAX "$WORK/cheesed.rpt"; fail "cheesed DENSITY>0.70 should PASS"; }
echo "  [1] DENS_MAX solid=FAIL cheesed=PASS"

echo "== 2. HAND-COMPUTED: engine density bracketed to 0.64 + tool reports 0.64 =="
[ "$(verdict cheesed DENS_HI)" = PASS ] || fail "cheesed DENSITY>0.6401 should PASS (density<=0.6401)"
[ "$(verdict cheesed DENS_LO)" = FAIL ] || fail "cheesed DENSITY>0.6399 should FAIL (density>0.6399)"
DA="$(jget "$WORK/rep_good.json" density_after)"
[ "$DA" = "0.64" ] || fail "tool density_after=$DA != hand-computed 0.64"
[ "$(jget "$WORK/rep_good.json" holes)" = "9" ] || fail "expected a 3x3=9 slot grid"
echo "  [2] engine density in (0.6399,0.6401]=0.64; tool density_after=0.64 holes=9 (256/400)"

echo "== 3. DRC-safe: min-width + min-notch PASS on cheesed =="
[ "$(verdict cheesed W_MIN)" = PASS ] || { grep W_MIN "$WORK/cheesed.rpt"; fail "cheesed INTERNAL<2.0 (min-width) should PASS"; }
[ "$(verdict cheesed N_MIN)" = PASS ] || { grep N_MIN "$WORK/cheesed.rpt"; fail "cheesed NOTCH<2.0 (min-notch) should PASS"; }
echo "  [3] W_MIN=PASS N_MIN=PASS (walls 2um, slots 4um)"

echo "== 4. PROVEN-NEGATIVE density: under-aggressive spec stays over budget =="
"$BIN" "$RULE" "$WORK/weak.gds" "$WORK/weak.rpt" "KLayout 0.30.9" 2>/dev/null
[ "$(verdict weak DENS_MAX)" = FAIL ] || fail "weak cheese should leave DENSITY>0.70 FAILing"
[ "$(jget "$WORK/rep_weak.json" reached)" = "False" ] || fail "weak cheese must report reached=false"
echo "  [4] weak: DENS_MAX=FAIL, tool reached=False, density_after=$(jget "$WORK/rep_weak.json" density_after)"

echo "== 5. PROVEN-NEGATIVE DRC-safety: thin wall trips the width rule =="
"$BIN" "$RULE" "$WORK/thin.gds" "$WORK/thin.rpt" "KLayout 0.30.9" 2>/dev/null
[ "$(verdict thin W_MIN)" = FAIL ] || fail "thin (wall=1um) must FAIL INTERNAL<2.0 (safety not vacuous)"
echo "  [5] thin: W_MIN=FAIL ($(grep -E '^FAIL  W_MIN' "$WORK/thin.rpt" | grep -oE '> [0-9]+$' || echo 'violations>0')) -> wall parameter is load-bearing"

echo "== 6. IDEMPOTENT: re-cheesing an in-budget layer changes nothing =="
[ "$(jget "$WORK/rep_again.json" changed)" = "False" ] || fail "re-cheese of in-budget layer must be a no-op"
[ "$(jget "$WORK/rep_again.json" holes)" = "0" ] || fail "re-cheese must add 0 holes"
echo "  [6] re-cheese changed=False holes=0"

echo "== 7. 0 regressions: frozen engine goldens byte-identical =="
EDA_IMAGE="$IMAGE" bash "$SVRFT/run_engine_parity.sh" >/dev/null 2>&1 || fail "engine parity regression FAILED"
echo "  [7] run_engine_parity.sh: all frozen goldens byte-identical"

echo "PASS run_cheese_test (7/7)"
