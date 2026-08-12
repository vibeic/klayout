#!/usr/bin/env bash
#
# run_deep_split_line_test.sh -- run vibeic-tests/deep_split_line_probe.py
# wherever a KLayout Python binding can be found.
#
# The probe guards a5a7a2d6b (DeepShapeStore split lines reported as real polygon
# edges by two-layer checks). Its C++ sibling,
# TEST(deep_two_layer_check_ignores_reduction_split_lines), can only run out of a
# full build; this one needs a binding only, so it reaches the SHIPPED artefact,
# where `ut_runner` and the `*.ut` libraries are deleted by
# tools/klayout/Dockerfile.
#
# Resolution order, most-local first, each announced:
#   1. $KLAYOUT_BLD/pymod          -- the -without-qt build this repo's gates use
#   2. `python3 -c "import klayout.db"` on the ambient interpreter
#   3. `strmrun` / `klayout -b -r` on PATH
#   4. $EDA_IMAGE via docker       -- the published image
# and an HONEST, NAMED skip if none of them exists. A skip is never a pass.
#
# `--require` turns "no binding reachable" from an honest skip into a FAILURE.
# run_all.sh wants the skip (a developer box legitimately has neither a build nor
# the image); a gate that decides whether a merge may be PUBLISHED does not, and
# the fleet's FORKS.json declaration passes it.
#
# Exit: 0 PASS   1 FAIL   0-with-SKIP-banner when no binding is reachable
#       (1 instead, under --require)
#
# vibeic-prereq: self
# (this harness resolves its own binding and prints its own named SKIP;
#  run_all.sh must not skip it for the absence of any single one of them)
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
PROBE="$ROOT/vibeic-tests/deep_split_line_probe.py"
NAME=run_deep_split_line_test

REQUIRE=0
for a in "$@"; do
  case "$a" in --require) REQUIRE=1 ;; esac
done

[ -f "$PROBE" ] || { echo "FAIL $NAME: $PROBE is missing from the tree"; exit 1; }

run_local_build () {
  [ -n "${KLAYOUT_BLD:-}" ] && [ -d "${KLAYOUT_BLD}/pymod" ] || return 9
  echo "  binding: pymod from KLAYOUT_BLD=$KLAYOUT_BLD"
  # The build dir comes FIRST on LD_LIBRARY_PATH: `make` in a module leaves the
  # fresh library there and only `make install` copies it to KLAYOUT_BIN, so
  # searching bin first can silently test last build's code.
  PYTHONPATH="$KLAYOUT_BLD/pymod" \
  LD_LIBRARY_PATH="$KLAYOUT_BLD:${KLAYOUT_BIN:-$KLAYOUT_BLD}:${KLAYOUT_BIN:-$KLAYOUT_BLD}/db_plugins:${LD_LIBRARY_PATH:-}" \
    python3 "$PROBE"
}

run_ambient_pymod () {
  python3 -c "import klayout.db" >/dev/null 2>&1 || return 9
  echo "  binding: klayout.db on the ambient python3"
  python3 "$PROBE"
}

run_klayout_binary () {
  local exe=""
  command -v strmrun  >/dev/null 2>&1 && exe="$(command -v strmrun) -b -r"
  [ -z "$exe" ] && command -v klayout >/dev/null 2>&1 && exe="$(command -v klayout) -zz -b -r"
  [ -n "$exe" ] || return 9
  echo "  binding: pya via $exe"
  $exe "$PROBE"
}

run_in_container () {
  command -v docker >/dev/null 2>&1 || return 9
  docker info >/dev/null 2>&1 || return 9
  local img="${EDA_IMAGE:-ghcr.io/vibeic/vibeic-eda:latest}"
  docker image inspect "$img" >/dev/null 2>&1 || docker pull "$img" >/dev/null 2>&1 || return 9
  echo "  binding: pymod inside $img (/foss/tools/klayout)"
  docker run --rm -v "$PROBE:/deep_split_line_probe.py:ro" --entrypoint /bin/bash "$img" \
    -c 'PYTHONPATH=/foss/tools/klayout/pymod LD_LIBRARY_PATH=/foss/tools/klayout python3 /deep_split_line_probe.py'
}

for how in run_local_build run_ambient_pymod run_klayout_binary run_in_container; do
  out="$($how 2>&1)"; rc=$?
  if [ $rc -ne 9 ]; then
    printf '%s\n' "$out"
    exit $rc
  fi
done

if [ "$REQUIRE" = 1 ]; then
  echo "FAIL $NAME: --require was given and no KLayout Python binding is"
  echo "     reachable, so the DeepShapeStore split-line guard did not run. A"
  echo "     gate that could not execute has not passed."
  exit 1
fi
echo "SKIP $NAME: no KLayout Python binding reachable -- none of"
echo "     \$KLAYOUT_BLD/pymod (run ./vibeic-tests/build_klayout_noqt.sh),"
echo "     an ambient \`import klayout.db\`, \`strmrun\`/\`klayout\` on PATH, or"
echo "     docker + \$EDA_IMAGE (${EDA_IMAGE:-ghcr.io/vibeic/vibeic-eda:latest})"
echo "     is available, so the DeepShapeStore split-line guard did not run."
exit 0
