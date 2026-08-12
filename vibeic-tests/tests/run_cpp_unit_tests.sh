#!/usr/bin/env bash
#
# run_cpp_unit_tests.sh -- run the C++ unit tests THIS FORK adds, via ut_runner.
#
# WHY THIS FILE EXISTS
# --------------------
# The fork's C++ guards live in `src/*/unit_tests/*.cc` and are registered in
# `unit_tests.pro`, i.e. in qmake and nowhere else. Every Linux-reachable
# invocation of `ut_runner` in this repo was zero: the repo-root `make test`
# target is macOS-only (build4mac.py, klayout.app, `./ut_runner -h || true`), and
# macbuild/macQAT*.sh are macOS-only too. So a guard could be deleted, or the
# code under it neutralised by an upstream merge, and nothing would say a word.
#
# `vibeic-tests/run_all.sh` invokes this script; this script invokes ut_runner
# with the patterns named in `vibeic-tests/FORK_SOURCES.json` -> `cpp_tests`, and
# `check_build_registration.py` asserts that list still covers every TEST() the
# fork adds, so adding a guard and forgetting to wire it is itself a red gate.
#
# TWO WAYS THIS COULD LIE, BOTH CLOSED
# ------------------------------------
#  * ut_runner exits 0 when its pattern matched NOTHING -- measured:
#      $ ut_runner 'dbDeepRegion:deep_two_layer_check_ignores_reduction_split_lines'
#        Executed 0 test(s).  All tests passed.                       rc=0
#    (a group-name typo: the group is dbDeepRegionTests). So the executed count
#    is parsed and a zero count is a FAILURE, never a pass.
#  * `make` in a module leaves the fresh library in KLAYOUT_BLD and only
#    `make install` copies it to KLAYOUT_BIN, so KLAYOUT_BLD goes FIRST on
#    LD_LIBRARY_PATH -- otherwise a rebuilt library is tested through last
#    build's binary.
#
# Env: KLAYOUT_BLD (dir holding ut_runner + the *.ut test libraries)
#      KLAYOUT_BIN (optional, the installed libraries)
#      KLAYOUT_SRC (the source tree; TESTSRC for ut_runner. Default: this repo)
#
# Exit: 0 PASS   1 FAIL   0-with-SKIP-banner when there is no build to run
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
NAME=run_cpp_unit_tests
MANIFEST="$ROOT/vibeic-tests/FORK_SOURCES.json"

[ -f "$MANIFEST" ] || { echo "FAIL $NAME: $MANIFEST is missing from the tree"; exit 1; }

BLD="${KLAYOUT_BLD:-}"
if [ -z "$BLD" ]; then
  for c in "$ROOT/bld-noqt" "$HOME/kbuild-out/bld"; do
    [ -x "$c/ut_runner" ] && { BLD="$c"; break; }
  done
fi
if [ -z "$BLD" ] || [ ! -x "$BLD/ut_runner" ]; then
  echo "SKIP $NAME: no ut_runner -- set \$KLAYOUT_BLD to a KLayout build dir, or"
  echo "     produce one with ./vibeic-tests/build_klayout_noqt.sh (ut_runner is"
  echo "     built by src/unit_tests/unit_tests.pro, which src/klayout.pro lists"
  echo "     unconditionally, so the -without-qt build already produces it)."
  exit 0
fi

PATTERNS="$(python3 - "$MANIFEST" <<'PY'
import json, sys
m = json.load(open(sys.argv[1]))
for t in m.get("cpp_tests", []):
    print("%s:%s" % (t["group"], t["test"]))
PY
)" || { echo "FAIL $NAME: could not read cpp_tests from $MANIFEST"; exit 1; }

if [ -z "$PATTERNS" ]; then
  echo "FAIL $NAME: $MANIFEST declares no cpp_tests. If the fork genuinely adds"
  echo "     no C++ guard, delete this harness and its run_all.sh entry rather"
  echo "     than leaving a suite that runs nothing and reports success."
  exit 1
fi

SRC="${KLAYOUT_SRC:-$ROOT}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
export TESTSRC="$SRC" TESTTMP="$TMP"
export LD_LIBRARY_PATH="$BLD:${KLAYOUT_BIN:-$BLD}:${KLAYOUT_BIN:-$BLD}/db_plugins:${LD_LIBRARY_PATH:-}"

echo "  ut_runner : $BLD/ut_runner"
echo "  TESTSRC   : $TESTSRC"

rc=0
n=0
while IFS= read -r pat; do
  [ -n "$pat" ] || continue
  n=$((n + 1))
  log="$TMP/$(printf '%s' "$pat" | tr -c 'A-Za-z0-9_' '_').log"
  "$BLD/ut_runner" "$pat" > "$log" 2>&1
  trc=$?
  executed="$(sed -n 's/.*Executed \([0-9][0-9]*\) test(s)$/\1/p' "$log" | tail -n 1)"
  executed="${executed:-0}"
  if [ "$executed" -eq 0 ]; then
    echo "FAIL  $pat -- ut_runner matched NO test and still exited $trc."
    echo "      A pattern that selects nothing is not a test that passed; check"
    echo "      the group name against \`ut_runner -l\`."
    rc=1
  elif [ $trc -ne 0 ]; then
    echo "FAIL  $pat  (rc=$trc, executed=$executed)"
    sed -n '/Test .* failed:/,/^$/p' "$log" | sed 's/^/      /' | head -n 20
    rc=1
  else
    echo "PASS  $pat  (executed=$executed)"
  fi
done <<EOF
$PATTERNS
EOF

if [ $rc -eq 0 ]; then
  echo "PASS $NAME ($n fork C++ guard(s) executed and clean)"
else
  echo "FAIL $NAME"
fi
exit $rc
