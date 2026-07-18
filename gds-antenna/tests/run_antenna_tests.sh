#!/usr/bin/env bash
# run_antenna_tests.sh — UNFAKEABLE gate for the GDS-geometry antenna deck.
#
# Proves, against the fork's real KLayout engine (no mocks):
#   1. a fabricated antenna-ratio VIOLATION (met1 area / gate area = 50, limit 40)
#      is FLAGGED, and the reported ratio equals the HAND-COMPUTED value (50.0);
#   2. a CLEAN structure (ratio 10 < 40) PASSes with 0 violations;
#   3. the STAGED model ignores an upper-metal jumper that only exists AFTER the
#      met1 etch stage (a met2 relief wire does NOT lower the met1-stage ratio);
#   4. the router cross-check AGREEs on clean/dirty for both fixtures.
#   5. CAA (cumulative antenna, #44): a design that PASSES the per-layer ratio on
#      every metal FAILS cumulative — CAA catches what per-layer structurally misses,
#      and the reported cumulative ratio equals the HAND-COMPUTED value (60.18).
#
# Skips (exit 0, SKIP banner) if no KLayout binary is on PATH — matches the plugin's
# honest-skip convention so CI without the fork build stays green.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
TOOL="$HERE/.."
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# locate a KLayout batch runner (strmrun or klayout -b -r)
KL=""
if command -v strmrun >/dev/null 2>&1; then KL="strmrun"; RFLAG="";
elif command -v klayout >/dev/null 2>&1; then KL="klayout"; RFLAG="-b -r";
else
  for c in /home/reyerchu/vibe-ic-forks/klayout-build/*/strmrun; do
    [ -x "$c" ] && { KL="$c"; RFLAG=""; break; }
  done
fi
if [ -z "$KL" ]; then
  echo "SKIP run_antenna_tests: no KLayout binary (strmrun/klayout) on PATH"; exit 0
fi
run_kl() { $KL $RFLAG "$1" 2>&1 | grep -vE 'tlXMLParser|pcell_declaration'; }

fail() { echo "FAIL: $1"; exit 1; }
CFG="$TOOL/antenna_config.example.json"

# --- fixtures -------------------------------------------------------------
ANT_OUT="$WORK/viol.gds"  ANT_LEN=100 run_kl "$TOOL/gen_fixtures.py" >/dev/null
ANT_OUT="$WORK/clean.gds" ANT_LEN=20  run_kl "$TOOL/gen_fixtures.py" >/dev/null
# a fixture whose met1 ratio is 50 BUT which also carries a big met2 jumper (len 200):
# in the final netlist the node is huge, but at the met1 etch STAGE the jumper is
# absent, so the met1-stage ratio must stay 50 (staged-model proof).
ANT_OUT="$WORK/staged.gds" ANT_LEN=100 ANT_MET2_LEN=200 run_kl "$TOOL/gen_fixtures.py" >/dev/null
[ -s "$WORK/viol.gds" ] && [ -s "$WORK/clean.gds" ] || fail "fixture generation produced no GDS"

# --- 1. violation fixture: FAIL + hand-computed ratio 50 ------------------
ANT_GDS="$WORK/viol.gds" ANT_CONFIG="$CFG" ANT_OUT="$WORK/viol.json" \
  run_kl "$TOOL/antenna_check.py" >/dev/null
python3 - "$WORK/viol.json" <<'PY' || exit 1
import json,sys
d=json.load(open(sys.argv[1]))
assert d["verdict"]=="FAIL", d
assert d["violations"]==1, d
r=d["per_layer"]["met1"]["worst_ratio"]
assert abs(r-50.0)<1e-6, f"met1 ratio {r} != hand-computed 50.0"
print("  [1] violation flagged; ratio == 50.0 (hand-computed) OK")
PY

# --- 2. clean fixture: PASS, ratio 10 ------------------------------------
ANT_GDS="$WORK/clean.gds" ANT_CONFIG="$CFG" ANT_OUT="$WORK/clean.json" \
  run_kl "$TOOL/antenna_check.py" >/dev/null
python3 - "$WORK/clean.json" <<'PY' || exit 1
import json,sys
d=json.load(open(sys.argv[1]))
assert d["verdict"]=="PASS", d
assert d["violations"]==0, d
r=d["per_layer"]["met1"]["worst_ratio"]
assert abs(r-10.0)<1e-6, f"met1 ratio {r} != hand-computed 10.0"
print("  [2] clean passes; ratio == 10.0 (hand-computed) OK")
PY

# --- 3. staged model: met2 jumper must NOT relieve the met1 stage ---------
ANT_GDS="$WORK/staged.gds" ANT_CONFIG="$CFG" ANT_OUT="$WORK/staged.json" \
  run_kl "$TOOL/antenna_check.py" >/dev/null
python3 - "$WORK/staged.json" <<'PY' || exit 1
import json,sys
d=json.load(open(sys.argv[1]))
m1=d["per_layer"]["met1"]["worst_ratio"]
assert abs(m1-50.0)<1e-6, f"staged met1 ratio {m1} != 50.0 (upper jumper leaked in!)"
assert d["per_layer"]["met1"]["violations"]>=1, d
print("  [3] staged model: met1-stage ratio stays 50.0 despite met2 jumper OK")
PY

# --- 4. router cross-check: AGREE on clean/dirty -------------------------
printf '[INFO ANT] Found 1 net violations.\n[INFO ANT] Found 0 pin violations.\n' \
  > "$WORK/router_dirty.rpt"
printf '[INFO ANT] Found 0 net violations.\n[INFO ANT] Found 0 pin violations.\n' \
  > "$WORK/router_clean.rpt"
python3 "$TOOL/xcheck_router.py" --deck "$WORK/viol.json"  --router "$WORK/router_dirty.rpt" >/dev/null || fail "xcheck should AGREE (both dirty)"
python3 "$TOOL/xcheck_router.py" --deck "$WORK/clean.json" --router "$WORK/router_clean.rpt" >/dev/null || fail "xcheck should AGREE (both clean)"
# and a DISAGREE must be caught (deck dirty, router clean)
if python3 "$TOOL/xcheck_router.py" --deck "$WORK/viol.json" --router "$WORK/router_clean.rpt" >/dev/null; then
  fail "xcheck must FLAG a clean/dirty disagreement"
fi
echo "  [4] router cross-check AGREEs on clean/dirty; disagreement is caught OK"

# --- 5. CAA (#44): passes per-layer, FAILS cumulative ---------------------
# CAA fixture: met1=60um (ratio 30) + met2=60um (ratio 30), joined by via1. Every
# per-layer metal ratio is 30 < 40 (per-layer PASS), but the cumulative interconnect
# area sum met1(30)+cont(0.09)+via1(0.09)+met2(30)=60.18 over gate(1.0) exceeds the
# cumulative bound 50 -> CAA FAIL. Hand-computable, no vendor data.
CFG_PL="$TOOL/antenna_config_perlayer.example.json"
CFG_CAA="$TOOL/antenna_config_caa.example.json"
ANT_OUT="$WORK/caa.gds" ANT_LEN=60 ANT_MET2_LEN=60 run_kl "$TOOL/gen_fixtures.py" >/dev/null
[ -s "$WORK/caa.gds" ] || fail "CAA fixture generation produced no GDS"

# 5a: the TRUE per-layer check PASSES this design (proves per-layer misses it)
ANT_GDS="$WORK/caa.gds" ANT_CONFIG="$CFG_PL" ANT_OUT="$WORK/caa_pl.json" \
  run_kl "$TOOL/antenna_check.py" >/dev/null
python3 - "$WORK/caa_pl.json" <<'PY' || exit 1
import json,sys
d=json.load(open(sys.argv[1]))
assert d["verdict"]=="PASS", d
assert d["violations"]==0, d
assert abs(d["per_layer"]["met1"]["worst_ratio"]-30.0)<1e-6, d
assert abs(d["per_layer"]["met2"]["worst_ratio"]-30.0)<1e-6, d
assert "caa" not in d, "CAA must be OFF when no cumulative_ratio configured"
print("  [5a] per-layer check PASSES the design (met1=met2=30.0 < 40) OK")
PY

# 5b: CAA FLAGS the SAME design (per-layer still clean; cumulative 60.18 > 50)
ANT_GDS="$WORK/caa.gds" ANT_CONFIG="$CFG_CAA" ANT_OUT="$WORK/caa_caa.json" \
  run_kl "$TOOL/antenna_check.py" >/dev/null
python3 - "$WORK/caa_caa.json" <<'PY' || exit 1
import json,sys
d=json.load(open(sys.argv[1]))
assert d["verdict"]=="FAIL", d
# per-layer is STILL clean -> the failure is purely cumulative (CAA != per-layer)
assert d["violations"]==0, ("per-layer must remain clean", d)
caa=d["caa"]
assert caa["enabled"] is True and caa["cumulative_violations"]==1, d
cc=d["per_layer"]["met2"]["cumulative_check"]
assert cc["violations"]==1, d
assert abs(cc["worst_ratio"]-60.18)<1e-3, f"CAA ratio {cc['worst_ratio']} != hand-computed 60.18"
comp=cc["detail"][0]["components"]
# hand-computed component areas: cont 0.09 + met1 30.0 + via1 0.09 + met2 30.0 = 60.18
assert abs(comp["met1"]-30.0)<1e-3 and abs(comp["met2"]-30.0)<1e-3, comp
assert abs(comp["via1"]-0.09)<1e-3 and abs(comp["cont"]-0.09)<1e-3, comp
assert abs(sum(comp.values())-60.18)<1e-3, comp
print("  [5b] CAA FLAGS it: per-layer clean, cumulative 60.18 > 50 (hand-computed) OK")
PY

echo "PASS run_antenna_tests (6/6)"
