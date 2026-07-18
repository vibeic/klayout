#!/usr/bin/env bash
# run_dfm_engine_test.sh -- UNFAKEABLE gate for DFM scoring / recommended (soft)
# rules (fork feature #47): a weighted aggregate over soft-rule violations,
# computed by db::SVRFEngine from the SAME real per-rule violation counts.
#
# Fixture: 3 slivers (SR_SLIVER, ratio 88.2) + 2 tiny squares (SR_MINAREA,
# 0.25 um^2). Proves, against the fork's real native C++ engine:
#   1. with weights SR_SLIVER=2.0, SR_MINAREA=0.5 the DFM score is the HAND-COMPUTED
#      2.0*3 + 0.5*2 = 7.0000 (soft_rules=2, total_viol=5); per-rule contribs
#      6.0000 and 1.0000 are reported;
#   2. the UNWEIGHTED HARD_SP rule contributes 0 (absent from the DFM detail,
#      soft_rules stays 2 -- not 3);
#   3. PROVEN-NEGATIVE: reweighting SR_MINAREA to 1.0 changes the score to the
#      hand-computed 2.0*3 + 1.0*2 = 8.0000 (score tracks the weights, not a const);
#   4. env unset -> NO DFM output + the per-rule report is unchanged (parity);
#   5. the 6 frozen engine goldens stay byte-identical (0 regressions).
#
# NO vendor data. Skips (exit 0) when the db build inputs or a container are absent.
# Env: KLAYOUT_SRC/BLD/BIN + EDA_IMAGE (default vibeic/vibeic-eda:0.2.18)
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
SVRF="$(cd "$HERE/.." && pwd)"
ROOT="$(cd "$SVRF/.." && pwd)"
DBP="$ROOT/src/plugins/tools/svrf_drc/db_plugin"
RULE="$SVRF/examples/dfm.rule"
KSRC="${KLAYOUT_SRC:-$HOME/kbuild}"
KBLD="${KLAYOUT_BLD:-$HOME/kbuild-out/bld}"
KBIN="${KLAYOUT_BIN:-$HOME/kbuild-out/bin}"
IMAGE="${EDA_IMAGE:-vibeic/vibeic-eda:0.2.18}"

if [ ! -d "$KSRC/src/db/db" ] || [ ! -f "$KBIN/libklayout_db.so" ]; then
  echo "SKIP run_dfm_engine_test: no KLayout db build (set KLAYOUT_SRC/BLD/BIN)"; exit 0
fi
if ! command -v docker >/dev/null 2>&1; then
  echo "SKIP run_dfm_engine_test: no docker for GDS generation"; exit 0
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
cp "$HERE/gen_dfm_gds.py" "$WORK/"
docker run --rm --entrypoint bash -v "$WORK":/work "$IMAGE" -lc "
  export PATH=/foss/tools/klayout:\$PATH
  DFM_GDS=/work/dfm.gds klayout -b -r /work/gen_dfm_gds.py >/dev/null 2>&1
"
[ -s "$WORK/dfm.gds" ] || fail "fixture generation produced no GDS"

echo "== 1. weighted score == hand-computed 7.0 (soft_rules=2, total_viol=5) =="
printf 'SR_SLIVER 2.0\nSR_MINAREA 0.5\n' > "$WORK/w1.txt"
SVRFDRC_DFM_WEIGHTS="$WORK/w1.txt" SVRFDRC_DFM_OUT="$WORK/dfm1.txt" \
  "$BIN" "$RULE" "$WORK/dfm.gds" "$WORK/r1.rpt" "KLayout 0.30.9" 2>/dev/null
grep -qE '^DFM score=7\.0000 soft_rules=2 total_viol=5$' "$WORK/dfm1.txt" || { cat "$WORK/dfm1.txt"; fail "DFM score != hand-computed 7.0"; }
grep -qE '^DFM soft rule=SR_SLIVER weight=2\.0000 viol=3 contrib=6\.0000$'  "$WORK/dfm1.txt" || { cat "$WORK/dfm1.txt"; fail "SR_SLIVER contrib wrong"; }
grep -qE '^DFM soft rule=SR_MINAREA weight=0\.5000 viol=2 contrib=1\.0000$' "$WORK/dfm1.txt" || { cat "$WORK/dfm1.txt"; fail "SR_MINAREA contrib wrong"; }
echo "  [1] $(grep '^DFM score' "$WORK/dfm1.txt")"

echo "== 2. unweighted HARD_SP contributes 0 (absent from DFM detail) =="
grep -q 'HARD_SP' "$WORK/dfm1.txt" && { cat "$WORK/dfm1.txt"; fail "unweighted HARD_SP must NOT enter the DFM score"; }
echo "  [2] HARD_SP excluded from the weighted score OK"

echo "== 3. PROVEN-NEGATIVE: reweight SR_MINAREA to 1.0 -> score 8.0 =="
printf 'SR_SLIVER 2.0\nSR_MINAREA 1.0\n' > "$WORK/w2.txt"
SVRFDRC_DFM_WEIGHTS="$WORK/w2.txt" SVRFDRC_DFM_OUT="$WORK/dfm2.txt" \
  "$BIN" "$RULE" "$WORK/dfm.gds" "$WORK/r2.rpt" "KLayout 0.30.9" 2>/dev/null
grep -qE '^DFM score=8\.0000 soft_rules=2 total_viol=5$' "$WORK/dfm2.txt" || { cat "$WORK/dfm2.txt"; fail "reweighted score != hand-computed 8.0"; }
echo "  [3] reweight -> $(grep '^DFM score' "$WORK/dfm2.txt")"

echo "== 4. env unset: no DFM output + per-rule report unchanged (parity) =="
"$BIN" "$RULE" "$WORK/dfm.gds" "$WORK/r0.rpt" "KLayout 0.30.9" 2> "$WORK/r0.err"
grep -q 'DFM score' "$WORK/r0.err" && fail "DFM output leaked with env unset (parity broken)"
diff "$WORK/r1.rpt" "$WORK/r0.rpt" >/dev/null || fail "per-rule report differs weighted-vs-unweighted (DFM must not move verdicts)"
echo "  [4] no DFM emitted; per-rule report byte-identical weighted-vs-unweighted OK"

echo "== 5. 0 regressions: 6 frozen goldens byte-identical =="
EDA_IMAGE="$IMAGE" bash "$HERE/run_engine_parity.sh" >/dev/null 2>&1 || fail "engine parity regression FAILED"
echo "  [5] run_engine_parity.sh: all frozen goldens byte-identical"

echo "PASS run_dfm_engine_test (5/5)"
