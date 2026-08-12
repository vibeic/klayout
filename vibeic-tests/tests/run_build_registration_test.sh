#!/usr/bin/env bash
#
# run_build_registration_test.sh -- run vibeic-tests/check_build_registration.py
# from run_all.sh.
#
# The checker is also declared directly in the fleet's FORKS.json
# `post_merge_check` for this fork, because that is the mechanism that actually
# executes on a merged tree; this wrapper is how the same command reaches
# `make vibeic-test` and the suite list. ONE implementation, two call sites.
#
# It needs no build, no container and no PDK -- pure text over the .pro/.pri
# graph -- so it always runs and is never skipped.
#
# Exit: 0 PASS   1 FAIL
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
exec python3 "$ROOT/vibeic-tests/check_build_registration.py" --root "$ROOT"
