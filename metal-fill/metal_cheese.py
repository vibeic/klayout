#!/usr/bin/env python3
"""metal_cheese.py — DRC-safe cheesing / slotting of wide metal on KLayout's native
Region engine.

The inverse of ``metal_fill.py``: where fill RAISES a too-SPARSE layer's density,
cheesing LOWERS a too-DENSE (wide) shape's density by cutting a regular grid of
holes into it, so it satisfies a MAX-density (CMP / stress-relief) rule without
losing its outer footprint. This is the slotting/cheesing emitter the flow was
missing: it had a max-density *checker* (``svrfdrc DENSITY .. > max``) that
FLAGGED a solid wide power strap, but nothing that FIXED it.

Commercial equivalent: Calibre YieldEnhancer cheesing / slotting; ICC2 wide-metal
slotting for CMP planarity + stress relief.

DRC-safety (why the holes never create a width/notch fault)
-----------------------------------------------------------
* hole side ``hole`` is the etched slot: hole >= min_space  -> the metal *notch*
  across a hole is >= min_space (no sub-space enclosed gap)
* the hole grid pitch is ``hole + wall`` so the metal WALL left between two
  adjacent holes is exactly ``wall``; set ``wall >= min_width`` -> no width fault
* every hole is kept ONLY if it lies fully inside ``metal.sized(-wall)`` (the metal
  eroded inward by ``wall``), so a hole never comes within ``wall`` of the shape
  boundary -> the outer footprint (and thus spacing to neighbours) is untouched
The synthetic sign-off test drives the fork's OWN ``svrfdrc`` on the cheesed GDS
and asserts the max-density rule that FAILED now PASSES with 0 new width/notch
violations; a deliberately-too-thin wall IS caught, so the gate is not vacuous.

Density model (matches svrfdrc no-WINDOW DENSITY: denominator = design extent)
------------------------------------------------------------------------------
    density = metal_area / extent_area          (extent = layer bbox, preserved)
The tool cuts the maximal DRC-safe hole grid that fits and reports the achieved
density; if that still exceeds ``max`` (infeasible with the given hole/wall) it
says ``reached=false`` honestly rather than silently passing.

Config (chip/PDK-AGNOSTIC — layer numbers + generic geometry supplied by caller):
    {
      "layers": [
        {"name":"met1","layer":[10,0],"max":0.70,"hole":4.0,"wall":2.0}
      ]
    }
  hole = slot side (um, >= min_space);  wall = metal left between slots (um, >= min_width).

Invocation (KLayout has no argv for scripts — parameters come from the environment):
    CHEESE_GDS=<in.gds> CHEESE_CONFIG=<cfg.json> CHEESE_OUT=<out.gds> \
        CHEESE_REPORT=<report.json> [CHEESE_CELL=<top>] klayout -b -r metal_cheese.py

Output report JSON: per-layer {density_before, density_after, max, reached (bool),
holes, hole_um, wall_um}.
"""
from __future__ import annotations

import json
import os
import sys


def _load_pya():
    try:
        import pya  # noqa: F401
        return pya
    except Exception:
        sys.stderr.write(
            "metal_cheese: KLayout Python module 'pya' not available "
            "(run inside the KLayout fork via `klayout -b -r`). DISCLOSED, not faked.\n")
        sys.exit(3)


def _li(ly, spec):
    n, d = int(spec[0]), int(spec[1])
    x = ly.find_layer(n, d)
    return x if x is not None else ly.layer(n, d)


def cheese_layer(ly, top, spec):
    import pya
    dbu = ly.dbu
    lidx = _li(ly, spec["layer"])
    maxd = float(spec["max"])
    hole = float(spec["hole"])
    wall = float(spec["wall"])
    h = int(round(hole / dbu))
    w = int(round(wall / dbu))
    pitch = h + w

    metal = pya.Region(top.begin_shapes_rec(lidx)).merged()
    bbox = metal.bbox()
    ext_area = float(bbox.area())
    if ext_area <= 0:
        return {"name": spec["name"], "skipped": "empty bbox"}

    d_before = metal.area() / ext_area
    if d_before <= maxd:
        # already within the max-density budget -> leave the geometry untouched.
        return {"name": spec["name"], "max": maxd, "hole_um": hole, "wall_um": wall,
                "density_before": round(d_before, 6), "density_after": round(d_before, 6),
                "reached": True, "holes": 0, "changed": False}

    # holes may only land inside the metal eroded inward by `wall` -> the outer
    # footprint stays intact and every hole keeps >= wall of metal all around it.
    eroded = metal.sized(-w)

    # analytic grid over the bbox, offset by `wall` from the left/bottom, pitch =
    # hole+wall. A candidate hole is kept only if it lies fully inside `eroded`.
    holes = pya.Region()
    nkept = 0
    x = bbox.left + w
    while x + h <= bbox.right - w + 1:      # +1 dbu tolerance for the touching case
        y = bbox.bottom + w
        while y + h <= bbox.top - w + 1:
            cand = pya.Box(x, y, x + h, y + h)
            if (pya.Region(cand) - eroded).is_empty():
                holes.insert(cand)
                nkept += 1
            y += pitch
        x += pitch
    holes.merge()

    result = metal - holes
    # rewrite the layer: clear it in every cell, re-insert the cheesed region at top.
    for ci in ly.each_cell():
        ci.shapes(lidx).clear()
    for poly in result.each():
        top.shapes(lidx).insert(poly)

    d_after = result.area() / ext_area
    return {"name": spec["name"], "max": maxd, "hole_um": hole, "wall_um": wall,
            "density_before": round(d_before, 6), "density_after": round(d_after, 6),
            "reached": d_after <= maxd, "holes": nkept, "changed": nkept > 0}


def main():
    pya = _load_pya()
    gds = os.environ.get("CHEESE_GDS")
    cfg_path = os.environ.get("CHEESE_CONFIG")
    out = os.environ.get("CHEESE_OUT")
    rep = os.environ.get("CHEESE_REPORT")
    cell = os.environ.get("CHEESE_CELL")
    if not gds or not cfg_path or not out:
        sys.stderr.write("metal_cheese: need CHEESE_GDS, CHEESE_CONFIG, CHEESE_OUT\n")
        sys.exit(2)

    cfg = json.load(open(cfg_path))
    ly = pya.Layout()
    ly.read(gds)
    if cell:
        top = ly.cell(cell)
    else:
        tops = ly.top_cells()
        top = tops[0]

    results = [cheese_layer(ly, top, spec) for spec in cfg["layers"]]
    ly.write(out)

    report = {"gds": gds, "out": out, "layers": results}
    if rep:
        json.dump(report, open(rep, "w"), indent=2)
    print(json.dumps(report))


if __name__ == "__main__":
    main()
