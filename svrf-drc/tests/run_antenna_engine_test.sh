#!/usr/bin/env bash
# run_antenna_engine_test.sh -- UNFAKEABLE gate for the NATIVE in-engine ANTENNA
# op (fork feature #20): the charge-ratio SVRF operator evaluated DIRECTLY on
# db::SVRFEngine / db::LayoutToNetlist, not routed to a separate tool.
#
# Proves, against the fork's real native C++ engine (no mocks, no Python interpreter):
#   1. a fabricated met1 antenna VIOLATION (met1 area / gate area = 50, limit 40)
#      is FLAGGED by the engine (report line FAIL ANT.M1) and the reported ratio
#      equals the HAND-COMPUTED value (50.0);
#   2. a CLEAN structure (met1 ratio 10 < 40) PASSes (ratio == 10.0);
#   3. the STAGED model: a met2 jumper deposited AFTER the met1 etch does NOT relieve
#      the met1-stage antenna (ANT.M1 stays FAIL @ ratio 50), while at the met2 stage
#      the full node makes ANT.M2 FAIL @ ratio 100 (hand-computed);
#   4. the one-layer ANTENNA form (no gate) still honest-SKIPs (routed out);
#   5. cross-check: the native engine's per-metal verdict AGREES with the independent
#      standalone gds-antenna Python checker on the same GDS (clean/dirty agreement).
#
# NO vendor data anywhere. Skips (exit 0, SKIP banner) when the KLayout db build
# inputs or a KLayout `pya` runner are absent -- matches the plugin's honest-skip
# convention so CI without the fork build stays green.
#
# Env (override as needed; same names as run_engine_parity.sh):
#   KLAYOUT_SRC   klayout source tree    (default ~/kbuild)
#   KLAYOUT_BLD   klayout build dir      (default ~/kbuild-out/bld)
#   KLAYOUT_BIN   klayout installed libs (default ~/kbuild-out/bin; also holds strmrun)
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
SVRF="$(cd "$HERE/.." && pwd)"                       # svrf-drc/
ROOT="$(cd "$SVRF/.." && pwd)"                       # fork root
DBP="$ROOT/src/plugins/tools/svrf_drc/db_plugin"
GEN="$ROOT/gds-antenna/gen_fixtures.py"              # hand-computable fixture generator
ACHK="$ROOT/gds-antenna/antenna_check.py"           # independent standalone checker (xcheck)
RULE="$SVRF/examples/antenna.rule"

KSRC="${KLAYOUT_SRC:-$HOME/kbuild}"
KBLD="${KLAYOUT_BLD:-$HOME/kbuild-out/bld}"
KBIN="${KLAYOUT_BIN:-$HOME/kbuild-out/bin}"

# --- locate a KLayout batch runner (strmrun / klayout -b -r) for the GDS builders --
KL=""; RFLAG=""
if command -v strmrun >/dev/null 2>&1; then KL="strmrun";
elif [ -x "$KBIN/strmrun" ]; then KL="$KBIN/strmrun";
elif command -v klayout >/dev/null 2>&1; then KL="klayout"; RFLAG="-b -r";
else
  for c in "$KBIN/strmrun" /home/reyerchu/vibe-ic-forks/klayout-build/*/strmrun; do
    [ -x "$c" ] && { KL="$c"; break; }
  done
fi
[ -z "$KL" ] && { echo "SKIP run_antenna_engine_test: no KLayout runner (strmrun/klayout)"; exit 0; }

# --- require the db build inputs to compile engine_smoke --------------------------
if [ ! -d "$KSRC/src/db/db" ] || [ ! -f "$KBIN/libklayout_db.so" ]; then
  echo "SKIP run_antenna_engine_test: no KLayout db build (set KLAYOUT_SRC/BLD/BIN)"; exit 0
fi

WORK="$(mktemp -d)"; trap 'rm -rf "$WORK"' EXIT
export LD_LIBRARY_PATH="$KBIN:$KBIN/db_plugins:${LD_LIBRARY_PATH:-}"
fail() { echo "FAIL: $1"; exit 1; }
run_kl() { $KL $RFLAG "$1" 2>&1 | grep -vE 'tlXMLParser|pcell_declaration'; }

echo "== build engine_smoke =="
BIN="$WORK/engine_smoke"
g++ -std=c++17 -O1 -DHAVE_PYTHON \
  -I"$DBP" -I"$KSRC/src/db/db" -I"$KBLD/db/db" -I"$KSRC/src/tl/tl" -I"$KBLD/tl/tl" -I"$KSRC/src/gsi/gsi" \
  "$HERE/engine_smoke.cc" "$DBP/dbSVRFDeck.cc" "$DBP/dbSVRFEngine.cc" \
  -L"$KBIN" -lklayout_db -lklayout_tl -lklayout_gsi \
  -Wl,--no-as-needed -L"$KBIN/db_plugins" -lgds2 -Wl,--as-needed \
  -Wl,-rpath,"$KBIN" -Wl,-rpath,"$KBIN/db_plugins" -o "$BIN" || fail "engine_smoke build failed"

echo "== generate synthetic fixtures =="
ANT_OUT="$WORK/viol.gds"   ANT_LEN=100                   run_kl "$GEN" >/dev/null  # met1 ratio 50
ANT_OUT="$WORK/clean.gds"  ANT_LEN=20                    run_kl "$GEN" >/dev/null  # met1 ratio 10
ANT_OUT="$WORK/staged.gds" ANT_LEN=100 ANT_MET2_LEN=200  run_kl "$GEN" >/dev/null  # met2 jumper
[ -s "$WORK/viol.gds" ] && [ -s "$WORK/clean.gds" ] && [ -s "$WORK/staged.gds" ] \
  || fail "fixture generation produced no GDS"

# run the native engine, capturing both the frozen report and the ratio diagnostic
eng() { # eng <gds> <rpt> -> writes rpt, prints ANTENNA_RATIO lines
  SVRFDRC_ANTENNA_RATIO=1 "$BIN" "$RULE" "$1" "$2" "KLayout 0.30.9" 2>&1 \
    | grep 'ANTENNA_RATIO'
}
# worst ratio for a named antenna rule from the diagnostic stream
worst() { awk -v n="$2" '$2==n{for(i=1;i<=NF;i++)if($i~"^worst="){sub("worst=","",$i);print $i}}' "$1"; }

echo "== 1. violation: ANT.M1 FAIL, ratio == 50.0 (hand-computed) =="
eng "$WORK/viol.gds" "$WORK/viol.rpt" > "$WORK/viol.diag"
grep -qE '^FAIL +ANT\.M1 +ANTENNA met1/gate' "$WORK/viol.rpt" || fail "ANT.M1 not FLAGGED on the violation fixture"
grep -qE '^PASS +ANT\.M2 ' "$WORK/viol.rpt" || fail "ANT.M2 should PASS (no met2 present)"
w=$(worst "$WORK/viol.diag" ANT.M1)
awk -v w="$w" 'BEGIN{exit !(w>49.999 && w<50.001)}' || fail "ANT.M1 ratio $w != hand-computed 50.0"
echo "  [1] ANT.M1 FAIL; reported ratio $w == 50.0 (hand-computed) OK"

echo "== 2. clean: ANT.M1 PASS, ratio == 10.0 =="
eng "$WORK/clean.gds" "$WORK/clean.rpt" > "$WORK/clean.diag"
grep -qE '^PASS +ANT\.M1 ' "$WORK/clean.rpt" || fail "ANT.M1 should PASS on the clean fixture"
w=$(worst "$WORK/clean.diag" ANT.M1)
awk -v w="$w" 'BEGIN{exit !(w>9.999 && w<10.001)}' || fail "ANT.M1 clean ratio $w != 10.0"
echo "  [2] ANT.M1 PASS; reported ratio $w == 10.0 (hand-computed) OK"

echo "== 3. staged: met2 jumper does NOT relieve the met1 stage =="
eng "$WORK/staged.gds" "$WORK/staged.rpt" > "$WORK/staged.diag"
grep -qE '^FAIL +ANT\.M1 ' "$WORK/staged.rpt" || fail "ANT.M1 must stay FAIL despite the met2 jumper"
w1=$(worst "$WORK/staged.diag" ANT.M1); w2=$(worst "$WORK/staged.diag" ANT.M2)
awk -v w="$w1" 'BEGIN{exit !(w>49.999 && w<50.001)}' || fail "staged ANT.M1 ratio $w1 != 50.0 (upper jumper leaked into the met1 stage!)"
awk -v w="$w2" 'BEGIN{exit !(w>99.999 && w<100.001)}' || fail "staged ANT.M2 ratio $w2 != 100.0"
echo "  [3] staged: met1-stage ratio stays $w1 (jumper ignored); met2-stage ratio $w2 OK"

echo "== 4. one-layer ANTENNA (no gate) still honest-SKIPs =="
grep -qE '^SKIP +ANT\.M1\.SIDE +ANTENNA met1 ' "$WORK/viol.rpt" || fail "one-layer ANTENNA must SKIP (routed out)"
echo "  [4] one-layer ANT.M1.SIDE SKIP (routed out, backward-compatible) OK"

echo "== 5. cross-check: native engine AGREES with the standalone Python checker =="
CFG="$ROOT/gds-antenna/antenna_config.example.json"
ANT_GDS="$WORK/viol.gds"  ANT_CONFIG="$CFG" ANT_OUT="$WORK/viol_py.json"  run_kl "$ACHK" >/dev/null
ANT_GDS="$WORK/clean.gds" ANT_CONFIG="$CFG" ANT_OUT="$WORK/clean_py.json" run_kl "$ACHK" >/dev/null
python3 - "$WORK/viol_py.json" "$WORK/clean_py.json" "$WORK/viol.rpt" "$WORK/clean.rpt" <<'PY' || exit 1
import json, re, sys
vpy, cpy, vrpt, crpt = sys.argv[1:5]
def eng_dirty(rpt):  # any ANT.* rule FAIL in the native engine report
    return any(re.match(r'^FAIL\s+ANT\.', ln) for ln in open(rpt))
vd = json.load(open(vpy))["verdict"]; cd = json.load(open(cpy))["verdict"]
assert vd == "FAIL" and eng_dirty(vrpt), ("violation: py=%s eng_dirty=%s" % (vd, eng_dirty(vrpt)))
assert cd == "PASS" and not eng_dirty(crpt), ("clean: py=%s eng_dirty=%s" % (cd, eng_dirty(crpt)))
print("  [5] engine<->python AGREE: violation both DIRTY, clean both CLEAN OK")
PY

echo "PASS run_antenna_engine_test (5/5)"
