#!/usr/bin/env bash
# run_fill_dummy_datatype_test.sh — UNFAKEABLE gate for the DUMMY-DATATYPE fill engine.
#
# `run_fill_tests.sh` exercises the SHARED-datatype path only (dummy fill on the metal's
# own datatype). Every capability the engine grew after that — a dedicated dummy
# datatype, a separate dummy-to-circuit spacing, manufacturing-grid snapping, the
# whole-die density window, and the pruning of fill cells that were never placed —
# is invisible to it: the shared-datatype test passes byte-for-byte against an engine
# that has NONE of them. That is how this file rotted three commits behind its own
# vendored copy without a single test going red.
#
# Proves, against the fork's real KLayout engine (no mocks):
#   1. `window_um: null` measures ONE whole-die window == the foundry coverage rule
#      (worst-window density == global density, exactly);
#   2. dummy fill on a SEPARATE datatype reaches the density target — the engine
#      measures drawn UNION dummy, not drawn alone;
#   3. the fill is LVS-INVISIBLE: the DRAWN layer's shape count is unchanged;
#   4. every dummy-fill edge lands on the declared manufacturing grid;
#   5. `space_to_metal` is honoured: no dummy within space_to_metal of circuit metal
#      + NEGATIVE control that the proximity probe fires at all;
#   6. fill cells that were never placed are PRUNED, so the streamed GDS has exactly
#      ONE top cell (a second root makes a sign-off deck refuse the file outright);
#   7. NEGATIVE control: an infeasible target reports `reached:false` — so [2] is a
#      real pass, not a flag that is always true.
#
# Skips (exit 0) if no KLayout binary is on PATH (honest-skip convention).
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
if [ -z "$KL" ]; then
  echo "SKIP run_fill_dummy_datatype_test: no KLayout binary (strmrun/klayout) on PATH"
  exit 0
fi
run_kl() { $KL $RFLAG "$1" 2>&1 | grep -vE 'tlXMLParser|pcell_declaration'; }
fail() { echo "FAIL: $1"; exit 1; }

# --- fixture: sparse die --------------------------------------------------
FILL_OUT="$WORK/sparse.gds" run_kl "$TOOL/gen_fixtures.py" >/dev/null
[ -s "$WORK/sparse.gds" ] || fail "sparse fixture not generated"

# Dummy fill on datatype 4, whole-die window, 5nm manufacturing grid, and a
# dummy-to-circuit spacing (0.40) WIDER than the dummy-to-dummy spacing (0.20).
cat > "$WORK/dummy.json" <<'JSON'
{"boundary_layer":[0,0],"window_um":null,"max_passes":8,"mfg_grid_um":0.005,
 "fill_datatype":null,
 "layers":[{"name":"metal1","layer":[34,0],"target":0.30,"max":0.95,
            "space":0.20,"space_to_metal":0.40,"width":1.0,"fill_datatype":4}]}
JSON

FILL_GDS="$WORK/sparse.gds" FILL_CONFIG="$WORK/dummy.json" FILL_OUT="$WORK/dummy.gds" \
  FILL_REPORT="$WORK/dummy_report.json" run_kl "$TOOL/metal_fill.py" >/dev/null
[ -s "$WORK/dummy_report.json" ] || fail "engine produced no report for the dummy-datatype config (does it understand window_um:null / per-layer fill_datatype?)"
[ -s "$WORK/dummy.gds" ] || fail "engine produced no filled GDS"

python3 - "$WORK/dummy_report.json" <<'PY' || exit 1
import json, sys
d = json.load(open(sys.argv[1]))
m = d["layers"][0]
for k in ("space_to_metal_um", "min_width_um", "top_width_um", "fill_sizes"):
    assert k in m, f"[2] report has no {k!r} -> engine predates the dummy-datatype rewrite: {m}"
assert "unplaced_fill_cells_pruned" in d, f"[6] report has no 'unplaced_fill_cells_pruned': {d.keys()}"
assert d.get("mfg_grid_um") == 0.005, f"[4] mfg_grid_um not carried into the report: {d.get('mfg_grid_um')}"
assert m["space_to_metal_um"] == 0.40, f"[5] space_to_metal not honoured: {m}"
assert m["fill_datatype"] == 4, f"[2] per-layer fill_datatype ignored: {m}"

# [1] whole-die window: one window means worst-window == global, exactly.
assert d["window_um"] is None, d["window_um"]
assert m["worst_window_after"] == m["density_after"], (
    f"[1] window_um:null did not collapse to a single whole-die window: "
    f"worst {m['worst_window_after']} vs global {m['density_after']}")

# [2] the engine measured drawn UNION dummy and reached the target.
assert m["worst_window_before"] < m["target"], m
assert m["reached"], f"[2] dummy-datatype fill did not reach target: {m}"
assert m["density_after"] > m["density_before"], m
assert not m["over_max"], f"[2] fill exceeded max density: {m}"
assert m["fill_shapes"] > 0, f"[2] no fill shapes reported: {m}"
print(f"  [1] whole-die window: worst-window == global == {m['density_after']} OK")
print(f"  [2] dummy(dt=4) fill reached target: {m['worst_window_before']} -> "
      f"{m['worst_window_after']} (target {m['target']}), {m['fill_shapes']} shapes OK")
PY

# [3,4,5] geometry assertions on the STREAMED GDS, not on the report.
DUMMY_BEFORE="$WORK/sparse.gds" DUMMY_AFTER="$WORK/dummy.gds" \
  DUMMY_REPORT="$WORK/dummy_report.json" run_kl "$HERE/check_dummy_geometry.py" \
  > "$WORK/geom.log" 2>&1
grep -q '^GEOM-OK' "$WORK/geom.log" || { cat "$WORK/geom.log"; fail "dummy-fill geometry checks (3/4/5/6) failed"; }
sed -n 's/^  \[/  [/p' "$WORK/geom.log"

# --- 6. a rung that fits NOWHERE must be pruned, not left as a second top cell ---
# top width 40um cannot fit in the fixture's ~13um channels, so the first ladder
# rungs place nothing; their fill cells must not survive into the stream.
cat > "$WORK/oversize.json" <<'JSON'
{"boundary_layer":[0,0],"window_um":null,"max_passes":8,"mfg_grid_um":0.005,
 "layers":[{"name":"metal1","layer":[34,0],"target":0.30,"max":0.99,
            "space":0.20,"space_to_metal":0.40,"width":40.0,"fill_datatype":4}]}
JSON
FILL_GDS="$WORK/sparse.gds" FILL_CONFIG="$WORK/oversize.json" \
  FILL_OUT="$WORK/oversize.gds" FILL_REPORT="$WORK/oversize_report.json" \
  run_kl "$TOOL/metal_fill.py" >/dev/null
[ -s "$WORK/oversize.gds" ] || fail "[6] oversize run produced no GDS"
python3 - "$WORK/oversize_report.json" <<'PY' || exit 1
import json, sys
d = json.load(open(sys.argv[1]))
pruned = d.get("unplaced_fill_cells_pruned")
assert pruned, ("[6] no fill cell was pruned -- the oversize fixture no longer creates "
                f"an unplaceable rung, so the prune assertion is vacuous: {d.get('layers')}")
print(f"  [6] unplaced fill cells pruned: {pruned}")
PY
DUMMY_GDS="$WORK/oversize.gds" run_kl "$HERE/check_single_top.py" > "$WORK/top.log" 2>&1
grep -q '^TOP-OK' "$WORK/top.log" || { cat "$WORK/top.log"; fail "[6] streamed GDS has more than one top cell"; }
sed -n 's/^  \[/  [/p' "$WORK/top.log"

# --- 7. negative control: an infeasible target must report reached:false ----
cat > "$WORK/infeasible.json" <<'JSON'
{"boundary_layer":[0,0],"window_um":null,"max_passes":4,"mfg_grid_um":0.005,
 "layers":[{"name":"metal1","layer":[34,0],"target":0.99,"max":1.0,
            "space":2.0,"space_to_metal":2.0,"width":0.5,"fill_datatype":4}]}
JSON
FILL_GDS="$WORK/sparse.gds" FILL_CONFIG="$WORK/infeasible.json" \
  FILL_OUT="$WORK/infeasible.gds" FILL_REPORT="$WORK/infeasible_report.json" \
  run_kl "$TOOL/metal_fill.py" >/dev/null
python3 - "$WORK/infeasible_report.json" <<'PY' || exit 1
import json, sys
d = json.load(open(sys.argv[1]))
m = d["layers"][0]
assert not m["reached"], f"[7] an infeasible 0.99 target reported reached:true -> the flag is vacuous: {m}"
assert d["verdict"] == "PARTIAL", f"[7] verdict should be PARTIAL, got {d['verdict']}"
print(f"  [7] negative control: infeasible target honestly reached:false "
      f"(worst {m['worst_window_after']} < {m['target']}) OK")
PY

echo "PASS run_fill_dummy_datatype_test (7/7)"
