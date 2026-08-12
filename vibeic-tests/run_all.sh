#!/usr/bin/env bash
#
# run_all.sh -- the ONE target that RUNS the regression harnesses this fork adds.
#
# WHY THIS FILE EXISTS
# --------------------
# The fork carries 25 hand-written FAIL->PASS gates for the features it adds on
# top of upstream KLayout (SVRF-native DRC, in-engine ANTENNA, metal fill, CAA,
# multi-patterning colouring, PERC latch-up, pattern match, LVS recon, CMP
# gradient, and the plugin-vendoring sync check). Every one of them was
# unreachable: `.github/workflows/build.yml` builds Python wheels and runs no
# test at all, and the repo-root `Makefile` has a `test` target that is macOS-only
# (`build4mac.py`, an .app bundle path, `ut_runner -h || true`). So the suites ran
# only when a human remembered to type their path.
#
# `make vibeic-test` (repo root) and `.github/workflows/vibeic-tests.yml` both
# invoke THIS script, and this script invokes every harness by name.
#
# WHAT IT DOES NOT DO
# -------------------
# It does not weaken a single assertion, it never adds `|| true`, and it never
# converts a red suite into a green one. A harness that exits non-zero is
# reported FAIL and this script exits 1.
#
# SKIPS ARE NAMED AND LOUD. Most harnesses need something this repo cannot carry:
# a KLayout `pya` runtime (they drive it through the published container image),
# or a `-without-qt` KLayout build to compile `engine_smoke` / `dump_parse_cpp`
# against `libklayout_db`. When that prerequisite is missing the suite is
# reported SKIP together with the exact reason and the command that would satisfy
# it -- never silently counted as a pass.
#
# ENVIRONMENT
# -----------
#   EDA_IMAGE     container carrying the KLayout binary + pya
#                 (default ghcr.io/vibeic/vibeic-eda:latest)
#   KLAYOUT_SRC   KLayout source tree            (default: this repo)
#   KLAYOUT_BLD   KLayout build dir              (default: ./bld-noqt, then
#                 $HOME/kbuild-out/bld)
#   KLAYOUT_BIN   KLayout libs + buddy binaries  (default: ./bin-noqt, then
#                 $HOME/kbuild-out/bin)
#   VIBEIC_TEST_TIMEOUT  per-suite timeout in seconds (default 1800)
#
# Produce KLAYOUT_BLD/KLAYOUT_BIN with:  ./vibeic-tests/build_klayout_noqt.sh
#
# Usage:
#   ./vibeic-tests/run_all.sh              # run everything
#   ./vibeic-tests/run_all.sh --list       # print the suite list and exit
#   ./vibeic-tests/run_all.sh svrf-drc     # run only suites whose path matches
#
# Exit: 0 no suite FAILED   1 at least one suite FAILED   2 could not start
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"

# ---------------------------------------------------------------------------
# THE SUITES. Every entry point this fork adds, listed by path. Adding a harness
# and not adding it here is the defect this file exists to remove, so
# `--list` is diffed against the tree by vibeic-tests/check_suite_list.sh.
# ---------------------------------------------------------------------------
SUITES=(
  vibeic-tests/check_suite_list.sh
  caa/tests/run_caa_test.sh
  cmp-gradient/tests/run_cmp_gradient_test.sh
  gds-antenna/tests/run_antenna_tests.sh
  gds-antenna/tests/run_diode_relief_test.sh
  lvs-recon/tests/run_lvs_recon_test.sh
  metal-fill/tests/run_cheese_test.sh
  metal-fill/tests/run_fill_dummy_datatype_test.sh
  metal-fill/tests/run_fill_tests.sh
  mp-color/tests/run_mp_color_test.sh
  pattern-match/tests/run_pattern_match_test.sh
  perc-latchup/tests/run_perc_latchup_test.sh
  plugin-wiring/tests/run_vendored_sync_test.sh
  svrf-drc/tests/run_antenna_engine_test.sh
  svrf-drc/tests/run_critarea_engine_test.sh
  svrf-drc/tests/run_densgrad_engine_test.sh
  svrf-drc/tests/run_dfm_engine_test.sh
  svrf-drc/tests/run_engine_parity.sh
  svrf-drc/tests/run_erc_engine_test.sh
  svrf-drc/tests/run_label_engine_test.sh
  svrf-drc/tests/run_mask_engine_test.sh
  svrf-drc/tests/run_parse_parity.sh
  svrf-drc/tests/run_property_engine_test.sh
  svrf-drc/tests/run_rve_engine_test.sh
  svrf-drc/tests/run_vspace_engine_test.sh
  svrf-drc/tests/run_waiver_engine_test.sh
)

FILTER=""
for a in "$@"; do
  case "$a" in
    --list) printf '%s\n' "${SUITES[@]}"; exit 0 ;;
    -h|--help) sed -n '2,60p' "$0"; exit 0 ;;
    *) FILTER="$a" ;;
  esac
done

# ---------------------------------------------------------------------------
# Resolve the environment ONCE, and say out loud what was found. A prerequisite
# that silently resolved to nothing is how a suite becomes a vacuous pass.
# ---------------------------------------------------------------------------
EDA_IMAGE="${EDA_IMAGE:-ghcr.io/vibeic/vibeic-eda:latest}"
export EDA_IMAGE

KLAYOUT_SRC="${KLAYOUT_SRC:-$ROOT}"
if [ -z "${KLAYOUT_BLD:-}" ]; then
  for c in "$ROOT/bld-noqt" "$HOME/kbuild-out/bld"; do
    [ -d "$c" ] && { KLAYOUT_BLD="$c"; break; }
  done
fi
if [ -z "${KLAYOUT_BIN:-}" ]; then
  for c in "$ROOT/bin-noqt" "$HOME/kbuild-out/bin"; do
    [ -f "$c/libklayout_db.so" ] && { KLAYOUT_BIN="$c"; break; }
  done
fi
KLAYOUT_BLD="${KLAYOUT_BLD:-}"
KLAYOUT_BIN="${KLAYOUT_BIN:-}"
export KLAYOUT_SRC KLAYOUT_BLD KLAYOUT_BIN

# The buddy binaries (strmrun, svrfdrc) live beside the libs; the harnesses look
# for them on PATH.
if [ -n "$KLAYOUT_BIN" ] && [ -d "$KLAYOUT_BIN" ]; then
  PATH="$KLAYOUT_BIN:$PATH"; export PATH
  LD_LIBRARY_PATH="$KLAYOUT_BIN:$KLAYOUT_BIN/db_plugins:${LD_LIBRARY_PATH:-}"
  export LD_LIBRARY_PATH
fi

# -- docker + image ---------------------------------------------------------
HAVE_DOCKER=0; DOCKER_WHY="docker not on PATH"
if command -v docker >/dev/null 2>&1; then
  if ! docker info >/dev/null 2>&1; then
    DOCKER_WHY="docker is installed but its daemon is not reachable"
  elif docker image inspect "$EDA_IMAGE" >/dev/null 2>&1; then
    HAVE_DOCKER=1
  elif docker pull "$EDA_IMAGE" >/dev/null 2>&1; then
    HAVE_DOCKER=1
  else
    DOCKER_WHY="EDA_IMAGE '$EDA_IMAGE' is neither present locally nor pullable"
  fi
fi

# -- a KLayout pya runner on PATH (strmrun / klayout) -----------------------
HAVE_KLBIN=0
command -v strmrun >/dev/null 2>&1 && HAVE_KLBIN=1
command -v klayout >/dev/null 2>&1 && HAVE_KLBIN=1

# -- the db build the C++ gates compile against -----------------------------
HAVE_DBBUILD=0; DB_WHY=""
if [ -n "$KLAYOUT_BIN" ] && [ -f "$KLAYOUT_BIN/libklayout_db.so" ] \
   && [ -n "$KLAYOUT_BLD" ] && [ -d "$KLAYOUT_SRC/src/db/db" ]; then
  HAVE_DBBUILD=1
else
  DB_WHY="no KLayout db build (run ./vibeic-tests/build_klayout_noqt.sh, or set KLAYOUT_SRC/BLD/BIN)"
fi

TIMEOUT="${VIBEIC_TEST_TIMEOUT:-1800}"
TIMEOUT_CMD=""
command -v timeout >/dev/null 2>&1 && TIMEOUT_CMD="timeout ${TIMEOUT}"

echo "=============================================================================="
echo " vibeic fork regression suites -- ${#SUITES[@]} entry point(s)"
echo "=============================================================================="
echo "  EDA_IMAGE     : $EDA_IMAGE  ($([ $HAVE_DOCKER = 1 ] && echo usable || echo "UNUSABLE: $DOCKER_WHY"))"
echo "  KLAYOUT_SRC   : $KLAYOUT_SRC"
echo "  KLAYOUT_BLD   : ${KLAYOUT_BLD:-<unset>}"
echo "  KLAYOUT_BIN   : ${KLAYOUT_BIN:-<unset>}"
echo "  klayout/strmrun on PATH: $([ $HAVE_KLBIN = 1 ] && command -v strmrun || command -v klayout || echo NO)"
echo "  db build for the C++ gates: $([ $HAVE_DBBUILD = 1 ] && echo yes || echo "no -- $DB_WHY")"
echo "  per-suite timeout: ${TIMEOUT}s"
echo

LOGDIR="${VIBEIC_TEST_LOGDIR:-$ROOT/vibeic-tests/logs}"
mkdir -p "$LOGDIR" || { echo "COULD NOT START: cannot create $LOGDIR"; exit 2; }

n_pass=0; n_fail=0; n_skip=0
declare -a FAILED=() SKIPPED=() PASSED=()

for s in "${SUITES[@]}"; do
  case "$s" in *"$FILTER"*) ;; *) continue ;; esac
  name="$(basename "$s" .sh)"
  path="$ROOT/$s"
  log="$LOGDIR/$name.log"

  if [ ! -f "$path" ]; then
    echo "FAIL  $s"
    echo "        the suite list names a harness that is not in the tree"
    n_fail=$((n_fail + 1)); FAILED+=("$s :: missing from the tree"); continue
  fi

  # --- prerequisites, checked BEFORE the run so the reason is precise -------
  need_docker=0; need_klbin=0; need_db=0
  grep -q 'docker run' "$path" && need_docker=1
  grep -qE 'command -v (strmrun|klayout|svrfdrc)' "$path" && need_klbin=1
  grep -q 'KLAYOUT_BLD' "$path" && need_db=1

  why=""
  [ $need_db = 1 ]     && [ $HAVE_DBBUILD = 0 ] && why="$DB_WHY"
  [ -z "$why" ] && [ $need_docker = 1 ] && [ $HAVE_DOCKER = 0 ] && why="$DOCKER_WHY"
  [ -z "$why" ] && [ $need_klbin = 1 ] && [ $HAVE_KLBIN = 0 ] && [ $need_docker = 0 ] \
      && why="no KLayout runner (strmrun/klayout) on PATH"
  if [ -n "$why" ]; then
    echo "SKIP  $s"
    echo "        $why"
    n_skip=$((n_skip + 1)); SKIPPED+=("$s :: $why"); continue
  fi

  # Announce before running: a suite can take tens of minutes (each svrf-drc gate
  # compiles engine_smoke and drives the container), and a CI log that says
  # nothing for half an hour is indistinguishable from a hang.
  echo "RUN   $s ..."
  start=$(date +%s)
  # rc is captured BEFORE any pipe -- `cmd | tee` would hand back tee's rc.
  $TIMEOUT_CMD bash "$path" > "$log" 2>&1
  rc=$?
  dur=$(( $(date +%s) - start ))

  # A harness that exits 0 having printed its own "SKIP <name>: <reason>" banner
  # did NOT run. Counting that as a pass is the exact deception this wiring
  # exists to remove.
  own_skip="$(grep -m1 -E "^[[:space:]]*SKIP +${name}[: ]" "$log" || true)"
  if [ $rc -eq 0 ] && [ -n "$own_skip" ]; then
    echo "SKIP  $s   (${dur}s)"
    echo "        ${own_skip#"${own_skip%%[![:space:]]*}"}"
    n_skip=$((n_skip + 1)); SKIPPED+=("$s :: ${own_skip}")
  elif [ $rc -eq 0 ]; then
    echo "PASS  $s   (${dur}s)"
    tail -n 1 "$log" | sed 's/^/        /'
    # a suite that passed while skipping SOME of its checks says so here
    grep -E "SKIP [A-Za-z0-9_]* ?checks?:" "$log" | sed 's/^ *//;s/^/        partial-skip: /'
    n_pass=$((n_pass + 1)); PASSED+=("$s")
  else
    if [ $rc -eq 124 ]; then
      echo "FAIL  $s   (TIMEOUT after ${TIMEOUT}s)"
      FAILED+=("$s :: TIMEOUT after ${TIMEOUT}s")
    else
      echo "FAIL  $s   (rc=$rc, ${dur}s)"
      FAILED+=("$s :: rc=$rc :: $(grep -m1 -E '^(FAIL|.*: FAIL)' "$log" | head -c 200)")
    fi
    sed -n '$p' "$log" | sed 's/^/        /'
    grep -m3 -E '^FAIL' "$log" | sed 's/^/        /'
    echo "        full log: $log"
    n_fail=$((n_fail + 1))
  fi
done

echo
echo "=============================================================================="
echo " RESULT: $n_pass passed, $n_fail FAILED, $n_skip skipped  (of $((n_pass+n_fail+n_skip)) run)"
echo "=============================================================================="
if [ ${#SKIPPED[@]} -gt 0 ]; then
  echo
  echo "SKIPPED -- each one names the prerequisite it needs. A skip is not a pass:"
  printf '  %s\n' "${SKIPPED[@]}"
fi
if [ ${#FAILED[@]} -gt 0 ]; then
  echo
  echo "FAILED:"
  printf '  %s\n' "${FAILED[@]}"
  echo
  echo "Logs in $LOGDIR"
  exit 1
fi
if [ $n_pass -eq 0 ]; then
  echo
  echo "NOTHING EXECUTED. Every suite was skipped for a missing prerequisite, so"
  echo "this run proves nothing about the fork's features -- and a run that proves"
  echo "nothing must not exit 0. Install what the skips above name, or invoke the"
  echo "suites whose prerequisites this environment has."
  exit 2
fi
exit 0
