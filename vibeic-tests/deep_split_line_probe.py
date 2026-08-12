#!/usr/bin/env python3
"""deep_split_line_probe.py -- the DeepShapeStore split line must not be reported
as a real polygon edge by a two-layer check.

WHAT IT GUARDS
--------------
`check_local_operation::override_distance()` (src/db/db/dbRegionLocalOperations.cc)
pins the search enlargement of the "foreign" pseudo-intruder section -- the
section that exists solely so the local operation can merge the subject's own
siblings back together -- to "touching". In box_scanner terms that is 1, not 0:
at 0 the boxes must OVERLAP, and two fragments of one polygon that the
DeepShapeStore split for complexity reduction share exactly an edge, so their
boxes only touch. The sibling was therefore invisible, the merge never happened,
and the artificial cut line entered the edge check as a genuine polygon
boundary. Because a cut line is always interior to the original polygon the
error is one-sided: enclosure / overlap / separation checks produce FALSE
errors. Fixed by a5a7a2d6b (`od.insert (std::make_pair (1, 1))`).

WHY A SEPARATE PROBE WHEN THE C++ TEST ALREADY EXISTS
-----------------------------------------------------
The only guard was `TEST(deep_two_layer_check_ignores_reduction_split_lines)` in
src/db/unit_tests/dbDeepRegionTests.cc, which is registered in unit_tests.pro
and therefore needs a full KLayout build to run. The shipped image deliberately
deletes `ut_runner` and the `*.ut` libraries, so on the artefact we actually ship
NOTHING could answer whether the fix is present. This probe needs only a KLayout
Python binding -- `klayout.db` (pymod) or `pya` -- which the image has, so the
same invariant is checkable on the shipped binary with no build at all.

IT IS PROVEN TO DISCRIMINATE, NOT ASSUMED TO
--------------------------------------------
Measured 2026-08-12 on a `-without-qt` 0.30.10 build of this tree, by reverting
the one-character fix and rebuilding libklayout_db only:

    fix present  (make_pair (1, 1)):  FLAT n=6  DEEP n=6  EQUAL=True
    fix reverted (make_pair (1, 0)):  FLAT n=6  DEEP n=7  EQUAL=False
                                      EXTRA_IN_DEEP = (1065,1310;1215,1310)/
                                                      (1065,1235;1215,1235)

Note the fix's *baseline is 0.30.10, not 0.30.9*: the same probe run against the
stock KLayout 0.30.9 in the base image reports EQUAL=True as well, because the
defect arrived with upstream's own 2026-07-19 two-layer deep rework. A "green on
0.30.9" reading is therefore NOT evidence of anything, which is exactly why the
control above reverts the patch on THIS source rather than comparing releases.

VACUITY IS A FAILURE, NOT A PASS
--------------------------------
The whole check is meaningless unless the DeepShapeStore actually splits the
subject polygon. If a future reduction-heuristic change stops the split, the
"no spurious edge" assertion becomes trivially true. So the split itself is
asserted as a PRECONDITION and a run that did not reproduce it exits non-zero
saying so, rather than reporting a pass it did not earn.

Usage:  python3 deep_split_line_probe.py          # pymod on PYTHONPATH
        klayout -b -r deep_split_line_probe.py    # or via a KLayout runner
Exit:   0 PASS   1 FAIL (spurious edge, or the precondition did not hold)
        2 no KLayout Python binding importable
"""
import sys

try:
    import klayout.db as kdb
except ImportError:
    try:
        import pya as kdb
    except ImportError:
        sys.stderr.write(
            "SKIP deep_split_line_probe: neither `klayout.db` (pymod) nor `pya` "
            "is importable; run this under a KLayout runner or with the pymod "
            "directory on PYTHONPATH\n")
        sys.exit(2)

#: An 18-vertex met2 shape from a real sky130A sign-off run, reduced to the
#: smallest geometry that still reproduces. It exceeds the DeepShapeStore's
#: default max_vertex_count (16), so the store splits it at y=1310 -- the cut
#: line this probe is about. dbu is 0.001, so these are nanometres.
SUBJECT = [
    1010, 1000, 1010, 1145, 1000, 1145, 1000, 1515, 1280, 1515, 1280, 1400,
    1990, 1400, 1990, 1540, 2105, 1540, 2105, 1630, 2365, 1630, 2365, 1310,
    2130, 1310, 2130, 1260, 1280, 1260, 1280, 1145, 1270, 1145, 1270, 1000,
]
#: Two vias inside the subject. The lower one sits 280 dbu from the real metal
#: boundary but only 75 dbu from the cut line, so an 85 dbu enclosure rule is
#: clean on the true geometry and violated on the artificial one.
INTRUDERS = [(1065, 1085, 1215, 1235), (2160, 1395, 2310, 1545)]
ENCLOSING_DBU = 85
#: The cut line, as it appears in an edge-pair string. Named so a failure says
#: WHICH edge is spurious rather than only that two numbers differ.
SPLIT_LINE = "1310;1215,1310"


def build(layout):
    top = layout.create_cell("TOP")
    l1 = layout.layer(1, 0)
    l2 = layout.layer(2, 0)
    pts = [kdb.Point(SUBJECT[i], SUBJECT[i + 1]) for i in range(0, len(SUBJECT), 2)]
    top.shapes(l1).insert(kdb.Polygon(pts))
    for box in INTRUDERS:
        top.shapes(l2).insert(kdb.Box(*box))
    return top, l1, l2


def main():
    ly = kdb.Layout()
    ly.dbu = 0.001
    top, l1, l2 = build(ly)

    def region(layer, dss=None):
        it = kdb.RecursiveShapeIterator(ly, top, layer)
        return kdb.Region(it, dss) if dss is not None else kdb.Region(it)

    # Projection metrics, whole_edges off -- the same db::RegionCheckOptions the
    # C++ guard uses. The default (Euclidian) does not reach the code path.
    flat_prim, flat_sec = region(l1), region(l2)
    flat = flat_prim.enclosing_check(flat_sec, ENCLOSING_DBU, False, kdb.Region.Projection)

    dss = kdb.DeepShapeStore()
    dss.threads = 0
    deep_prim, deep_sec = region(l1, dss), region(l2, dss)
    deep = deep_prim.enclosing_check(deep_sec, ENCLOSING_DBU, False, kdb.Region.Projection)

    fs = sorted(str(e) for e in flat.each())
    ds = sorted(str(e) for e in deep.each())
    n_flat_frag = flat_prim.count()
    n_deep_frag = deep_prim.count()

    print("  subject fragments: flat=%d deep=%d  (max_vertex_count=%s)"
          % (n_flat_frag, n_deep_frag, dss.max_vertex_count))
    print("  enclosing_check edge pairs: flat=%d deep=%d" % (len(fs), len(ds)))

    rc = 0
    if n_deep_frag <= n_flat_frag:
        print("FAIL deep_split_line_probe: PRECONDITION NOT MET -- the "
              "DeepShapeStore did not split the subject polygon (deep=%d "
              "fragment(s), flat=%d), so 'no spurious split-line edge' is "
              "trivially true and this run proves nothing about the fix."
              % (n_deep_frag, n_flat_frag))
        return 1

    extra = [e for e in ds if e not in fs]
    lost = [e for e in fs if e not in ds]
    if extra or lost:
        rc = 1
        print("FAIL deep_split_line_probe: the deep result does not equal the "
              "flat reference -- a two-layer check on a DeepShapeStore-split "
              "polygon is reporting the cut line as a real edge "
              "(dbRegionLocalOperations.cc check_local_operation::override_distance).")
        for e in extra:
            print("    only in DEEP: %s" % e)
        for e in lost:
            print("    only in FLAT: %s" % e)
    if any(SPLIT_LINE in e for e in ds):
        rc = 1
        print("FAIL deep_split_line_probe: the split line y=1310 is present in "
              "the deep result as a subject edge")

    if rc == 0:
        print("PASS deep_split_line_probe (deep == flat on a split polygon; "
              "%d edge pair(s), split line absent)" % len(ds))
    return rc


if __name__ == "__main__":
    sys.exit(main())
