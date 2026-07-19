#!/usr/bin/env bash
# run_cmp_gradient_test.sh -- UNFAKEABLE gate for the CMP density-gradient check
# (#49): lay a grid of density windows over the layer, measure metal density in
# each EXACTLY on KLayout's Region engine, and flag any pair of rook-adjacent
# windows whose density delta exceeds max_gradient (the dishing/erosion proxy).
#
# Fixture (gen_cmp_gds.py): 10x10 um windows (area 100 um^2). Each tile is filled
# to an EXACT density by a full-height metal strip -- density = strip_width / 10 --
# so every window density and every gradient is hand-computed.
#
#   1. step (0.80 | 0.20): worst gradient = |0.80 - 0.20| = 0.60. With
#      max_gradient 0.30 -> FAIL, and the two window densities are exactly 0.80
#      and 0.20 (measured metal area / window area).
#   2. GRADIENT IS GEOMETRY-DERIVED: the reported densities are 0.80 and 0.20 to
#      the part-in-1000, i.e. the measured metal area (80 and 20 um^2), not a
#      constant -- a hardcoded checker could not produce them.
#   3. RAMP PASSES THE SAME SWING: graded (0.80 | 0.50 | 0.20) has the SAME total
#      0.80->0.20 change but in two 0.30 steps -> PASS with max_gradient 0.30.
#      So the check is a per-neighbour STEP, not a global range.
#   4. PROVEN-NEGATIVE: flat (0.50 | 0.50) -> gradient 0.0 -> PASS.
#   5. DENSITY BOUNDARY: edge (0.80 | 0.50) delta EXACTLY 0.30 -> PASS (strict
#      '>'); justin (0.80 | 0.499) delta 0.301 -> FAIL. One part in 1000 of
#      density (a 0.01 um strip-width in a 10 um window) decides it.
#
# NO vendor data. Skips (exit 0) when a container is absent.
# Env: EDA_IMAGE (default vibeic/vibeic-eda:0.2.18)
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
TOOL="$(cd "$HERE/.." && pwd)"
IMAGE="${EDA_IMAGE:-vibeic/vibeic-eda:0.2.18}"

if ! command -v docker >/dev/null 2>&1; then
  echo "SKIP run_cmp_gradient_test: no docker (KLayout pya needed)"; exit 0
fi

WORK="$(mktemp -d)"; trap 'rm -rf "$WORK"' EXIT
fail() { echo "FAIL: $1"; exit 1; }

cp "$TOOL/cmp_gradient.py" "$HERE/gen_cmp_gds.py" "$WORK/"
printf '{"layer":[10,0],"window_um":10.0,"step_um":10.0,"max_gradient":0.30,"origin_um":[0,0]}\n' > "$WORK/cfg.json"

echo "== generate fixtures + run the tool =="
docker run --rm --entrypoint bash -v "$WORK":/work "$IMAGE" -lc "
  export PATH=/foss/tools/klayout:\$PATH
  for m in step graded flat edge justin; do
    CG_MODE=\$m CG_OUT=/work/\$m.gds klayout -b -r /work/gen_cmp_gds.py >/dev/null 2>&1
    CG_GDS=/work/\$m.gds CG_CONFIG=/work/cfg.json CG_OUT=/work/\$m.json \
      klayout -b -r /work/cmp_gradient.py >/dev/null 2>&1
  done
" >/dev/null 2>&1
for m in step graded flat edge justin; do [ -s "$WORK/$m.json" ] || fail "no report for $m"; done

j() { python3 -c "import json;print($2)" <<<"$(cat "$WORK/$1.json")"; }
V()  { python3 -c "import json;print(json.load(open('$WORK/$1.json'))['verdict'])"; }
MG() { python3 -c "import json;print(json.load(open('$WORK/$1.json'))['max_gradient'])"; }
DENS() { python3 -c "import json;print([w['density'] for w in json.load(open('$WORK/$1.json'))['windows']])"; }

echo "== 1. step (0.80|0.20): worst gradient 0.60 -> FAIL =="
[ "$(V step)" = "FAIL" ]     || fail "step not FAIL"
[ "$(MG step)" = "0.6" ]     || fail "step max_gradient != 0.60"
echo "  [1] gradient 0.60 > 0.30 -> FAIL"

echo "== 2. gradient is geometry-derived: densities are the measured metal area =="
[ "$(DENS step)" = "[0.8, 0.2]" ] || fail "step densities != [0.80, 0.20] (measured area / window area)"
echo "  [2] window densities 0.80 and 0.20 = 80 and 20 um^2 metal / 100 um^2"

echo "== 3. a ramp with the same swing PASSes (per-neighbour step, not range) =="
[ "$(V graded)" = "PASS" ]        || fail "graded should PASS (two 0.30 steps)"
[ "$(DENS graded)" = "[0.8, 0.5, 0.2]" ] || fail "graded densities != [0.80,0.50,0.20]"
echo "  [3] 0.80->0.50->0.20 ramp PASSes though the total swing equals step's"

echo "== 4. PROVEN-NEGATIVE: flat -> gradient 0.0 -> PASS =="
[ "$(V flat)" = "PASS" ]  || fail "flat not PASS"
[ "$(MG flat)" = "0.0" ]  || fail "flat max_gradient != 0.0"
echo "  [4] two equal-density windows -> 0.0 gradient (not a constant-FAIL)"

echo "== 5. density boundary: 0.30 PASSes, 0.301 FAILs =="
[ "$(V edge)" = "PASS" ]     || fail "delta == 0.30 must PASS (strict '>')"
[ "$(MG edge)" = "0.3" ]     || fail "edge max_gradient != 0.30"
[ "$(V justin)" = "FAIL" ]   || fail "delta 0.301 must FAIL"
[ "$(MG justin)" = "0.301" ] || fail "justin max_gradient != 0.301"
echo "  [5] 0.80|0.50 delta 0.30 -> PASS ; 0.80|0.499 delta 0.301 -> FAIL"

echo "PASS run_cmp_gradient_test (5/5)"
