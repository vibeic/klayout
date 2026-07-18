#!/usr/bin/env bash
# run_fill_tests.sh — UNFAKEABLE gate for the per-layer density metal-fill utility.
#
# Proves, against the fork's real KLayout fill engine + `svrfdrc` DRC engine (no mocks):
#   1. a sparse layer (~4% density) below the 30% target is RAISED to >= target
#      (worst-window density after >= target), i.e. the fill actually fixes density;
#   2. the correctly-filled GDS has 0 new spacing AND 0 new width DRC violations
#      (svrfdrc on the fork's own engine, before == after == 0);
#   3. NEGATIVE control: a deliberately-too-tight fill (0.05um spacing) IS caught by
#      the same deck (SPACE.M1 > 0) — so [2] is a real pass, not a vacuous one.
#
# Skips (exit 0) if no KLayout / svrfdrc binary is on PATH (honest-skip convention).
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
TOOL="$HERE/.."
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

KL=""
if command -v strmrun >/dev/null 2>&1; then KL="strmrun"; RFLAG="";
elif command -v klayout >/dev/null 2>&1; then KL="klayout"; RFLAG="-b -r";
else
  for c in /home/reyerchu/vibe-ic-forks/klayout-build/*/strmrun; do
    [ -x "$c" ] && { KL="$c"; RFLAG=""; break; }
  done
fi
SVRF=""
if command -v svrfdrc >/dev/null 2>&1; then SVRF="svrfdrc";
else
  for c in /home/reyerchu/vibe-ic-forks/klayout-build/*/svrfdrc; do
    [ -x "$c" ] && { SVRF="$c"; break; }
  done
fi
if [ -z "$KL" ]; then
  echo "SKIP run_fill_tests: no KLayout binary (strmrun/klayout) on PATH"; exit 0
fi
run_kl() { $KL $RFLAG "$1" 2>&1 | grep -vE 'tlXMLParser|pcell_declaration'; }
fail() { echo "FAIL: $1"; exit 1; }

CFG="$TOOL/fill_config.example.json"
DECK="$HERE/fill_drc.rule"

# --- fixture: sparse die --------------------------------------------------
FILL_OUT="$WORK/sparse.gds" run_kl "$TOOL/gen_fixtures.py" >/dev/null
[ -s "$WORK/sparse.gds" ] || fail "sparse fixture not generated"

# --- 1. fill raises worst-window density to >= target --------------------
FILL_GDS="$WORK/sparse.gds" FILL_CONFIG="$CFG" FILL_OUT="$WORK/filled.gds" \
  FILL_REPORT="$WORK/fill.json" run_kl "$TOOL/metal_fill.py" >/dev/null
python3 - "$WORK/fill.json" <<'PY' || exit 1
import json,sys
d=json.load(open(sys.argv[1]))
m=d["layers"][0]
assert m["worst_window_before"] < m["target"], m
assert m["reached"], f"worst_window_after {m['worst_window_after']} < target {m['target']}: {m}"
assert m["density_after"] > m["density_before"], m
assert not m["over_max"], f"fill exceeded max density: {m}"
print(f"  [1] density raised: worst-window {m['worst_window_before']} -> "
      f"{m['worst_window_after']} (target {m['target']}); "
      f"global {m['density_before']} -> {m['density_after']} OK")
PY

# --- 2 + 3 need the DRC engine -------------------------------------------
if [ -z "$SVRF" ]; then
  echo "  [2,3] SKIP DRC checks: no svrfdrc binary on PATH"
  echo "PASS run_fill_tests (1/1 density; DRC skipped)"
  exit 0
fi
drc_count() {  # $1=gds -> prints "space width"
  $SVRF "$DECK" "$1" "$WORK/drc.rpt" --cell SP >/dev/null 2>&1
  s=$(grep 'SPACE.M1' "$WORK/drc.rpt" | grep -oE '> [0-9]+$' | grep -oE '[0-9]+')
  w=$(grep 'WIDTH.M1' "$WORK/drc.rpt" | grep -oE '> [0-9]+$' | grep -oE '[0-9]+')
  echo "${s:-NA} ${w:-NA}"
}
read sb wb < <(drc_count "$WORK/sparse.gds")
read sa wa < <(drc_count "$WORK/filled.gds")
[ "$sb" = "0" ] && [ "$wb" = "0" ] || fail "pre-fill fixture not DRC-clean ($sb/$wb)"
[ "$sa" = "0" ] && [ "$wa" = "0" ] || fail "FILLED GDS has new DRC: space=$sa width=$wa"
echo "  [2] filled GDS DRC-clean: space $sb->$sa, width $wb->$wa (0 new) OK"

# --- 3. negative control: too-tight fill must be CAUGHT ------------------
cat > "$WORK/tight.json" <<JSON
{"boundary_layer":[0,0],"window_um":20.0,"max_passes":3,"fill_datatype":null,
 "layers":[{"name":"met1","layer":[34,0],"target":0.90,"max":0.99,"space":0.05,"width":0.50}]}
JSON
FILL_GDS="$WORK/sparse.gds" FILL_CONFIG="$WORK/tight.json" FILL_OUT="$WORK/tight.gds" \
  FILL_REPORT="$WORK/tightrep.json" run_kl "$TOOL/metal_fill.py" >/dev/null
read st wt < <(drc_count "$WORK/tight.gds")
if [ "$st" = "0" ] || [ "$st" = "NA" ]; then
  fail "negative control: too-tight fill produced $st spacing violations (deck vacuous?)"
fi
echo "  [3] negative control: too-tight (0.05um) fill CAUGHT ($st spacing violations) OK"

echo "PASS run_fill_tests (3/3)"
