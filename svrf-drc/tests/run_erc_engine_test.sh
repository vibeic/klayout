#!/usr/bin/env bash
# run_erc_engine_test.sh -- UNFAKEABLE gate for the native in-engine ERC op
# (fork feature #13): electrical rule checks solved by db::SVRFEngine directly on
# db::LayoutToNetlist from GEOMETRY + the deck's own CONNECT stack, with NO
# schematic and NO netlist input.
#
# Deck (svrf-drc/examples/erc.rule):
#   gate = poly AND active ; tie = met1 AND cont
#   CONNECT poly met1 BY cont ; CONNECT met1 met2 BY via1
#   ERC.FLOATGATE { ERC FLOATING gate tie }   ERC.ISLAND { ERC UNCONNECTED met1 }
#   ERC.NOTIE     { ERC FLOATING gate }       <- one-operand form
#
# Fixture (gen_erc_gds.py): NET A a properly tied gate, NET B the gate under
# test, NET C an isolated met1 island. Proves, against the fork's REAL native C++
# engine and KLayout's OWN rdb reader:
#
#   1. viol -> ERC.FLOATGATE FAIL 1 + ERC.ISLAND FAIL 1, and ERC.NOTIE is an
#      honest SKIP (never a false PASS). Net A is tied and is NOT counted, so the
#      check reads connectivity, not "does a gate exist".
#   2. HAND-COMPUTED GEOMETRY: the emitted .lyrdb, loaded back through KLayout's
#      own pya.ReportDatabase, puts the FLOATGATE marker at exactly the gate_B
#      bbox [12,0,13,2] (poly_B [12,-1..13,6] AND active_B [10,0..16,2]) and the
#      ISLAND marker at exactly met1_C [30,10,32,12].
#   3. PROVEN-GEOMETRY: shifting structure B by +10 um moves the FLOATGATE marker
#      to exactly [22,0,23,2] while the untouched ISLAND marker stays put -- the
#      coordinates are read from geometry, not constants.
#   4. FAIL -> PASS: adding cont_B INSIDE poly_B (so the net reaches met1 and the
#      tie marker) flips ERC.FLOATGATE to PASS 0.
#   5. PROVEN-NEGATIVE (the boundary): the SAME cont_B + met1_B, but with cont_B
#      placed 1 DBU (0.001 um) clear of poly_B's x=13 edge, must STILL FAIL. The
#      layer inventory is identical to the passing case -- only the 1-DBU touch is
#      gone -- so a check that merely looked for "a cont and a met1 somewhere"
#      would wrongly PASS here.
#   6. PROVEN-NEGATIVE (clean): a fully repaired layout -> BOTH checks PASS 0.
#   7. dotted-category RVE regression: the foundry rule-naming convention is
#      DOTTED (ERC.FLOATGATE, and real decks use M1.S.1). KLayout's rdb reader
#      parses <item><category> as a "."-separated PATH, so the name must be
#      quoted the way rdb::Category::path() quotes it. The gate asserts the DB
#      LOADS -- before the fix this exact file was rejected by KLayout itself.
#   8. 0 regressions: the frozen engine goldens stay byte-identical.
#
# NO vendor data. Skips (exit 0) when the db build inputs or a container are absent.
# Env: KLAYOUT_SRC/BLD/BIN + EDA_IMAGE (default vibeic/vibeic-eda:0.2.18)
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
SVRF="$(cd "$HERE/.." && pwd)"
ROOT="$(cd "$SVRF/.." && pwd)"
DBP="$ROOT/src/plugins/tools/svrf_drc/db_plugin"
RULE="$SVRF/examples/erc.rule"
KSRC="${KLAYOUT_SRC:-$HOME/kbuild}"
KBLD="${KLAYOUT_BLD:-$HOME/kbuild-out/bld}"
KBIN="${KLAYOUT_BIN:-$HOME/kbuild-out/bin}"
IMAGE="${EDA_IMAGE:-vibeic/vibeic-eda:0.2.18}"

if [ ! -d "$KSRC/src/db/db" ] || [ ! -f "$KBIN/libklayout_db.so" ]; then
  echo "SKIP run_erc_engine_test: no KLayout db build (set KLAYOUT_SRC/BLD/BIN)"; exit 0
fi
if ! command -v docker >/dev/null 2>&1; then
  echo "SKIP run_erc_engine_test: no docker for GDS generation / rdb load"; exit 0
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
cp "$HERE/gen_erc_gds.py" "$HERE/rve_load.py" "$WORK/"
docker run --rm --entrypoint bash -v "$WORK":/work "$IMAGE" -lc "
  export PATH=/foss/tools/klayout:\$PATH
  for m in viol moved fixed nearmiss clean; do
    ERC_MODE=\$m ERC_OUT=/work/\$m.gds klayout -b -r /work/gen_erc_gds.py >/dev/null 2>&1
  done
"
for g in viol moved fixed nearmiss clean; do
  [ -s "$WORK/$g.gds" ] || fail "fixture $g generation produced no GDS"
done

#  run the native engine on one fixture -> report (+ .lyrdb when $2 is given)
run() { SVRFDRC_RVE_OUT="${2:-}" "$BIN" "$RULE" "$WORK/$1.gds" "$WORK/$1.rpt" "KLayout 0.30.9" 2>/dev/null; }
#  verdict + count of one rule from the frozen report
verdict() { awk -v n="$2" '$2==n {print $1}' "$WORK/$1.rpt"; }
count()   { awk -v n="$2" '$2==n {print $NF}' "$WORK/$1.rpt"; }
#  load a .lyrdb through KLayout's OWN rdb reader -> canonical digest
load() { docker run --rm --entrypoint bash -v "$WORK":/work "$IMAGE" -lc "
  export PATH=/foss/tools/klayout:\$PATH
  LYRDB=/work/$1 klayout -b -r /work/rve_load.py 2>&1 | grep -E '^(CAT|BBOX|NITEMS|ERROR) '
"; }

echo "== 1. viol: FLOATGATE FAIL 1 + ISLAND FAIL 1 + NOTIE honest SKIP =="
run viol "$WORK/viol.lyrdb"
grep -E '^(PASS|FAIL|SKIP) +ERC' "$WORK/viol.rpt" | sed 's/^/   /'
[ "$(verdict viol ERC.FLOATGATE)" = "FAIL" ] || fail "viol ERC.FLOATGATE not FAIL"
[ "$(count   viol ERC.FLOATGATE)" = "1" ]    || fail "viol ERC.FLOATGATE count != 1 (net A is tied and must NOT count)"
[ "$(verdict viol ERC.ISLAND)"    = "FAIL" ] || fail "viol ERC.ISLAND not FAIL"
[ "$(count   viol ERC.ISLAND)"    = "1" ]    || fail "viol ERC.ISLAND count != 1"
[ "$(verdict viol ERC.NOTIE)"     = "SKIP" ] || fail "one-operand ERC FLOATING must SKIP, never PASS"
echo "  [1] FLOATGATE=1 (only the untied net B), ISLAND=1, NOTIE=SKIP"

echo "== 2. HAND-COMPUTED marker geometry (loaded by KLayout's own rdb reader) =="
D="$(load viol.lyrdb)"; echo "$D" | sed 's/^/   /'
echo "$D" | grep -qE '^ERROR'                              && { echo "$D"; fail "KLayout refused to load the .lyrdb"; }
echo "$D" | grep -qxE 'BBOX ERC.FLOATGATE:12,0,13,2'       || { echo "$D"; fail "FLOATGATE marker != hand-computed gate_B [12,0,13,2]"; }
echo "$D" | grep -qxE 'BBOX ERC.ISLAND:30,10,32,12'        || { echo "$D"; fail "ISLAND marker != hand-computed met1_C [30,10,32,12]"; }
echo "$D" | grep -qxE 'NITEMS 2'                           || { echo "$D"; fail "viol NITEMS != 2"; }
echo "  [2] markers at gate_B [12,0,13,2] and met1_C [30,10,32,12], NITEMS=2"

echo "== 3. PROVEN-GEOMETRY: structure B shifted +10 um -> marker tracks to [22,0,23,2] =="
run moved "$WORK/moved.lyrdb"
M="$(load moved.lyrdb)"
echo "$M" | grep -qxE 'BBOX ERC.FLOATGATE:22,0,23,2'       || { echo "$M"; fail "moved: FLOATGATE marker not tracked to [22,0,23,2] (coords hardcoded?)"; }
echo "$M" | grep -qE  'BBOX ERC.FLOATGATE:12,0,13,2'       && { echo "$M"; fail "moved: stale [12,0,13,2] present -> coords NOT read from geometry"; }
echo "$M" | grep -qxE 'BBOX ERC.ISLAND:30,10,32,12'        || { echo "$M"; fail "moved: the untouched ISLAND marker must stay at [30,10,32,12]"; }
echo "  [3] B tracked to [22,0,23,2]; untouched island unmoved -> geometry-derived"

echo "== 4. FAIL -> PASS: cont_B placed INSIDE poly_B ties the net =="
run fixed
[ "$(verdict fixed ERC.FLOATGATE)" = "PASS" ] || fail "fixed ERC.FLOATGATE did not flip to PASS"
[ "$(count   fixed ERC.FLOATGATE)" = "0" ]    || fail "fixed ERC.FLOATGATE count != 0"
[ "$(verdict fixed ERC.ISLAND)"    = "FAIL" ] || fail "fixed: the untouched island must still FAIL (no blanket-PASS)"
echo "  [4] FLOATGATE FAIL 1 -> PASS 0; the untouched ISLAND still FAILs"

echo "== 5. PROVEN-NEGATIVE: cont_B 1 DBU clear of poly_B -> must STILL FAIL =="
run nearmiss
[ "$(verdict nearmiss ERC.FLOATGATE)" = "FAIL" ] || fail "1-DBU-detached cont wrongly relieved the floating gate (presence, not connectivity!)"
[ "$(count   nearmiss ERC.FLOATGATE)" = "1" ]    || fail "nearmiss ERC.FLOATGATE count != 1"
[ "$(verdict nearmiss ERC.ISLAND)"    = "FAIL" ] || fail "nearmiss ERC.ISLAND not FAIL"
echo "  [5] same layers, 1 DBU (0.001 um) gap -> still FAIL 1 (true connectivity)"

echo "== 6. PROVEN-NEGATIVE: fully repaired layout -> both checks PASS =="
run clean "$WORK/clean.lyrdb"
[ "$(verdict clean ERC.FLOATGATE)" = "PASS" ] || fail "clean ERC.FLOATGATE not PASS"
[ "$(verdict clean ERC.ISLAND)"    = "PASS" ] || fail "clean ERC.ISLAND not PASS"
C="$(load clean.lyrdb)"
echo "$C" | grep -qE 'CAT '   && { echo "$C"; fail "clean layout must yield NO categories"; }
echo "$C" | grep -qxE 'NITEMS 0' || { echo "$C"; fail "clean NITEMS != 0"; }
echo "  [6] clean: both PASS, 0 categories, NITEMS=0 (not a constant-FAIL)"

echo "== 7. dotted-category .lyrdb regression (foundry rule naming) =="
#  Every category above is dotted (ERC.FLOATGATE / ERC.ISLAND) and step 2 proved
#  KLayout's own reader accepts the file. Assert the on-disk quoting explicitly so
#  a regression to the raw-name form is caught even if the reader gets laxer.
grep -q "<category>&apos;ERC.FLOATGATE&apos;</category>\|<category>'ERC.FLOATGATE'</category>" "$WORK/viol.lyrdb" \
  || fail "dotted item category is not quoted -> KLayout parses it as a category PATH"
echo "  [7] dotted rule names are path-quoted in <item><category>"

echo "== 8. 0 regressions: frozen goldens byte-identical =="
EDA_IMAGE="$IMAGE" bash "$HERE/run_engine_parity.sh" >/dev/null 2>&1 || fail "engine parity regression FAILED"
echo "  [8] run_engine_parity.sh: all frozen goldens byte-identical"

echo "PASS run_erc_engine_test (8/8)"
