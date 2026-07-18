#!/usr/bin/env bash
# run_diode_relief_test.sh -- UNFAKEABLE gate for diode-aware antenna relief +
# insertion-site suggestion (#45).
#
# antenna_check.py recognizes an antenna-protection diode ELECTRICALLY connected
# to a net (via LayoutToNetlist), relieves that net's antenna violation, and for
# an UNPROTECTED violating net emits a candidate diode insertion site. Proves,
# against the fork's real KLayout LayoutToNetlist engine (no mocks):
#   1. baseline VIOLATION: the ANT_LEN=100 structure (met1 50um^2 / gate 1um^2 =
#      ratio 50 > 40) FAILs, and the tool emits ONE insertion site at the
#      HAND-COMPUTED vulnerable gate  bbox=[0.75,0,1.25,2.0] centre=[1.0,1.0];
#   2. FAIL->PASS: the SAME structure with a diode marker on the antenna net is
#      RELIEVED -> PASS, relieved_nets=1, insertion_sites=0, relieved ratio 50;
#   3. PROVEN-CONNECTIVITY: a diode marker placed OFF the net (touching no
#      conductor) does NOT relieve -> still FAIL (recognition needs an electrical
#      connection, a stray diode can't hide a real antenna);
#   4. PARITY: with the pre-#45 config (no "diode_layers") the JSON is
#      BYTE-IDENTICAL to the pre-#45 tool on both a dirty and a clean fixture.
#
# NO vendor data. Skips (exit 0) when a container is absent.
# Env: EDA_IMAGE (default vibeic/vibeic-eda:0.2.18)
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
TOOL="$(cd "$HERE/.." && pwd)"
ROOT="$(cd "$TOOL/.." && pwd)"
IMAGE="${EDA_IMAGE:-vibeic/vibeic-eda:0.2.18}"

if ! command -v docker >/dev/null 2>&1; then
  echo "SKIP run_diode_relief_test: no docker (KLayout pya needed)"; exit 0
fi
WORK="$(mktemp -d)"; trap 'rm -rf "$WORK"' EXIT
fail() { echo "FAIL: $1"; exit 1; }

cp "$TOOL/gen_fixtures.py" "$TOOL/antenna_check.py" \
   "$TOOL/antenna_config.example.json" "$TOOL/antenna_config_diode.example.json" "$WORK/"
git -C "$ROOT" show HEAD:gds-antenna/antenna_check.py > "$WORK/antenna_check_HEAD.py" 2>/dev/null || \
  cp "$TOOL/antenna_check.py" "$WORK/antenna_check_HEAD.py"

echo "== generate fixtures + run checks (container klayout pya) =="
docker run --rm --entrypoint bash -v "$WORK":/work "$IMAGE" -lc '
  export PATH=/foss/tools/klayout:$PATH
  DC=/work/antenna_config_diode.example.json
  OC=/work/antenna_config.example.json
  ANT_OUT=/work/noprot.gds ANT_LEN=100                 klayout -b -r /work/gen_fixtures.py 2>/dev/null
  ANT_OUT=/work/prot.gds   ANT_LEN=100 ANT_DIODE=1        klayout -b -r /work/gen_fixtures.py 2>/dev/null
  ANT_OUT=/work/off.gds    ANT_LEN=100 ANT_DIODE_OFFNET=1 klayout -b -r /work/gen_fixtures.py 2>/dev/null
  ANT_OUT=/work/clean.gds  ANT_LEN=20                   klayout -b -r /work/gen_fixtures.py 2>/dev/null
  ANT_GDS=/work/noprot.gds ANT_CONFIG=$DC ANT_OUT=/work/noprot.json klayout -b -r /work/antenna_check.py >/dev/null 2>&1
  ANT_GDS=/work/prot.gds   ANT_CONFIG=$DC ANT_OUT=/work/prot.json   klayout -b -r /work/antenna_check.py >/dev/null 2>&1
  ANT_GDS=/work/off.gds    ANT_CONFIG=$DC ANT_OUT=/work/off.json    klayout -b -r /work/antenna_check.py >/dev/null 2>&1
  # parity: original config, HEAD vs NEW, on a dirty + a clean fixture
  for f in noprot clean; do
    ANT_GDS=/work/$f.gds ANT_CONFIG=$OC ANT_OUT=/work/${f}_head.json klayout -b -r /work/antenna_check_HEAD.py >/dev/null 2>&1
    ANT_GDS=/work/$f.gds ANT_CONFIG=$OC ANT_OUT=/work/${f}_new.json  klayout -b -r /work/antenna_check.py      >/dev/null 2>&1
  done
'
for f in noprot prot off; do [ -s "$WORK/$f.json" ] || fail "missing $f.json"; done

jq2() { python3 -c 'import json,sys; d=json.load(open(sys.argv[1])); print(eval("d"+sys.argv[2]))' "$1" "$2"; }

echo "== 1. baseline VIOLATION + hand-computed insertion site =="
[ "$(jq2 "$WORK/noprot.json" "['verdict']")" = FAIL ] || fail "noprot should FAIL"
[ "$(jq2 "$WORK/noprot.json" "['violations']")" = 1 ] || fail "noprot violations != 1"
[ "$(jq2 "$WORK/noprot.json" "['diode_recognition']['insertion_sites']")" = 1 ] || fail "noprot insertion_sites != 1"
S="$(jq2 "$WORK/noprot.json" "['per_layer']['met1']['suggestions'][0]")"
echo "   suggestion: $S"
[ "$(jq2 "$WORK/noprot.json" "['per_layer']['met1']['suggestions'][0]['gate_bbox_um']")" = "[0.75, 0.0, 1.25, 2.0]" ] || fail "gate bbox != hand-computed [0.75,0,1.25,2.0]"
[ "$(jq2 "$WORK/noprot.json" "['per_layer']['met1']['suggestions'][0]['diode_xy_um']")" = "[1.0, 1.0]" ] || fail "diode insertion xy != hand-computed [1.0,1.0]"
echo "  [1] noprot FAIL; insertion site gate_bbox=[0.75,0,1.25,2.0] xy=[1.0,1.0] (hand-computed)"

echo "== 2. FAIL->PASS: recognized diode relieves the net =="
[ "$(jq2 "$WORK/prot.json" "['verdict']")" = PASS ] || fail "prot should PASS (diode relief)"
[ "$(jq2 "$WORK/prot.json" "['violations']")" = 0 ] || fail "prot violations != 0"
[ "$(jq2 "$WORK/prot.json" "['diode_recognition']['relieved_nets']")" = 1 ] || fail "prot relieved_nets != 1"
[ "$(jq2 "$WORK/prot.json" "['diode_recognition']['insertion_sites']")" = 0 ] || fail "prot insertion_sites != 0"
[ "$(jq2 "$WORK/prot.json" "['per_layer']['met1']['relieved'][0]['ratio']")" = 50.0 ] || fail "relieved ratio != 50.0"
echo "  [2] prot PASS; relieved_nets=1 insertion_sites=0 relieved-ratio=50.0"

echo "== 3. PROVEN-CONNECTIVITY: off-net diode does NOT relieve =="
[ "$(jq2 "$WORK/off.json" "['verdict']")" = FAIL ] || fail "off-net diode must NOT relieve (still FAIL)"
[ "$(jq2 "$WORK/off.json" "['diode_recognition']['relieved_nets']")" = 0 ] || fail "off-net relieved_nets != 0"
[ "$(jq2 "$WORK/off.json" "['violations']")" = 1 ] || fail "off-net violations != 1"
echo "  [3] off-net diode: verdict=FAIL relieved_nets=0 (recognition is connectivity-based)"

echo "== 4. PARITY: original config byte-identical to pre-#45 tool =="
diff "$WORK/noprot_head.json" "$WORK/noprot_new.json" >/dev/null || fail "parity broken on dirty fixture (original config)"
diff "$WORK/clean_head.json"  "$WORK/clean_new.json"  >/dev/null || fail "parity broken on clean fixture (original config)"
echo "  [4] no diode_layers => byte-identical to HEAD on dirty + clean"

echo "PASS run_diode_relief_test (4/4)"
