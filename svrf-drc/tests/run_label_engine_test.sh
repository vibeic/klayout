#!/usr/bin/env bash
# run_label_engine_test.sh -- UNFAKEABLE gate for text/label handling (#24):
# TEXT layers wired to NET NAMES via db::LayoutToNetlist::connect(Region, Texts),
# and the label-short electrical check built on top of them.
#
# Deck (svrf-drc/examples/label.rule):
#   LABEL netname met1              <- attach the TEXT shapes as net names
#   ERC.NAMESHORT { ERC SHORT netname }
#
# Fixture (gen_label_gds.py): three met1 rails, each carrying one designer name
# as a TEXT shape -- railX "VDD" [0,0..10,2], railY "VSS" [20,0..30,2], and
# railZ "CLK" [0,10..10,12] which never participates.
#
#   1. short: a met1 bridge joins railX to railY, so ONE extracted net carries
#      BOTH "VDD" and "VSS" -> FAIL 1. railZ, carrying one name, is NOT flagged.
#   2. HAND-COMPUTED GEOMETRY: the .lyrdb marker, loaded back through KLayout's
#      OWN pya.ReportDatabase, is ONE polygon at exactly [0,0,30,2] -- the merged
#      extent of railX + bridge + railY, i.e. the conductor to cut.
#   3. THE LABEL WIRING IS REAL (this is the #24 claim itself): with LABEL in the
#      deck the engine names the offending net "VDD,VSS" and the clean net "CLK";
#      with the LABEL line REMOVED and nothing else changed, the identical short
#      is still found but the nets fall back to the anonymous $1/$2. So the names
#      demonstrably come from the TEXT shapes through the L2N, not from the check.
#   4. PROVEN-NEGATIVE at the boundary: the same bridge 1 DBU short of railY
#      (right edge 19.999 vs railY at 20.0) leaves the nets separate -> PASS 0,
#      with an identical layer and label inventory.
#   5. PROVEN-NEGATIVE: no bridge -> PASS 0, empty marker DB.
#   6. TWO LABELS != TWO NAMES: a second "VDD" text on railX (one net, two label
#      shapes, ONE distinct name) is NOT a clash -> PASS 0. A check that counted
#      labels instead of distinct names would wrongly FAIL here.
#   7. honest-SKIP: fewer than two distinct names in the whole layout cannot
#      produce a clash, so the rule SKIPs rather than asserting a vacuous PASS.
#   8. 0 regressions: the frozen engine goldens stay byte-identical.
#
# NO vendor data. Skips (exit 0) when the db build inputs or a container are absent.
# Env: KLAYOUT_SRC/BLD/BIN + EDA_IMAGE (default vibeic/vibeic-eda:0.2.18)
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
SVRF="$(cd "$HERE/.." && pwd)"
ROOT="$(cd "$SVRF/.." && pwd)"
DBP="$ROOT/src/plugins/tools/svrf_drc/db_plugin"
RULE="$SVRF/examples/label.rule"
KSRC="${KLAYOUT_SRC:-$HOME/kbuild}"
KBLD="${KLAYOUT_BLD:-$HOME/kbuild-out/bld}"
KBIN="${KLAYOUT_BIN:-$HOME/kbuild-out/bin}"
IMAGE="${EDA_IMAGE:-vibeic/vibeic-eda:0.2.18}"

if [ ! -d "$KSRC/src/db/db" ] || [ ! -f "$KBIN/libklayout_db.so" ]; then
  echo "SKIP run_label_engine_test: no KLayout db build (set KLAYOUT_SRC/BLD/BIN)"; exit 0
fi
if ! command -v docker >/dev/null 2>&1; then
  echo "SKIP run_label_engine_test: no docker for GDS generation / rdb load"; exit 0
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
cp "$HERE/gen_label_gds.py" "$HERE/rve_load.py" "$WORK/"
docker run --rm --entrypoint bash -v "$WORK":/work "$IMAGE" -lc "
  export PATH=/foss/tools/klayout:\$PATH
  for m in short nearmiss clean dupname; do
    LBL_MODE=\$m LBL_OUT=/work/\$m.gds klayout -b -r /work/gen_label_gds.py >/dev/null 2>&1
  done
"
for g in short nearmiss clean dupname; do
  [ -s "$WORK/$g.gds" ] || fail "fixture $g generation produced no GDS"
done

run() { SVRFDRC_RVE_OUT="${3:-}" SVRFDRC_ERCNET=1 "$BIN" "${4:-$RULE}" "$WORK/$1.gds" \
          "$WORK/$2.rpt" "KLayout 0.30.9" 2>"$WORK/$2.err"; }
verdict() { awk -v n="$2" '$2==n {print $1}' "$WORK/$1.rpt"; }
count()   { awk -v n="$2" '$2==n {print $NF}' "$WORK/$1.rpt"; }
load() { docker run --rm --entrypoint bash -v "$WORK":/work "$IMAGE" -lc "
  export PATH=/foss/tools/klayout:\$PATH
  LYRDB=/work/$1 klayout -b -r /work/rve_load.py 2>&1 | grep -E '^(CAT|BBOX|NITEMS|ERROR) '
"; }

for m in short nearmiss clean dupname; do run "$m" "$m" "$WORK/$m.lyrdb"; done

echo "== 1. short: one net carries VDD and VSS -> FAIL 1 =="
grep -E '^(PASS|FAIL|SKIP) +ERC' "$WORK/short.rpt" | sed 's/^/   /'
[ "$(verdict short ERC.NAMESHORT)" = "FAIL" ] || fail "short ERC.NAMESHORT not FAIL"
[ "$(count   short ERC.NAMESHORT)" = "1" ]    || fail "short count != 1 (railZ carries ONE name and must not be flagged)"
echo "  [1] exactly 1 clashing net; the single-named railZ is untouched"

echo "== 2. HAND-COMPUTED marker geometry (KLayout's own rdb reader) =="
D="$(load short.lyrdb)"; echo "$D" | sed 's/^/   /'
echo "$D" | grep -qE '^ERROR'                        && { echo "$D"; fail "KLayout refused the .lyrdb"; }
echo "$D" | grep -qxE 'BBOX ERC.NAMESHORT:0,0,30,2'  || { echo "$D"; fail "marker != the merged shorted net [0,0,30,2]"; }
echo "$D" | grep -qxE 'NITEMS 1'                     || { echo "$D"; fail "short NITEMS != 1 (one marker per shorted net)"; }
echo "  [2] one marker at railX+bridge+railY = [0,0,30,2]"

echo "== 3. the LABEL wiring is real: names come from the TEXT shapes =="
grep -q 'net=VDD,VSS' "$WORK/short.err" || { sed 's/^/   /' "$WORK/short.err"; fail "the offending net is not named VDD,VSS -> labels are NOT reaching the netlist"; }
grep -q 'net=CLK'     "$WORK/short.err" || fail "the untouched net is not named CLK"
#  control: identical deck with the LABEL line removed -> same short, no names
grep -v '^LABEL' "$RULE" > "$WORK/nolabel.rule"
run short nolabel "" "$WORK/nolabel.rule"
[ "$(verdict nolabel ERC.NAMESHORT)" = "FAIL" ] || fail "the check must still find the short without LABEL"
grep -q 'net=\$'      "$WORK/nolabel.err" || { sed 's/^/   /' "$WORK/nolabel.err"; fail "without LABEL the nets should be anonymous (\$1/\$2)"; }
grep -q 'net=VDD,VSS' "$WORK/nolabel.err" && fail "a name appeared WITHOUT the LABEL statement -> the name is not label-derived"
echo "  [3] LABEL on -> nets named VDD,VSS / CLK; LABEL off -> same short, anonymous \$n"

echo "== 4. PROVEN-NEGATIVE at the boundary: bridge 1 DBU short of railY =="
[ "$(verdict nearmiss ERC.NAMESHORT)" = "PASS" ] \
  || fail "a bridge 1 DBU clear of railY was reported as a name clash"
echo "  [4] right edge 19.999 vs railY at 20.0 -> PASS 0"

echo "== 5. PROVEN-NEGATIVE: no bridge -> PASS, empty marker DB =="
[ "$(verdict clean ERC.NAMESHORT)" = "PASS" ] || fail "clean not PASS"
C="$(load clean.lyrdb)"
echo "$C" | grep -qE 'CAT '     && { echo "$C"; fail "clean must yield NO categories"; }
echo "$C" | grep -qxE 'NITEMS 0' || { echo "$C"; fail "clean NITEMS != 0"; }
echo "  [5] PASS, 0 categories, NITEMS=0 (not a constant-FAIL)"

echo "== 6. TWO LABELS is not TWO NAMES: a duplicate VDD is not a clash =="
[ "$(verdict dupname ERC.NAMESHORT)" = "PASS" ] \
  || fail "a second label bearing the SAME name was counted as a clash (labels counted, not distinct names)"
grep -q 'names=\[VDD\]' "$WORK/dupname.err" || fail "the duplicated name did not collapse to one distinct name"
echo "  [6] one net, two VDD labels, one distinct name -> PASS 0"

echo "== 7. honest-SKIP: fewer than two distinct names cannot clash =="
printf 'LAYER met1 4 0\nLAYER netname 40 0\nLAYER cont 3 0\nLAYER poly 2 0\nCONNECT poly met1 BY cont\nLABEL netname met1\nERC.ONE {\n  ERC SHORT netname\n}\n' > "$WORK/one.rule"
#  one rail, ONE label -> no second name exists, so no clash is possible
cat > "$WORK/gen_one.py" <<'PYEOF'
import pya
ly = pya.Layout(); ly.dbu = 0.001
top = ly.create_cell('TOP')
top.shapes(ly.layer(4, 0)).insert(pya.Box(0, 0, 10000, 2000))
top.shapes(ly.layer(40, 0)).insert(pya.Text('VDD', pya.Trans(pya.Vector(5000, 1000))))
ly.write('/work/one.gds')
PYEOF
docker run --rm --entrypoint bash -v "$WORK":/work "$IMAGE" -lc "
  export PATH=/foss/tools/klayout:\$PATH
  klayout -b -r /work/gen_one.py >/dev/null 2>&1
" >/dev/null 2>&1
[ -s "$WORK/one.gds" ] || fail "single-name fixture produced no GDS"
run one one "" "$WORK/one.rule"
[ "$(verdict one ERC.ONE)" = "SKIP" ] || fail "a layout with ONE distinct name must SKIP, not assert a vacuous PASS"
echo "  [7] single distinct name -> SKIP (never a vacuous PASS)"

echo "== 8. 0 regressions: frozen goldens byte-identical =="
EDA_IMAGE="$IMAGE" bash "$HERE/run_engine_parity.sh" >/dev/null 2>&1 || fail "engine parity regression FAILED"
echo "  [8] run_engine_parity.sh: all frozen goldens byte-identical"

echo "PASS run_label_engine_test (8/8)"
