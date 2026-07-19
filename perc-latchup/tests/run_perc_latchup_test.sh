#!/usr/bin/env bash
# run_perc_latchup_test.sh -- UNFAKEABLE gate for the latch-up guard-ring checks
# (#26): the geometry-derivable half of Calibre PERC's latch-up family. For every
# sensitive device the tool verifies a substrate/well tap guard ring by DISTANCE
# (a tap within max_dist), WIDTH (ring >= min_width) and ENCLOSURE (the device
# lies inside a HOLE of the merged tap -- a full ring, not a one-sided bar).
#
# Fixture (gen_perc_gds.py): a 2x2 um device at [0,0..2,2], rules max_dist 1.0 um
# and min_width 0.4 um. Each mode perturbs ONE property so each check is isolated:
#
#   1. clean : a full ring (width 0.5, gap 0.5) -> all three checks PASS.
#   2. bar   : tap on ONE side only -> distance and width PASS, but a bar has no
#      hole -> ONLY enclosure FAILs, marker = the device [0,0,2,2]. Proves the
#      enclosure check is real ring topology, not mere tap presence.
#   3. far   : a full ring 1.5 um from the device -> ONLY distance FAILs (still
#      enclosed, still wide enough), marker = the device.
#   4. narrow: a full ring only 0.3 um wide -> ONLY width FAILs, marker = the ring.
#   5. clean is also the PROVEN-NEGATIVE for all three (0 violations, empty DB).
#   6. DBU BOUNDARY: a ring whose inner edge is EXACTLY 1.0 um from the device ->
#      distance PASSES (gap <= max_dist); the SAME ring 1 DBU (0.001 um) further
#      out -> distance FAILs. One DBU decides it.
#
# Every violation marker is confirmed by loading the emitted .lyrdb back through
# KLayout's OWN pya.ReportDatabase.
#
# NO vendor data. Skips (exit 0) when a container is absent.
# Env: EDA_IMAGE (default vibeic/vibeic-eda:0.2.18)
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
TOOL="$(cd "$HERE/.." && pwd)"
ROOT="$(cd "$TOOL/.." && pwd)"
IMAGE="${EDA_IMAGE:-vibeic/vibeic-eda:0.2.18}"

if ! command -v docker >/dev/null 2>&1; then
  echo "SKIP run_perc_latchup_test: no docker (KLayout pya needed)"; exit 0
fi

WORK="$(mktemp -d)"; trap 'rm -rf "$WORK"' EXIT
fail() { echo "FAIL: $1"; exit 1; }

cp "$TOOL/perc_latchup.py" "$HERE/gen_perc_gds.py" "$ROOT/svrf-drc/tests/rve_load.py" "$WORK/"
printf '{"device_layer":[12,0],"tap_layer":[13,0],"max_dist_um":1.0,"min_width_um":0.4}\n' > "$WORK/cfg.json"

echo "== generate fixtures + run the tool =="
docker run --rm --entrypoint bash -v "$WORK":/work "$IMAGE" -lc "
  export PATH=/foss/tools/klayout:\$PATH
  for m in clean bar far narrow edge justin; do
    PL_MODE=\$m PL_OUT=/work/\$m.gds klayout -b -r /work/gen_perc_gds.py >/dev/null 2>&1
    PL_GDS=/work/\$m.gds PL_CONFIG=/work/cfg.json PL_OUT=/work/\$m.json \
      PL_RVE=/work/\$m.lyrdb klayout -b -r /work/perc_latchup.py >/dev/null 2>&1
  done
" >/dev/null 2>&1
for m in clean bar far narrow edge justin; do [ -s "$WORK/$m.json" ] || fail "no report for $m"; done

v()  { python3 -c "import json;print(json.load(open('$WORK/$1.json'))['$2']['verdict'])"; }
nv() { python3 -c "import json;print(json.load(open('$WORK/$1.json'))['$2']['violations'])"; }
ov() { python3 -c "import json;print(json.load(open('$WORK/$1.json'))['verdict'])"; }
mk() { python3 -c "import json;print(json.load(open('$WORK/$1.json'))['$2']['markers'][0]['bbox_um'])"; }
load() { docker run --rm --entrypoint bash -v "$WORK":/work "$IMAGE" -lc "
  export PATH=/foss/tools/klayout:\$PATH
  LYRDB=/work/$1 klayout -b -r /work/rve_load.py 2>&1 | grep -E '^(CAT|BBOX|NITEMS|ERROR) '
"; }

echo "== 1. clean: a full ring -> all three checks PASS =="
[ "$(ov clean)" = "PASS" ]           || fail "clean overall not PASS"
[ "$(v clean distance)" = "PASS" ]   || fail "clean distance not PASS"
[ "$(v clean width)" = "PASS" ]      || fail "clean width not PASS"
[ "$(v clean enclosure)" = "PASS" ]  || fail "clean enclosure not PASS"
C="$(load clean.lyrdb)"
echo "$C" | grep -qxE 'NITEMS 0' || { echo "$C"; fail "clean marker DB must be empty"; }
echo "  [1] distance/width/enclosure all PASS, empty marker DB"

echo "== 2. bar: one-sided tap -> ONLY enclosure fails (ring topology, not presence) =="
[ "$(v bar distance)" = "PASS" ]    || fail "bar distance should PASS (tap is close)"
[ "$(v bar width)" = "PASS" ]       || fail "bar width should PASS (tap is 0.5 wide)"
[ "$(v bar enclosure)" = "FAIL" ]   || fail "bar enclosure should FAIL (a bar has no ring hole)"
[ "$(nv bar enclosure)" = "1" ]     || fail "bar enclosure violations != 1"
[ "$(mk bar enclosure)" = "[0.0, 0.0, 2.0, 2.0]" ] || fail "bar enclosure marker != the device [0,0,2,2]"
B="$(load bar.lyrdb)"; echo "$B" | sed 's/^/   /'
echo "$B" | grep -qxE 'CAT LATCHUP_NO_RING'           || { echo "$B"; fail "bar missing LATCHUP_NO_RING"; }
echo "$B" | grep -qxE 'BBOX LATCHUP_NO_RING:0,0,2,2'  || { echo "$B"; fail "bar marker != [0,0,2,2]"; }
echo "  [2] only enclosure FAILs, marker = the unprotected device, via KLayout's rdb reader"

echo "== 3. far: full ring 1.5 um away -> ONLY distance fails =="
[ "$(v far distance)" = "FAIL" ]    || fail "far distance should FAIL"
[ "$(v far width)" = "PASS" ]       || fail "far width should PASS"
[ "$(v far enclosure)" = "PASS" ]   || fail "far enclosure should PASS (still a ring)"
[ "$(mk far distance)" = "[0.0, 0.0, 2.0, 2.0]" ] || fail "far distance marker != the device"
echo "  [3] only distance FAILs -- the ring encircles but is too far to tie down"

echo "== 4. narrow: 0.3 um ring -> ONLY width fails =="
[ "$(v narrow width)" = "FAIL" ]     || fail "narrow width should FAIL (0.3 < 0.4)"
[ "$(v narrow distance)" = "PASS" ]  || fail "narrow distance should PASS"
[ "$(v narrow enclosure)" = "PASS" ] || fail "narrow enclosure should PASS"
N="$(load narrow.lyrdb)"
echo "$N" | grep -qxE 'CAT LATCHUP_RING_WIDTH' || { echo "$N"; fail "narrow missing LATCHUP_RING_WIDTH"; }
echo "  [4] only width FAILs, marker = the too-thin ring"

echo "== 5. each check is independently isolated (no cross-talk) =="
#  every FAIL above changed exactly ONE check; assert the total failing checks
for m in bar far narrow; do
  cnt=$(python3 -c "import json;d=json.load(open('$WORK/$m.json'));print(sum(1 for k in ('distance','width','enclosure') if d[k]['verdict']=='FAIL'))")
  [ "$cnt" = "1" ] || fail "$m changed $cnt checks, expected exactly 1"
done
echo "  [5] bar->enclosure only, far->distance only, narrow->width only"

echo "== 6. DBU boundary: inner edge at 1.000 um PASSes, 1.001 um FAILs =="
[ "$(v edge distance)" = "PASS" ]   || fail "gap == 1.000 um must PASS (gap <= max_dist)"
[ "$(v justin distance)" = "FAIL" ] || fail "gap 1.001 um (1 DBU past) must FAIL"
echo "  [6] 1.000 um -> PASS, 1.001 um -> FAIL (pinned to the DBU)"

echo "PASS run_perc_latchup_test (6/6)"
