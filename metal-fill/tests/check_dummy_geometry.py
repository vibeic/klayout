#!/usr/bin/env python3
"""check_dummy_geometry.py — assert the DUMMY-datatype fill's geometry on the STREAM.

The report is the engine's own account of what it did; this reads the GDS it actually
wrote. Run under KLayout (`strmrun` / `klayout -b -r`).

    DUMMY_BEFORE=<pre-fill.gds> DUMMY_AFTER=<filled.gds> DUMMY_REPORT=<report.json> \
        strmrun check_dummy_geometry.py

Prints `GEOM-OK` on success; anything else means a failed assertion.
"""
import json
import os
import sys

DRAWN = (34, 0)
DUMMY = (34, 4)


def _count(cell, lidx):
    return sum(1 for _ in cell.begin_shapes_rec(lidx))


def main():
    import pya
    before_p = os.environ["DUMMY_BEFORE"]
    after_p = os.environ["DUMMY_AFTER"]
    rep = json.load(open(os.environ["DUMMY_REPORT"]))
    spec = rep["layers"][0]
    grid_um = rep["mfg_grid_um"]

    lb = pya.Layout()
    lb.read(before_p)
    tb = lb.top_cell()
    n_drawn_before = _count(tb, lb.layer(*DRAWN))

    la = pya.Layout()
    la.read(after_p)
    ta = la.top_cell()
    dbu = la.dbu
    n_drawn_after = _count(ta, la.layer(*DRAWN))
    dummy = pya.Region(ta.begin_shapes_rec(la.layer(*DUMMY)))
    drawn = pya.Region(ta.begin_shapes_rec(la.layer(*DRAWN))).merged()
    n_dummy = dummy.count()

    # [3] LVS-invisibility: the drawn layer is untouched by the fill.
    if n_drawn_after != n_drawn_before:
        print(f"FAIL [3] drawn layer {DRAWN} changed: {n_drawn_before} -> {n_drawn_after}")
        return 1
    if n_dummy <= 0:
        print(f"FAIL [3] no shapes on the dummy layer {DUMMY}")
        return 1
    if n_dummy != spec["fill_shapes"]:
        print(f"FAIL [3] report claims {spec['fill_shapes']} fill shapes, stream has {n_dummy}")
        return 1
    print(f"  [3] LVS-invisible: drawn {DRAWN} unchanged at {n_drawn_after} shapes; "
          f"{n_dummy} dummy shapes on {DUMMY} OK")

    # [4] every dummy edge on the manufacturing grid.
    g = int(round(grid_um / dbu))
    if g <= 0:
        print(f"FAIL [4] manufacturing grid {grid_um} smaller than one dbu {dbu}")
        return 1
    off = 0
    for p in dummy.each():
        for pt in p.each_point_hull():
            if pt.x % g or pt.y % g:
                off += 1
    if off:
        print(f"FAIL [4] {off} dummy-fill vertices off the {grid_um}um manufacturing grid")
        return 1
    print(f"  [4] all {n_dummy} dummy fills on the {grid_um}um manufacturing grid OK")

    # [5] dummy-to-circuit clearance >= space_to_metal (square/Chebyshev metric, the
    #     same one the engine sizes with), plus a NEGATIVE control that the probe fires.
    spm = int(round(spec["space_to_metal_um"] / dbu))
    if not (drawn.sized(spm - 1) & dummy).is_empty():
        print(f"FAIL [5] a dummy fill sits closer than space_to_metal "
              f"({spec['space_to_metal_um']}um) to circuit metal")
        return 1
    probe = 4 * spm
    if (drawn.sized(probe) & dummy).is_empty():
        print(f"FAIL [5] negative control: NO dummy fill within {probe * dbu}um of circuit "
              f"metal either -- the proximity probe cannot fire, so the check is vacuous")
        return 1
    print(f"  [5] dummy-to-circuit clearance >= {spec['space_to_metal_um']}um "
          f"(and the probe fires at {probe * dbu}um, so not vacuous) OK")

    print("GEOM-OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
