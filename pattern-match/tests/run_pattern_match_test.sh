#!/usr/bin/env bash
# run_pattern_match_test.sh -- UNFAKEABLE gate for geometric pattern matching (#21).
#
# pattern_match.py finds every EXACT congruent occurrence of a reference shape on a
# target layer, using KLayout's real Region engine. Proves (no mocks):
#   1. TRANSLATION: an asymmetric L pattern occurs 3 times same-orientation -> EXACTLY
#      3 matches at the HAND-KNOWN bboxes [10,10,13,12] [20,10,23,12] [10,20,13,22];
#   2. RIGID: enabling the 8 rigid orientations adds the 90-deg-rotated copy ->
#      EXACTLY 4 matches (the extra at [30,10,32,13]) -- count tracks the flag;
#   3. SPECIFICITY: a DIFFERENT L that shares the pattern's 3x2 bbox (notch at x=2,
#      @ (20,20)) is NEVER matched, and a plain rectangle is NEVER matched -- so the
#      match is true geometric congruence, not a bbox-only fake;
#   4. PROVEN-NEGATIVE: an ABSENT pattern (a T-shape from a separate GDS) -> 0
#      matches (not constant-nonempty);
#   5. pattern_vertices == 6 (the L is compiled, not guessed).
#
# NO vendor data. Skips (exit 0) when a container is absent.
# Env: EDA_IMAGE (default vibeic/vibeic-eda:0.2.18)
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
TOOL="$(cd "$HERE/.." && pwd)"
IMAGE="${EDA_IMAGE:-vibeic/vibeic-eda:0.2.18}"

if ! command -v docker >/dev/null 2>&1; then
  echo "SKIP run_pattern_match_test: no docker (KLayout pya needed)"; exit 0
fi
WORK="$(mktemp -d)"; trap 'rm -rf "$WORK"' EXIT
fail() { echo "FAIL: $1"; exit 1; }

cp "$TOOL/pattern_match.py" "$HERE/gen_pattern_gds.py" "$WORK/"
cat > "$WORK/cfg_trans.json" <<'J'
{ "target_layer":[10,0], "pattern_layer":[20,0], "orientations":"translation" }
J
cat > "$WORK/cfg_rigid.json" <<'J'
{ "target_layer":[10,0], "pattern_layer":[20,0], "orientations":"rigid" }
J
cat > "$WORK/cfg_absent.json" <<'J'
{ "target_layer":[10,0], "pattern_layer":[20,0], "orientations":"rigid", "pattern_gds":"/work/alt.gds" }
J

echo "== generate fixture + run matcher (container klayout pya) =="
docker run --rm --entrypoint bash -v "$WORK":/work "$IMAGE" -lc '
  export PATH=/foss/tools/klayout:$PATH
  PM_FIX_OUT=/work/pattern.gds klayout -b -r /work/gen_pattern_gds.py 2>/dev/null
  PM_ALT_OUT=/work/alt.gds     klayout -b -r /work/gen_pattern_gds.py 2>/dev/null
  PM_GDS=/work/pattern.gds PM_CONFIG=/work/cfg_trans.json  PM_OUT=/work/t.json klayout -b -r /work/pattern_match.py >/dev/null 2>&1
  PM_GDS=/work/pattern.gds PM_CONFIG=/work/cfg_rigid.json  PM_OUT=/work/r.json klayout -b -r /work/pattern_match.py >/dev/null 2>&1
  PM_GDS=/work/pattern.gds PM_CONFIG=/work/cfg_absent.json PM_OUT=/work/a.json klayout -b -r /work/pattern_match.py >/dev/null 2>&1
'
for f in t r a; do [ -s "$WORK/$f.json" ] || fail "missing $f.json"; done

jq2() { python3 -c 'import json,sys; d=json.load(open(sys.argv[1])); print(eval("d"+sys.argv[2]))' "$1" "$2"; }
locs() { python3 -c 'import json,sys; d=json.load(open(sys.argv[1])); print(sorted(m["bbox_um"] for m in d["locations"]))' "$1"; }

echo "== 1. TRANSLATION: exactly 3 matches at hand-known bboxes =="
[ "$(jq2 "$WORK/t.json" "['matches']")" = 3 ] || { cat "$WORK/t.json"; fail "translation matches != 3"; }
[ "$(locs "$WORK/t.json")" = "[[10.0, 10.0, 13.0, 12.0], [10.0, 20.0, 13.0, 22.0], [20.0, 10.0, 23.0, 12.0]]" ] \
  || { locs "$WORK/t.json"; fail "translation match bboxes != hand-known {A,B,C}"; }
echo "  [1] translation: 3 matches at {A,B,C} hand-known bboxes"

echo "== 2. RIGID: exactly 4 matches (adds the rot-90 copy at [30,10,32,13]) =="
[ "$(jq2 "$WORK/r.json" "['matches']")" = 4 ] || { cat "$WORK/r.json"; fail "rigid matches != 4"; }
python3 -c 'import json,sys
d=json.load(open(sys.argv[1]))
assert [30.0,10.0,32.0,13.0] in [m["bbox_um"] for m in d["locations"]], "missing rot-90 match"' "$WORK/r.json" \
  || fail "rigid did not add the rot-90 copy"
echo "  [2] rigid: 4 matches (adds rot-90 at [30,10,32,13]); count tracks the orientation flag"

echo "== 3. SPECIFICITY: same-bbox different-notch L and a rectangle NEVER match =="
python3 -c 'import json,sys
for p in sys.argv[1:]:
    d=json.load(open(p))
    for m in d["locations"]:
        assert m["bbox_um"]!=[20.0,20.0,23.0,22.0], "matched the different-notch L (bbox-only fake!)"
        assert m["bbox_um"]!=[30.0,20.0,33.0,22.0], "matched the rectangle"' "$WORK/t.json" "$WORK/r.json" \
  || fail "specificity broken: a non-congruent same-bbox shape matched"
echo "  [3] different-notch L (same 3x2 bbox) + rectangle excluded -> true congruence, not bbox"

echo "== 4. PROVEN-NEGATIVE: an absent (T-shape) pattern -> 0 matches =="
[ "$(jq2 "$WORK/a.json" "['matches']")" = 0 ] || { cat "$WORK/a.json"; fail "absent pattern should yield 0 matches"; }
echo "  [4] absent T-pattern: 0 matches (not constant-nonempty)"

echo "== 5. pattern compiled: 6 vertices =="
[ "$(jq2 "$WORK/t.json" "['pattern_vertices']")" = 6 ] || fail "pattern_vertices != 6"
echo "  [5] pattern_vertices=6"

echo "PASS run_pattern_match_test (5/5)"
