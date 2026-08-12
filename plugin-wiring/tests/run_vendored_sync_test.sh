#!/usr/bin/env bash
# run_vendored_sync_test.sh — the gate for `vendored_sync_check.py`.
#
# Hermetic: builds a throw-away fork repo + plugin programs dir in $TMPDIR, so it needs
# nothing but python3 and git (no KLayout, no real plugin checkout) and cannot be made
# green by the state of this machine.
#
# A sync check is only worth landing if it has all three states, so each is proven:
#   1. in sync + a TRUE provenance line              -> PASS (rc 0)
#   2. vendored bytes diverge from the fork          -> FAIL (rc 1)
#   3. bytes agree but the provenance line is STALE  -> FAIL (rc 1)
#   4. ... and refreshing that line alone            -> PASS (rc 0)   [3 is satisfiable]
#   5. a vendored file with no fork counterpart      -> FAIL (rc 1)
#   6. a vendored dir with no PROVENANCE.md          -> FAIL (rc 1)
#   7. no plugin checkout at all                     -> HONEST-SKIP (rc 3), never PASS
#   8. an EXPLICIT plugin path that does not exist   -> ERROR (rc 2), never a silent
#                                                       fall-through to another checkout
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
CHECK="$HERE/../vendored_sync_check.py"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

FORK="$WORK/fork"
PLUG="$WORK/plugin/programs"
MAN="$WORK/manifest.json"
fail() { echo "FAIL: $1"; exit 1; }

cat > "$MAN" <<'JSON'
{"vendored_dirs":[{"fork_dir":"eng","plugin_dir":"eng_v"}],
 "ignore":["PROVENANCE.md","__pycache__","*.pyc"]}
JSON

mkdir -p "$FORK/eng" "$PLUG/eng_v"
printf 'v1\n' > "$FORK/eng/engine.py"
printf 'shared\n' > "$FORK/eng/helper.py"
git -C "$FORK" init -q
git -C "$FORK" -c user.email=t@t -c user.name=t add eng >/dev/null
git -C "$FORK" -c user.email=t@t -c user.name=t commit -qm v1
C1="$(git -C "$FORK" rev-parse HEAD)"

vendor() {  # copy the fork dir into the plugin dir and stamp the given sha
  cp "$FORK/eng/"*.py "$PLUG/eng_v/"
  cat > "$PLUG/eng_v/PROVENANCE.md" <<EOF
# Vendored KLayout-fork sign-off engine

* upstream: \`vibeic/klayout\` — \`eng/\`
* upstream commit: $1
EOF
}

run() { python3 "$CHECK" --manifest "$MAN" --fork-root "$FORK" \
                         --plugin-programs "$PLUG" "$@" 2>&1; }
rc_of() { run "$@" >"$WORK/out.txt" 2>&1; echo $?; }

# --- 1. in sync + true provenance -> PASS ---------------------------------
vendor "$C1"
rc=$(rc_of); out=$(cat "$WORK/out.txt")
[ "$rc" = "0" ] || fail "[1] in-sync tree should PASS, got rc=$rc:
$out"
echo "  [1] in sync + true provenance line -> PASS (rc 0) OK"

# --- 2. content divergence -> FAIL ----------------------------------------
printf 'v1-EDITED-DOWNSTREAM\n' > "$PLUG/eng_v/engine.py"
rc=$(rc_of); out=$(cat "$WORK/out.txt")
[ "$rc" = "1" ] || fail "[2] diverged content should FAIL, got rc=$rc:
$out"
echo "$out" | grep -q "DIFFERS" || fail "[2] no DIFFERS in output:
$out"
echo "  [2] vendored bytes != fork bytes -> FAIL (rc 1) OK"

# --- 3. bytes agree, provenance line STALE -> FAIL ------------------------
# fork moves on; the plugin re-vendors the new bytes but forgets the sha line.
printf 'v2\n' > "$FORK/eng/engine.py"
git -C "$FORK" -c user.email=t@t -c user.name=t commit -qam v2
C2="$(git -C "$FORK" rev-parse HEAD)"
vendor "$C1"                      # correct bytes, STALE sha
rc=$(rc_of); out=$(cat "$WORK/out.txt")
[ "$rc" = "1" ] || fail "[3] stale provenance sha should FAIL, got rc=$rc:
$out"
echo "$out" | grep -q "NOT the fork content at that commit" \
  || fail "[3] stale-sha not diagnosed:
$out"
echo "$out" | grep -q "DIFFERS" \
  && fail "[3] reported a content divergence that does not exist:
$out"
echo "$out" | grep -q "upstream commit: $C2" \
  || fail "[3] remedy did not name the correct commit $C2:
$out"
echo "  [3] bytes agree but provenance line stale -> FAIL (rc 1), remedy names ${C2:0:9} OK"

# --- 4. refreshing ONLY that line -> PASS (so [3] is satisfiable) ---------
vendor "$C2"
rc=$(rc_of); out=$(cat "$WORK/out.txt")
[ "$rc" = "0" ] || fail "[4] refreshed provenance line should PASS, got rc=$rc:
$out"
echo "  [4] refreshing the line alone -> PASS (rc 0), so [3] is not permanently red OK"

# --- 5. vendored file with no fork counterpart -> FAIL --------------------
printf 'orphan\n' > "$PLUG/eng_v/only_downstream.py"
rc=$(rc_of); out=$(cat "$WORK/out.txt")
[ "$rc" = "1" ] || fail "[5] orphan vendored file should FAIL, got rc=$rc:
$out"
echo "$out" | grep -q "does not exist in the fork" || fail "[5] orphan not diagnosed:
$out"
rm "$PLUG/eng_v/only_downstream.py"
echo "  [5] vendored file absent from the fork -> FAIL (rc 1) OK"

# --- 6. vendored dir with no PROVENANCE.md -> FAIL ------------------------
mv "$PLUG/eng_v/PROVENANCE.md" "$WORK/prov.bak"
rc=$(rc_of); out=$(cat "$WORK/out.txt")
[ "$rc" = "1" ] || fail "[6] missing PROVENANCE.md should FAIL, got rc=$rc:
$out"
echo "$out" | grep -q "no PROVENANCE.md" || fail "[6] missing provenance not diagnosed:
$out"
mv "$WORK/prov.bak" "$PLUG/eng_v/PROVENANCE.md"
echo "  [6] vendored dir with no provenance claim -> FAIL (rc 1) OK"

# --- 7. no plugin checkout -> HONEST-SKIP, never PASS ---------------------
# empty HOME so the conventional hints cannot resolve to this machine's real checkout.
out=$(env -u VIBEIC_PLUGIN_PROGRAMS HOME="$WORK/emptyhome" \
      python3 "$CHECK" --manifest "$MAN" --fork-root "$FORK" 2>&1); rc=$?
[ "$rc" = "3" ] || fail "[7] no plugin checkout should HONEST-SKIP (rc 3), got rc=$rc:
$out"
echo "  [7] no plugin checkout -> HONEST-SKIP (rc 3), not a vacuous PASS OK"

# --- 8. explicit-but-missing path -> ERROR, not a silent fall-through -----
out=$(python3 "$CHECK" --manifest "$MAN" --fork-root "$FORK" \
      --plugin-programs "$WORK/nope" 2>&1); rc=$?
[ "$rc" = "2" ] || fail "[8] explicit missing path should ERROR (rc 2), got rc=$rc:
$out"
echo "  [8] explicit --plugin-programs that does not exist -> ERROR (rc 2) OK"

echo "PASS run_vendored_sync_test (8/8)"
