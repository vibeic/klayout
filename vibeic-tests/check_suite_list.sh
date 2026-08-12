#!/usr/bin/env bash
#
# check_suite_list.sh -- the wiring cannot rot silently.
#
# `vibeic-tests/run_all.sh` names its suites in a literal list, which is what
# makes them reachable AND what makes them runnable. A list is also the thing
# that goes stale: the 25 harnesses this repo carries became unreachable one at a
# time, each added next to its feature and wired to nothing.
#
# So this asks the only question that keeps the list true: is there a harness in
# the tree that `run_all.sh` does not name? It is cheap (no build, no container,
# pure git + text) and runs as the first suite of every CI job.
#
# Exit: 0 every harness in the tree is in the runner's list
#       1 at least one harness exists that the runner would never invoke
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"

listed="$("$HERE/run_all.sh" --list)"

# Harnesses of OURS: `run_*.sh` under a tests/ directory. Upstream KLayout has no
# such file, which is why the pattern is safe to apply tree-wide; `git ls-files`
# keeps untracked scratch out.
found="$(cd "$ROOT" && git ls-files '*/tests/run_*.sh' 2>/dev/null || true)"
if [ -z "$found" ]; then
  echo "SKIP check_suite_list: not a git checkout, cannot enumerate the tree"
  exit 0
fi

rc=0
missing=""
for f in $found; do
  case "$listed" in
    *"$f"*) ;;
    *) missing="$missing $f"; rc=1 ;;
  esac
done

n_found=$(printf '%s\n' $found | wc -l)
n_listed=$(printf '%s\n' "$listed" | wc -l)
echo "  harnesses in the tree: $n_found   named by run_all.sh: $n_listed"

if [ $rc -ne 0 ]; then
  echo "FAIL: run_all.sh does not name these harnesses, so nothing would ever run them:"
  for f in $missing; do echo "    $f"; done
  echo "  Add them to the SUITES list in vibeic-tests/run_all.sh."
  exit 1
fi

# The reverse direction is caught by run_all.sh itself (a listed path that is not
# in the tree is reported FAIL there), so it is not duplicated here.
echo "PASS check_suite_list ($n_found/$n_found harnesses are wired into run_all.sh)"
