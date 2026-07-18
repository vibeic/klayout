#!/usr/bin/env bash
# run_lvs_recon_test.sh -- UNFAKEABLE gate for LVS-recon short isolation (#30):
# path-trace the shape-level adjacency graph between two seeds that are supposed
# to be on SEPARATE nets and isolate the CULPRITS -- the shapes whose removal
# genuinely breaks the connection (cut vertices on the A-B paths).
#
# Fixture (gen_recon_gds.py): two met1 rails railA [0,0..20,2] and railB
# [0,10..20,12], one seed marker on each, and an 8 um gap bridged differently per
# mode. Every culprit bbox below is hand-known from the generator.
#
#   1. bridge   ONE met1 bridge -> SHORTED, path length 3, EXACTLY ONE culprit at
#      the HAND-KNOWN bbox [8,2,10,10]; the emitted .lyrdb loads back through
#      KLayout's OWN pya.ReportDatabase with that same single marker.
#   2. PROVEN-GEOMETRY: the same bridge at x=14..16 moves the culprit to exactly
#      [14,2,16,10] with no stale [8,...] -- coordinates read from geometry.
#   3. HONEST NON-ANSWER: with TWO independent bridges the nets are still SHORTED,
#      but NO single shape is a cut vertex -> 0 culprits + redundant_paths=true
#      and an EMPTY marker DB. This is the test a "blame something on the path"
#      heuristic fails: it would name one of the two bridges.
#   4. CUT-VERTEX SEMANTICS ARE REAL, not positional: the culprit reported in (1)
#      is re-verified by CONSTRUCTION -- deleting exactly that shape from the
#      layout and re-running yields verdict SEPARATE.
#   5. CONNECT-STACK TRAVERSAL: with no met1 bridge at all, a short that runs
#      down through cont -> poly strap -> cont is still found (path length 5,
#      3 culprits: each link of the only chain). poly reaches met1 ONLY BY cont,
#      so this cannot come from same-layer touching.
#   6. PROVEN-NEGATIVE at the boundary: the same bridge shortened by 1 DBU
#      (top edge at y=9.999 instead of 10.0) no longer touches railB -> SEPARATE.
#      Same layers, same shape count as (1).
#   7. PROVEN-NEGATIVE: no bridge -> SEPARATE, 0 culprits, empty marker DB.
#   8. DIRECT ABUTMENT: when the two rails touch each other there is no
#      intervening shape to delete -> direct_abutment=true, 0 culprits, and the
#      abutment bbox is exactly the shared edge [0,2,20,2].
#
# NO vendor data. Skips (exit 0) when a container is absent.
# Env: EDA_IMAGE (default vibeic/vibeic-eda:0.2.18)
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
TOOL="$(cd "$HERE/.." && pwd)"
ROOT="$(cd "$TOOL/.." && pwd)"
IMAGE="${EDA_IMAGE:-vibeic/vibeic-eda:0.2.18}"

if ! command -v docker >/dev/null 2>&1; then
  echo "SKIP run_lvs_recon_test: no docker (KLayout pya needed)"; exit 0
fi

WORK="$(mktemp -d)"; trap 'rm -rf "$WORK"' EXIT
fail() { echo "FAIL: $1"; exit 1; }

cp "$TOOL/lvs_recon.py" "$HERE/gen_recon_gds.py" "$ROOT/svrf-drc/tests/rve_load.py" "$WORK/"
cat > "$WORK/cfg.json" <<'CFG'
{
  "conductors": {"poly": [2,0], "cont": [3,0], "met1": [4,0]},
  "connects": [["poly","met1","cont"]],
  "seed_layer": [30,0]
}
CFG

#  delete_bridge.py -- rebuild `bridge` WITHOUT the shape the tool blamed, to
#  re-verify the cut-vertex claim by construction rather than by assertion.
cat > "$WORK/delete_culprit.py" <<'DEL'
import pya, os, json
res = json.load(open(os.environ["RES"]))
c = res["culprits"][0]["bbox_um"]
ly = pya.Layout(); ly.read(os.environ["SRC"])
top = ly.top_cell()
li = ly.find_layer(4, 0)
kill = pya.Box(int(c[0]*1000), int(c[1]*1000), int(c[2]*1000), int(c[3]*1000))
todo = [s for s in top.shapes(li).each() if s.box == kill]
assert len(todo) == 1, "expected exactly one shape at the reported bbox, got %d" % len(todo)
for s in todo:
    top.shapes(li).erase(s)
ly.write(os.environ["DST"])
print("erased", c)
DEL

echo "== generate fixtures + run the tool (container klayout pya) =="
docker run --rm --entrypoint bash -v "$WORK":/work "$IMAGE" -lc "
  export PATH=/foss/tools/klayout:\$PATH
  for m in bridge moved twopath viapath nearmiss clean abut; do
    LR_MODE=\$m LR_GDS_OUT=/work/\$m.gds klayout -b -r /work/gen_recon_gds.py >/dev/null 2>&1
    LR_GDS=/work/\$m.gds LR_CONFIG=/work/cfg.json LR_OUT=/work/\$m.json \
      LR_RVE=/work/\$m.lyrdb klayout -b -r /work/lvs_recon.py >/dev/null 2>&1
  done
  # (4) re-verify the cut vertex BY CONSTRUCTION: delete it and re-run
  RES=/work/bridge.json SRC=/work/bridge.gds DST=/work/repaired.gds \
    klayout -b -r /work/delete_culprit.py >/dev/null 2>&1
  LR_GDS=/work/repaired.gds LR_CONFIG=/work/cfg.json LR_OUT=/work/repaired.json \
    klayout -b -r /work/lvs_recon.py >/dev/null 2>&1
" >/dev/null 2>&1
for m in bridge moved twopath viapath nearmiss clean abut repaired; do
  [ -s "$WORK/$m.json" ] || fail "no report produced for $m"
done

jq_() { python3 -c "import json,sys;d=json.load(open('$WORK/$1.json'));print($2)"; }
load() { docker run --rm --entrypoint bash -v "$WORK":/work "$IMAGE" -lc "
  export PATH=/foss/tools/klayout:\$PATH
  LYRDB=/work/$1 klayout -b -r /work/rve_load.py 2>&1 | grep -E '^(CAT|BBOX|NITEMS|ERROR) '
"; }

echo "== 1. bridge: SHORTED, exactly one culprit at the hand-known bbox =="
[ "$(jq_ bridge 'd["verdict"]')"     = "SHORTED" ] || fail "bridge not SHORTED"
[ "$(jq_ bridge 'd["path_length"]')" = "3" ]       || fail "bridge path_length != 3 (railA/bridge/railB)"
[ "$(jq_ bridge 'len(d["culprits"])')" = "1" ]     || fail "bridge culprits != 1"
[ "$(jq_ bridge 'd["culprits"][0]["bbox_um"]')" = "[8.0, 2.0, 10.0, 10.0]" ] \
  || fail "bridge culprit bbox != hand-known [8,2,10,10]"
D="$(load bridge.lyrdb)"; echo "$D" | sed 's/^/   /'
echo "$D" | grep -qE '^ERROR'                    && { echo "$D"; fail "KLayout refused the .lyrdb"; }
echo "$D" | grep -qxE 'BBOX LVS_SHORT:8,2,10,10' || { echo "$D"; fail "marker != [8,2,10,10]"; }
echo "$D" | grep -qxE 'NITEMS 1'                 || { echo "$D"; fail "bridge NITEMS != 1"; }
echo "  [1] culprit = the bridge [8,2,10,10], confirmed through KLayout's own rdb reader"

echo "== 2. PROVEN-GEOMETRY: bridge at x=14..16 -> culprit tracks =="
[ "$(jq_ moved 'd["culprits"][0]["bbox_um"]')" = "[14.0, 2.0, 16.0, 10.0]" ] \
  || fail "moved culprit not tracked to [14,2,16,10] (coords hardcoded?)"
M="$(load moved.lyrdb)"
echo "$M" | grep -qxE 'BBOX LVS_SHORT:14,2,16,10' || { echo "$M"; fail "moved marker != [14,2,16,10]"; }
echo "$M" | grep -qE  'BBOX LVS_SHORT:8,2,10,10'  && { echo "$M"; fail "stale [8,2,10,10] present"; }
echo "  [2] culprit follows the geometry; no stale coordinate"

echo "== 3. HONEST NON-ANSWER: two independent bridges -> no cut vertex =="
[ "$(jq_ twopath 'd["verdict"]')"          = "SHORTED" ] || fail "twopath must still be SHORTED"
[ "$(jq_ twopath 'len(d["culprits"])')"    = "0" ]       || fail "twopath must name NO culprit (neither bridge is a cut vertex)"
[ "$(jq_ twopath 'd["redundant_paths"]')"  = "True" ]    || fail "twopath redundant_paths not set"
T="$(load twopath.lyrdb)"
echo "$T" | grep -qxE 'NITEMS 0' || { echo "$T"; fail "twopath marker DB must be empty"; }
echo "  [3] SHORTED but 0 culprits + redundant_paths -> refuses to blame an arbitrary shape"

echo "== 4. the cut vertex re-verified BY CONSTRUCTION (delete it, re-run) =="
[ "$(jq_ repaired 'd["verdict"]')"       = "SEPARATE" ] || fail "deleting the named culprit did NOT clear the short -> the claim was wrong"
[ "$(jq_ repaired 'len(d["culprits"])')" = "0" ]        || fail "repaired layout still reports culprits"
echo "  [4] erasing exactly the reported shape flips SHORTED -> SEPARATE"

echo "== 5. CONNECT-stack traversal: short through cont -> poly -> cont =="
[ "$(jq_ viapath 'd["verdict"]')"       = "SHORTED" ] || fail "viapath short not found (CONNECT stack not traversed)"
[ "$(jq_ viapath 'd["path_length"]')"   = "5" ]       || fail "viapath path_length != 5"
[ "$(jq_ viapath 'len(d["culprits"])')" = "3" ]       || fail "viapath culprits != 3 (each link of the only chain is a cut vertex)"
[ "$(jq_ viapath 'sorted(c["layer"] for c in d["culprits"])')" = "['cont', 'cont', 'poly']" ] \
  || fail "viapath culprit layers != cont/cont/poly"
echo "  [5] path length 5, culprits cont+poly+cont -- poly reaches met1 only BY cont"

echo "== 6. PROVEN-NEGATIVE at the boundary: bridge 1 DBU short of railB =="
[ "$(jq_ nearmiss 'd["verdict"]')" = "SEPARATE" ] \
  || fail "a bridge 1 DBU clear of railB was reported as a short (bbox proximity, not adjacency!)"
echo "  [6] top edge at y=9.999 vs railB at y=10.0 -> SEPARATE"

echo "== 7. PROVEN-NEGATIVE: no bridge -> SEPARATE, empty marker DB =="
[ "$(jq_ clean 'd["verdict"]')"       = "SEPARATE" ] || fail "clean not SEPARATE"
[ "$(jq_ clean 'len(d["culprits"])')" = "0" ]        || fail "clean culprits != 0"
C="$(load clean.lyrdb)"
echo "$C" | grep -qE 'CAT '     && { echo "$C"; fail "clean must yield NO categories"; }
echo "$C" | grep -qxE 'NITEMS 0' || { echo "$C"; fail "clean NITEMS != 0"; }
echo "  [7] SEPARATE, 0 categories, NITEMS=0 (not a constant-SHORTED)"

echo "== 8. direct abutment: no intervening shape to delete =="
[ "$(jq_ abut 'd["verdict"]')"          = "SHORTED" ] || fail "abut not SHORTED"
[ "$(jq_ abut 'd["direct_abutment"]')"  = "True" ]    || fail "abut direct_abutment not set"
[ "$(jq_ abut 'len(d["culprits"])')"    = "0" ]       || fail "abut must name no culprit (the rails themselves are never culprits)"
[ "$(jq_ abut 'd["abutment_bbox_um"]')" = "[0.0, 2.0, 20.0, 2.0]" ] \
  || fail "abutment bbox != the shared edge [0,2,20,2]"
echo "  [8] direct_abutment at exactly the shared edge [0,2,20,2], 0 culprits"

echo "PASS run_lvs_recon_test (8/8)"
