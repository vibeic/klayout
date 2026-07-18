#!/usr/bin/env bash
# run_waiver_engine_test.sh -- UNFAKEABLE proven-negative gate for automated
# waiver management (fork feature #10): geometry-anchored suppression of
# pre-approved DRC markers, evaluated DIRECTLY inside db::SVRFEngine.
#
# Two min-area violations sit at known coordinates:
#   A: marker bbox [0,0,0.5,0.5] um     B: marker bbox [10,10,10.6,10.6] um
# Proves, against the fork's real native C++ engine:
#   1. NO waiver file          -> MINAREA FAIL 2 (both flag);
#   2. waiver box that FULLY CONTAINS A -> FAIL 1 (only A waived) + audit trail
#      records A's marker bbox and the waiver box;
#   3. PROVEN-NEGATIVE (wrong coords)   -> FAIL 2 (a far-away box suppresses nothing);
#   4. PROVEN-NEGATIVE (partial overlap) -> FAIL 2 (a box that only clips A does
#      NOT suppress it -- suppression requires FULL containment);
#   5. PROVEN-NEGATIVE (rule-name mismatch) -> FAIL 2 (a waiver keyed to another
#      rule never applies to MINAREA); wildcard "*" DOES apply -> FAIL 1;
#   6. waiver covering BOTH markers -> FAIL 0 (MINAREA PASS) -- full suppression;
#   7. the 6 frozen engine goldens stay byte-identical (0 regressions, env unset).
#
# NO vendor data. Skips (exit 0) when the db build inputs or a container are absent.
# Env: KLAYOUT_SRC/BLD/BIN + EDA_IMAGE (default vibeic/vibeic-eda:0.2.18)
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
SVRF="$(cd "$HERE/.." && pwd)"
ROOT="$(cd "$SVRF/.." && pwd)"
DBP="$ROOT/src/plugins/tools/svrf_drc/db_plugin"
RULE="$SVRF/examples/waiver.rule"
KSRC="${KLAYOUT_SRC:-$HOME/kbuild}"
KBLD="${KLAYOUT_BLD:-$HOME/kbuild-out/bld}"
KBIN="${KLAYOUT_BIN:-$HOME/kbuild-out/bin}"
IMAGE="${EDA_IMAGE:-vibeic/vibeic-eda:0.2.18}"

if [ ! -d "$KSRC/src/db/db" ] || [ ! -f "$KBIN/libklayout_db.so" ]; then
  echo "SKIP run_waiver_engine_test: no KLayout db build (set KLAYOUT_SRC/BLD/BIN)"; exit 0
fi
if ! command -v docker >/dev/null 2>&1; then
  echo "SKIP run_waiver_engine_test: no docker for GDS generation"; exit 0
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

echo "== generate synthetic fixture (container klayout pya) =="
cp "$HERE/gen_waiver_gds.py" "$WORK/"
docker run --rm --entrypoint bash -v "$WORK":/work "$IMAGE" -lc "
  export PATH=/foss/tools/klayout:\$PATH
  WV_OUT=/work/waiver.gds klayout -b -r /work/gen_waiver_gds.py >/dev/null 2>&1
"
[ -s "$WORK/waiver.gds" ] || fail "fixture generation produced no GDS"

# run the engine, optionally with a waiver file; echo the MINAREA count
count() { grep -oE '^(PASS|FAIL) +MINAREA .* -> [0-9]+$' "$1" | grep -oE '[0-9]+$'; }
runwv() { # runwv <waiver-file-or-empty> <report> <audit>
  if [ -n "$1" ]; then export SVRFDRC_WAIVERS="$1"; else unset SVRFDRC_WAIVERS; fi
  SVRFDRC_WAIVER_AUDIT="$3" "$BIN" "$RULE" "$WORK/waiver.gds" "$2" "KLayout 0.30.9" 2>/dev/null
  unset SVRFDRC_WAIVERS
}

echo "== 1. no waiver -> FAIL 2 =="
runwv "" "$WORK/r0.rpt" "$WORK/a0.txt"
[ "$(count "$WORK/r0.rpt")" = "2" ] || { cat "$WORK/r0.rpt"; fail "no-waiver count != 2"; }
echo "  [1] $(grep -E '^FAIL +MINAREA' "$WORK/r0.rpt")"

echo "== 2. waiver fully containing A -> FAIL 1 + audit =="
printf 'MINAREA -0.1 -0.1 0.6 0.6\n' > "$WORK/wA.txt"
runwv "$WORK/wA.txt" "$WORK/r1.rpt" "$WORK/a1.txt"
[ "$(count "$WORK/r1.rpt")" = "1" ] || { cat "$WORK/r1.rpt"; fail "A-waiver count != 1"; }
grep -qE 'WAIVED rule=MINAREA marker_bbox_um=\[0.0000,0.0000,0.5000,0.5000\]' "$WORK/a1.txt" || { cat "$WORK/a1.txt"; fail "audit trail missing A's marker bbox"; }
echo "  [2] count 1; audit: $(cat "$WORK/a1.txt")"

echo "== 3. PROVEN-NEGATIVE wrong coordinates -> FAIL 2 =="
printf 'MINAREA 100 100 101 101\n' > "$WORK/wWrong.txt"
runwv "$WORK/wWrong.txt" "$WORK/r2.rpt" "$WORK/a2.txt"
[ "$(count "$WORK/r2.rpt")" = "2" ] || { cat "$WORK/r2.rpt"; fail "far-away waiver must NOT suppress anything (count != 2)"; }
echo "  [3] far-away waiver box suppresses nothing (count 2) OK"

echo "== 4. PROVEN-NEGATIVE partial overlap -> FAIL 2 =="
# box [0.2,0.2,0.6,0.6] clips A ([0,0,0.5,0.5]) but does NOT fully contain it
printf 'MINAREA 0.2 0.2 0.6 0.6\n' > "$WORK/wPart.txt"
runwv "$WORK/wPart.txt" "$WORK/r3.rpt" "$WORK/a3.txt"
[ "$(count "$WORK/r3.rpt")" = "2" ] || { cat "$WORK/r3.rpt"; fail "partial-overlap waiver must NOT suppress A (count != 2)"; }
echo "  [4] partial-overlap waiver does not hide the violation (count 2) OK"

echo "== 5. rule-name mismatch -> FAIL 2; wildcard '*' -> FAIL 1 =="
printf 'OTHERRULE -0.1 -0.1 0.6 0.6\n' > "$WORK/wMiss.txt"
runwv "$WORK/wMiss.txt" "$WORK/r4.rpt" "$WORK/a4.txt"
[ "$(count "$WORK/r4.rpt")" = "2" ] || { cat "$WORK/r4.rpt"; fail "waiver keyed to another rule must NOT apply (count != 2)"; }
printf '* -0.1 -0.1 0.6 0.6\n' > "$WORK/wStar.txt"
runwv "$WORK/wStar.txt" "$WORK/r5.rpt" "$WORK/a5.txt"
[ "$(count "$WORK/r5.rpt")" = "1" ] || { cat "$WORK/r5.rpt"; fail "wildcard waiver should suppress A (count != 1)"; }
echo "  [5] rule-mismatch keeps 2; wildcard suppresses -> 1 OK"

echo "== 6. waiver covering BOTH markers -> FAIL 0 (MINAREA PASS) =="
printf 'MINAREA -1 -1 11 11\n' > "$WORK/wBoth.txt"
runwv "$WORK/wBoth.txt" "$WORK/r6.rpt" "$WORK/a6.txt"
[ "$(count "$WORK/r6.rpt")" = "0" ] || { cat "$WORK/r6.rpt"; fail "both-covering waiver should suppress all (count != 0)"; }
grep -qE '^PASS +MINAREA' "$WORK/r6.rpt" || fail "MINAREA should PASS when both markers are waived"
echo "  [6] both waived -> $(grep -E '^PASS +MINAREA' "$WORK/r6.rpt")"

echo "== 7. 0 regressions: 6 frozen goldens byte-identical (env unset) =="
EDA_IMAGE="$IMAGE" bash "$HERE/run_engine_parity.sh" >/dev/null 2>&1 || fail "engine parity regression FAILED"
echo "  [7] run_engine_parity.sh: all frozen goldens byte-identical"

echo "PASS run_waiver_engine_test (7/7)"
