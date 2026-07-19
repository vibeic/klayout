#!/usr/bin/env bash
# run_mp_color_test.sh -- UNFAKEABLE gate for multi-patterning coloring (#25):
# build the same-layer CONFLICT GRAPH (an edge between every pair of features
# closer than the single-mask minimum) and decide colorability -- 2-coloring is
# BIPARTITENESS, the obstruction is an ODD CYCLE, which the tool extracts exactly.
#
# Fixture (gen_mp_gds.py): 1x1 um squares, single-mask min spacing 0.20 um. A
# gap BELOW 0.20 um conflicts; a gap of exactly 0.20 um is clean. Every gap is
# hand-chosen, so the conflict graph -- and its coloring -- is checkable by eye.
#
#   1. triangle: three mutually-close squares -> conflict graph is a TRIANGLE
#      (3 edges) -> NOT 2-colorable -> UNCOLORABLE, odd_cycle length 3. The
#      emitted .lyrdb, loaded through KLayout's OWN pya.ReportDatabase, holds the
#      three HAND-KNOWN squares [0,0,1,1], [1.1,0,2.1,1], [0.55,1.1,1.55,2.1].
#   2. chain: the SAME three squares with the third moved far up -> a single
#      conflict edge -> COLORABLE, and the two conflicting squares get DIFFERENT
#      colors (the coloring is real, not a rubber stamp).
#   3. square4: a ring with 0.15 um orthogonal gaps but 0.212 um diagonal gaps
#      -> EXACTLY a 4-CYCLE (even) -> COLORABLE, and the printed coloring is a
#      proper checkerboard: EVERY conflicting pair has two different colors.
#   4. five: a ring of five squares -> a 5-CYCLE (odd) -> UNCOLORABLE with 2
#      masks, odd_cycle length 5 (exercises odd-cycle extraction beyond a
#      triangle); the .lyrdb holds all five ring squares.
#   5. n_colors=3 witness: the SAME triangle and the SAME 5-cycle that are
#      UNCOLORABLE with 2 masks BECOME colorable with 3, and the tool proves it
#      by printing an assignment that uses 3 colors with NO conflicting pair
#      sharing a color -- it never claims colorability it cannot witness.
#   6. DBU BOUNDARY / PROVEN-NEGATIVE: two squares at EXACTLY 0.20 um -> 0
#      conflict edges (clean); the SAME two 1 DBU (0.001 um) closer -> exactly 1
#      edge. The conflict test is pinned to the DBU.
#
# NO vendor data. Skips (exit 0) when a container is absent.
# Env: EDA_IMAGE (default vibeic/vibeic-eda:0.2.18)
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
TOOL="$(cd "$HERE/.." && pwd)"
ROOT="$(cd "$TOOL/.." && pwd)"
IMAGE="${EDA_IMAGE:-vibeic/vibeic-eda:0.2.18}"

if ! command -v docker >/dev/null 2>&1; then
  echo "SKIP run_mp_color_test: no docker (KLayout pya needed)"; exit 0
fi

WORK="$(mktemp -d)"; trap 'rm -rf "$WORK"' EXIT
fail() { echo "FAIL: $1"; exit 1; }

cp "$TOOL/mp_color.py" "$HERE/gen_mp_gds.py" "$ROOT/svrf-drc/tests/rve_load.py" "$WORK/"
printf '{"layer":[10,0],"min_spacing":0.200,"n_colors":2}\n' > "$WORK/cfg2.json"
printf '{"layer":[10,0],"min_spacing":0.200,"n_colors":3}\n' > "$WORK/cfg3.json"

#  checker.py -- assert the printed coloring is PROPER: recompute the conflict
#  graph and require every conflicting pair to hold two different colors.
cat > "$WORK/checker.py" <<'CHK'
import pya, os, json
res = json.load(open(os.environ["RES"]))
ly = pya.Layout(); ly.read(os.environ["GDS"]); dbu = ly.dbu
top = ly.top_cell()
li = ly.layer(10, 0)
sh = []
it = top.begin_shapes_rec(li)
while not it.at_end():
    s = it.shape()
    if s.is_box() or s.is_polygon():
        sh.append(s.polygon.transformed(it.trans()))
    it.next()
d = int(round(0.200 / dbu))
col = {}
for c in res["coloring"]:
    b = c["bbox_um"]
    col[(b[0], b[1], b[2], b[3])] = c["color"]
def key(p):
    b = p.bbox()
    return (round(b.left*dbu,4), round(b.bottom*dbu,4), round(b.right*dbu,4), round(b.top*dbu,4))
ncol = len(set(col.values()))
bad = 0
for i in range(len(sh)):
    for j in range(i+1, len(sh)):
        if not pya.Region(sh[i]).separation_check(pya.Region(sh[j]), d).is_empty():
            if col[key(sh[i])] == col[key(sh[j])]:
                bad += 1
print("PROPER" if bad == 0 else "IMPROPER", "colors_used=%d" % ncol, "conflicts_violated=%d" % bad)
CHK

echo "== generate fixtures + run the tool =="
docker run --rm --entrypoint bash -v "$WORK":/work "$IMAGE" -lc "
  export PATH=/foss/tools/klayout:\$PATH
  for m in triangle chain square4 five edge justin; do
    MP_MODE=\$m MP_OUT=/work/\$m.gds klayout -b -r /work/gen_mp_gds.py >/dev/null 2>&1
    MP_GDS=/work/\$m.gds MP_CONFIG=/work/cfg2.json MP_OUT=/work/\$m.2.json \
      MP_RVE=/work/\$m.2.lyrdb klayout -b -r /work/mp_color.py >/dev/null 2>&1
  done
  for m in triangle five; do
    MP_GDS=/work/\$m.gds MP_CONFIG=/work/cfg3.json MP_OUT=/work/\$m.3.json \
      klayout -b -r /work/mp_color.py >/dev/null 2>&1
  done
  RES=/work/square4.2.json GDS=/work/square4.gds klayout -b -r /work/checker.py > /work/s4.chk 2>/dev/null
  RES=/work/triangle.3.json GDS=/work/triangle.gds klayout -b -r /work/checker.py > /work/t3.chk 2>/dev/null
  RES=/work/five.3.json GDS=/work/five.gds klayout -b -r /work/checker.py > /work/f3.chk 2>/dev/null
" >/dev/null 2>&1
for m in triangle chain square4 five edge justin; do
  [ -s "$WORK/$m.2.json" ] || fail "no report for $m (n=2)"
done

jq_() { python3 -c "import json;d=json.load(open('$WORK/$1'));print($2)"; }
load() { docker run --rm --entrypoint bash -v "$WORK":/work "$IMAGE" -lc "
  export PATH=/foss/tools/klayout:\$PATH
  LYRDB=/work/$1 klayout -b -r /work/rve_load.py 2>&1 | grep -E '^(CAT|BBOX|NITEMS|ERROR) '
"; }

echo "== 1. triangle: 3-cycle -> UNCOLORABLE, odd cycle length 3 =="
[ "$(jq_ triangle.2.json 'd["verdict"]')"       = "UNCOLORABLE" ] || fail "triangle not UNCOLORABLE"
[ "$(jq_ triangle.2.json 'd["conflict_edges"]')" = "3" ]          || fail "triangle edges != 3"
[ "$(jq_ triangle.2.json 'd["odd_cycle_len"]')"  = "3" ]          || fail "triangle odd_cycle_len != 3"
D="$(load triangle.2.lyrdb)"; echo "$D" | sed 's/^/   /'
echo "$D" | grep -qE '^ERROR'                              && { echo "$D"; fail "KLayout refused the .lyrdb"; }
echo "$D" | grep -qxE 'BBOX MP_ODD_CYCLE:0,0,1,1'          || fail "triangle marker missing [0,0,1,1]"
echo "$D" | grep -qxE 'BBOX MP_ODD_CYCLE:1.1,0,2.1,1'      || fail "triangle marker missing [1.1,0,2.1,1]"
echo "$D" | grep -qxE 'BBOX MP_ODD_CYCLE:0.55,1.1,1.55,2.1' || fail "triangle marker missing [0.55,1.1,1.55,2.1]"
echo "$D" | grep -qxE 'NITEMS 3'                           || fail "triangle NITEMS != 3"
echo "  [1] odd cycle = the three hand-known squares, via KLayout's own rdb reader"

echo "== 2. chain: one edge -> COLORABLE, the conflicting pair differ in color =="
[ "$(jq_ chain.2.json 'd["verdict"]')"        = "COLORABLE" ] || fail "chain not COLORABLE"
[ "$(jq_ chain.2.json 'd["conflict_edges"]')" = "1" ]         || fail "chain edges != 1"
[ "$(jq_ chain.2.json 'len(set(c["color"] for c in d["coloring"] if c["bbox_um"][1] < 2))')" = "2" ] \
  || fail "the two low conflicting squares did NOT get different colors"
echo "  [2] single edge, the conflicting pair 2-colored differently"

echo "== 3. square4: 4-cycle -> COLORABLE, proper checkerboard =="
[ "$(jq_ square4.2.json 'd["verdict"]')"        = "COLORABLE" ] || fail "square4 not COLORABLE"
[ "$(jq_ square4.2.json 'd["conflict_edges"]')" = "4" ]         || fail "square4 edges != 4 (diagonals must NOT conflict)"
grep -q '^PROPER .* conflicts_violated=0' "$WORK/s4.chk" \
  || { cat "$WORK/s4.chk"; fail "square4 coloring is not a proper 2-coloring"; }
echo "  [3] 4-cycle, $(cat "$WORK/s4.chk") -- every conflicting pair differs"

echo "== 4. five: 5-cycle -> UNCOLORABLE, odd cycle length 5 =="
[ "$(jq_ five.2.json 'd["verdict"]')"       = "UNCOLORABLE" ] || fail "five not UNCOLORABLE"
[ "$(jq_ five.2.json 'd["conflict_edges"]')" = "5" ]          || fail "five edges != 5"
[ "$(jq_ five.2.json 'd["odd_cycle_len"]')"  = "5" ]          || fail "five odd_cycle_len != 5"
F="$(load five.2.lyrdb)"
echo "$F" | grep -qxE 'NITEMS 5' || { echo "$F"; fail "five odd-cycle marker count != 5"; }
echo "  [4] odd cycle length 5 (extraction beyond a triangle), 5 markers"

echo "== 5. n_colors=3 witnesses what 2 could not color =="
grep -q '^PROPER colors_used=3 conflicts_violated=0' "$WORK/t3.chk" \
  || { cat "$WORK/t3.chk"; fail "triangle is not properly 3-colored with 3 colors"; }
grep -q '^PROPER .* conflicts_violated=0' "$WORK/f3.chk" \
  || { cat "$WORK/f3.chk"; fail "5-cycle is not properly 3-colored"; }
[ "$(jq_ triangle.3.json 'd["verdict"]')" = "COLORABLE" ] || fail "triangle must be COLORABLE with 3 masks"
[ "$(jq_ five.3.json 'd["verdict"]')"     = "COLORABLE" ] || fail "5-cycle must be COLORABLE with 3 masks"
echo "  [5] triangle: $(cat "$WORK/t3.chk"); 5-cycle: $(cat "$WORK/f3.chk")"

echo "== 6. DBU boundary: 0.200 um clean, 0.199 um conflicts =="
[ "$(jq_ edge.2.json 'd["conflict_edges"]')"   = "0" ] || fail "gap == 0.200 um must NOT conflict (0 edges)"
[ "$(jq_ edge.2.json 'd["verdict"]')"          = "COLORABLE" ] || fail "edge not COLORABLE"
[ "$(jq_ justin.2.json 'd["conflict_edges"]')" = "1" ] || fail "gap 0.199 um (1 DBU inside) must conflict (1 edge)"
echo "  [6] 0.200 um -> 0 edges, 0.199 um -> 1 edge (pinned to the DBU)"

echo "PASS run_mp_color_test (6/6)"
