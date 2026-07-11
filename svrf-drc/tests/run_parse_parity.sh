#!/usr/bin/env bash
#
# run_parse_parity.sh -- prove the NATIVE C++ parser (db::parse_deck) reproduces
# the FROZEN parse-dump goldens byte-for-byte on SYNTHETIC decks. No vendor data.
#
# The goldens (tests/<name>.golden for demo/conn/coverage) were produced by the
# reference Python parser (svrf_parse.py) via tests/dump_parse.py and COMMITTED
# as the frozen oracle. The Python parser has since been RETIRED (the native C++
# parser is the shipped path), so this test diffs the C++ dump (dump_parse_cpp)
# against the committed golden -- no Python required.
#
# Env (override as needed):
#   KLAYOUT_SRC   klayout source tree      (default ~/kbuild)
#   KLAYOUT_BLD   klayout build dir        (default ~/kbuild-out/bld)
#   KLAYOUT_BIN   klayout installed libs   (default ~/kbuild-out/bin)
#
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
SVRF="$(cd "$HERE/.." && pwd)"                       # svrf-drc/
DBP="$SVRF/../src/plugins/tools/svrf_drc/db_plugin"  # dbSVRFDeck source

KSRC="${KLAYOUT_SRC:-$HOME/kbuild}"
KBLD="${KLAYOUT_BLD:-$HOME/kbuild-out/bld}"
KBIN="${KLAYOUT_BIN:-$HOME/kbuild-out/bin}"
WORK="$(mktemp -d)"
BIN="$WORK/dump_parse_cpp"

echo "== 1. build dump_parse_cpp =="
g++ -std=c++17 -O1 \
  -I"$DBP" -I"$KSRC/src/db/db" -I"$KBLD/db/db" -I"$KSRC/src/tl/tl" -I"$KBLD/tl/tl" -I"$KSRC/src/gsi/gsi" \
  "$HERE/dump_parse_cpp.cc" "$DBP/dbSVRFDeck.cc" \
  -L"$KBIN" -lklayout_db -lklayout_tl -lklayout_gsi \
  -Wl,-rpath,"$KBIN" -o "$BIN"

echo "== 2. dump each synthetic deck + diff vs FROZEN golden =="
rc=0
for name in demo conn coverage; do
  golden="$HERE/${name}.golden"
  deck="$SVRF/examples/${name}.rule"
  if [ ! -f "$golden" ]; then
    echo "  $name: FAIL (no frozen golden at $golden)"; rc=1; continue
  fi
  "$BIN" "$deck" > "$WORK/${name}_cpp.txt"
  if diff "$golden" "$WORK/${name}_cpp.txt" >/dev/null; then
    echo "  $name: PASS (byte-identical to frozen golden)"
  else
    echo "  $name: FAIL"; diff "$golden" "$WORK/${name}_cpp.txt" || true; rc=1
  fi
done

rm -rf "$WORK"
exit $rc
