#!/usr/bin/env bash
# run_caa_test.sh -- UNFAKEABLE gate for Critical Area Analysis (#46): the
# short-critical-area GEOMETRY (grow both conductors by the defect radius and
# intersect) plus the weighted integral against a defect-density distribution.
#
# Fixture (gen_caa_gds.py): two parallel rails on layers 10/0 and 11/0, width
# 1 um, length 20 um (x from -5..15), facing gap s, and an INTERIOR measurement
# window x in [0,10] (W = 10 um) so the facing-edge overlap is measured without
# end/corner effects. HAND-COMPUTED, exact:
#     Ac_short(x) = W * (x - s)   for defect diameter x > s, else 0
#     AWC_short   = W * D0 * x0^2 / s   for D(x) = D0 * 2 x0^2 / x^3  (x0 <= s)
#
# Proves, against the fork's tool on KLayout's OWN Region engine:
#   1. PER-DIAMETER GEOMETRY, exact to the DBU (s=0.5 um, W=10 um):
#        Ac(0.5) = 0        (defect diameter == spacing, no overlap)
#        Ac(1.0) = 5.0 um^2 (= 10 * (1.0 - 0.5))
#        Ac(2.0) = 15.0 um^2(= 10 * (2.0 - 0.5))
#      These are areas of the actual grown-region intersection polygon.
#   2. WEIGHTED AWC matches the closed form: 10 * 1 * 0.1^2 / 0.5 = 0.2 um^2.
#   3. SPACING-DERIVED (not constant): doubling s to 1.0 um HALVES AWC to 0.1,
#      and pushes Ac(1.0) to 0 (now 2r = s) -- the numbers track the geometry.
#   4. WINDOW-DERIVED: doubling the window length W to 20 um doubles Ac(1.0) to
#      10.0 and AWC to 0.4 -- the critical area scales with the facing length.
#   5. DEFECT-DENSITY IS A PARAMETER: doubling D0 exactly doubles AWC (the [EXT]
#      half is linear and separable from the geometry).
#   6. PROVEN-NEGATIVE: rails 5 um apart -> for every defect up to x_max=4 um,
#      2r < s -> Ac = 0 at every diameter -> AWC = 0 (not a constant-nonzero).
#
# NO vendor data. Skips (exit 0) when a container is absent.
# Env: EDA_IMAGE (default vibeic/vibeic-eda:0.2.18)
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
TOOL="$(cd "$HERE/.." && pwd)"
IMAGE="${EDA_IMAGE:-vibeic/vibeic-eda:0.2.18}"

if ! command -v docker >/dev/null 2>&1; then
  echo "SKIP run_caa_test: no docker (KLayout pya needed)"; exit 0
fi

WORK="$(mktemp -d)"; trap 'rm -rf "$WORK"' EXIT
fail() { echo "FAIL: $1"; exit 1; }

cp "$TOOL/caa.py" "$HERE/gen_caa_gds.py" "$WORK/"
printf '{"mode":"short","layer_a":[10,0],"layer_b":[11,0],"window_um":[0,-5,10,10],"defect":{"x0":0.1,"d0":1.0,"law":"inverse_cube"},"x_max_um":4.0,"x_step_um":0.05}\n' > "$WORK/cfg.json"
printf '{"mode":"short","layer_a":[10,0],"layer_b":[11,0],"window_um":[0,-5,20,10],"defect":{"x0":0.1,"d0":1.0,"law":"inverse_cube"},"x_max_um":4.0,"x_step_um":0.05}\n' > "$WORK/cfg_wide.json"
printf '{"mode":"short","layer_a":[10,0],"layer_b":[11,0],"window_um":[0,-5,10,10],"defect":{"x0":0.1,"d0":2.0,"law":"inverse_cube"},"x_max_um":4.0,"x_step_um":0.05}\n' > "$WORK/cfg_d0x2.json"

echo "== generate fixtures + run the tool =="
docker run --rm --entrypoint bash -v "$WORK":/work "$IMAGE" -lc "
  export PATH=/foss/tools/klayout:\$PATH
  for m in s05 s10 wide clean; do
    CAA_MODE=\$m CAA_OUT=/work/\$m.gds klayout -b -r /work/gen_caa_gds.py >/dev/null 2>&1
  done
  CAA_GDS=/work/s05.gds  CAA_CONFIG=/work/cfg.json      CAA_OUT=/work/s05.json  klayout -b -r /work/caa.py >/dev/null 2>&1
  CAA_GDS=/work/s10.gds  CAA_CONFIG=/work/cfg.json      CAA_OUT=/work/s10.json  klayout -b -r /work/caa.py >/dev/null 2>&1
  CAA_GDS=/work/wide.gds CAA_CONFIG=/work/cfg_wide.json CAA_OUT=/work/wide.json klayout -b -r /work/caa.py >/dev/null 2>&1
  CAA_GDS=/work/s05.gds  CAA_CONFIG=/work/cfg_d0x2.json CAA_OUT=/work/d0x2.json klayout -b -r /work/caa.py >/dev/null 2>&1
  CAA_GDS=/work/clean.gds CAA_CONFIG=/work/cfg.json     CAA_OUT=/work/clean.json klayout -b -r /work/caa.py >/dev/null 2>&1
" >/dev/null 2>&1
for m in s05 s10 wide d0x2 clean; do [ -s "$WORK/$m.json" ] || fail "no report for $m"; done

ac()  { python3 -c "import json;d=json.load(open('$WORK/$1'));print(next(p['ac_um2'] for p in d['per_diameter'] if abs(p['x_um']-$2)<1e-6))"; }
awc() { python3 -c "import json;print(json.load(open('$WORK/$1'))['awc_um2'])"; }
maxac() { python3 -c "import json;d=json.load(open('$WORK/$1'));print(max(p['ac_um2'] for p in d['per_diameter']))"; }

echo "== 1. per-diameter critical area, exact to the DBU (s=0.5, W=10) =="
echo "   Ac(0.5)=$(ac s05.json 0.5)  Ac(1.0)=$(ac s05.json 1.0)  Ac(2.0)=$(ac s05.json 2.0)"
[ "$(ac s05.json 0.5)" = "0.0" ]  || fail "Ac(0.5) != 0 (defect diameter == spacing must give no overlap)"
[ "$(ac s05.json 1.0)" = "5.0" ]  || fail "Ac(1.0) != 5.0 (= 10*(1.0-0.5))"
[ "$(ac s05.json 2.0)" = "15.0" ] || fail "Ac(2.0) != 15.0 (= 10*(2.0-0.5))"
echo "  [1] Ac(x)=W*(x-s): 0, 5.0, 15.0 um^2 -- areas of the real grown-intersection polygon"

echo "== 2. weighted AWC matches the closed form W*D0*x0^2/s = 0.2 =="
[ "$(awc s05.json)" = "0.2" ] || fail "AWC != 0.2 um^2 (closed form 10*1*0.01/0.5)"
echo "  [2] AWC = 0.2 um^2 (exact piecewise-linear x inverse-cube integral)"

echo "== 3. spacing-derived: s=1.0 halves AWC to 0.1 and zeroes Ac(1.0) =="
[ "$(awc s10.json)" = "0.1" ]      || fail "s=1.0 AWC != 0.1 (must halve)"
[ "$(ac s10.json 1.0)" = "0.0" ]   || fail "s=1.0 Ac(1.0) != 0 (2r == s, no overlap)"
[ "$(ac s10.json 2.0)" = "10.0" ]  || fail "s=1.0 Ac(2.0) != 10.0 (= 10*(2.0-1.0))"
echo "  [3] s doubled -> AWC 0.2 -> 0.1, Ac(1.0) -> 0, Ac(2.0) -> 10.0 (tracks geometry)"

echo "== 4. window-derived: W=20 doubles Ac(1.0) to 10.0 and AWC to 0.4 =="
[ "$(ac wide.json 1.0)" = "10.0" ] || fail "W=20 Ac(1.0) != 10.0 (must scale with facing length)"
[ "$(awc wide.json)" = "0.4" ]     || fail "W=20 AWC != 0.4 (must double)"
echo "  [4] W doubled -> Ac(1.0) 5.0 -> 10.0, AWC 0.2 -> 0.4"

echo "== 5. defect density is a separable parameter: D0 x2 doubles AWC =="
[ "$(awc d0x2.json)" = "0.4" ]     || fail "D0 doubled AWC != 0.4 (the [EXT] half must be linear)"
[ "$(ac d0x2.json 1.0)" = "5.0" ]  || fail "D0 must NOT change the geometry Ac(1.0)"
echo "  [5] D0 2x -> AWC 0.2 -> 0.4, geometry Ac unchanged (geometry vs density separated)"

echo "== 6. PROVEN-NEGATIVE: rails 5 um apart -> Ac=0 for all x, AWC=0 =="
[ "$(maxac clean.json)" = "0.0" ] || fail "clean max Ac != 0 (no defect up to 4 um can bridge a 5 um gap)"
[ "$(awc clean.json)" = "0.0" ]   || fail "clean AWC != 0"
echo "  [6] every defect diameter gives Ac=0 -> AWC=0 (not a constant-nonzero)"

echo "PASS run_caa_test (6/6)"
