#!/usr/bin/env bash
#
# build_klayout_noqt.sh -- produce the KLayout build that the fork's C++ gates
# compile against, into ./bld-noqt + ./bin-noqt.
#
# WHY -without-qt SPECIFICALLY, AND WHY IT IS NOT AN OPTIMISATION
# --------------------------------------------------------------
# `svrf-drc/tests/{run_engine_parity,run_parse_parity,run_*_engine_test}.sh`
# compile `engine_smoke.cc` / `dump_parse_cpp.cc` together with
# `dbSVRFDeck.cc` + `dbSVRFEngine.cc` and link against `libklayout_tl`. In
# `src/tl/tl/tlThreads.h`:
#
#     #if defined(HAVE_QT) && !defined(HAVE_PTHREADS)
#     class TL_PUBLIC Thread : public QThread { ... };     // header-only
#     #else
#     class TL_PUBLIC Thread { ... };                      // tlThreads.cc
#     #endif
#
# so a Qt build EXPORTS NO `tl::Thread` symbols at all. Measured against the
# published runtime image (a Qt6 build):
#
#     nm -D --defined-only libklayout_tl.so | grep -c '6ThreadC'   ->  0
#     ld: undefined reference to `tl::Thread::Thread()'            (x4 call sites)
#
# and against the `-without-qt` build this script produces: 2 constructors and
# 17 `tl::Thread*` symbols in total, link OK. The gates therefore need this build
# and cannot use the shipped image's libraries.
#
# Requires (Ubuntu/Debian):
#   build-essential python3-dev zlib1g-dev libexpat1-dev libcurl4-openssl-dev
#   libpng-dev qt6-base-dev
# qt6-base-dev is needed for `qmake` ONLY -- it is the makefile generator
# `build.sh` drives; HAVE_QT is still 0 in the produced build. `qt6-base-dev-tools`
# alone is NOT enough: it ships androiddeployqt and qdbuscpp2xml but no qmake.
#
# Env: KLAYOUT_BUILD_JOBS (default nproc)
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
BLD="${KLAYOUT_BLD:-$ROOT/bld-noqt}"
BIN="${KLAYOUT_BIN:-$ROOT/bin-noqt}"
JOBS="${KLAYOUT_BUILD_JOBS:-$(nproc 2>/dev/null || echo 4)}"

QMAKE=""
for c in /usr/lib/qt6/bin/qmake /usr/lib/qt6/bin/qmake6 "$(command -v qmake6 || true)" \
         "$(command -v qmake || true)"; do
  [ -n "$c" ] && [ -x "$c" ] && { QMAKE="$c"; break; }
done
if [ -z "$QMAKE" ]; then
  echo "CANNOT BUILD: no qmake found. Install qt6-base-dev (qmake is the makefile"
  echo "              generator build.sh drives; HAVE_QT stays 0)."
  exit 2
fi
echo "qmake: $QMAKE"

# `cp -a` of a source tree carries stale objects, and a stale .o linked into a
# fresh .so is a build that reports success and ships last week's code.
find "$BLD" -name '*.o' -delete 2>/dev/null || true

cd "$ROOT"
./build.sh -qmake "$QMAKE" -without-qt -noruby -nolibgit2 \
           -j"$JOBS" -build "$BLD" -bin "$BIN"

# A build step that exits 0 having produced nothing is the failure mode that
# matters here, so assert the ARTEFACTS rather than the exit code.
missing=0
for a in "$BIN/libklayout_db.so" "$BIN/libklayout_tl.so" "$BIN/libklayout_gsi.so" \
         "$BIN/db_plugins/libgds2.so" "$BIN/strmrun" "$BIN/svrfdrc"; do
  [ -e "$a" ] || { echo "MISSING ARTEFACT: $a"; missing=1; }
done
n=$(nm -D --defined-only "$BIN/libklayout_tl.so" 2>/dev/null | grep -c '2tl6Thread' || true)
echo "tl::Thread symbols exported by libklayout_tl.so: $n"
[ "${n:-0}" -gt 0 ] || { echo "FAIL: this build exports no tl::Thread -- the C++ gates cannot link against it"; missing=1; }
[ "$missing" = 0 ] || exit 1

echo "OK: KLAYOUT_BLD=$BLD KLAYOUT_BIN=$BIN"
