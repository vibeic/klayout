#!/usr/bin/env python3
"""cmp_gradient.py — CMP density-gradient (planarity) checker on KLayout's native
Region engine.

A forkable proxy for Calibre CMPAnalyzer's planarity concern: after chemical-
mechanical polishing, a LARGE STEP in metal density between neighbouring regions
causes dishing/erosion (the polish rate depends on local density). The full CMP
model is foundry-calibrated (EXT); the density-GRADIENT checker -- window-to-window
delta -- is a cheap, geometry-derivable proxy, and that is what this ships.

The tool lays a regular grid of windows over the layer, measures metal density in
each window (metal area / window area) EXACTLY on KLayout's Region engine, and
flags any pair of ROOK-ADJACENT windows whose density delta exceeds max_gradient.
It reports the worst gradient and the offending window pair, so the number is
hand-checkable from the fixture.

Config (chip/PDK-AGNOSTIC — the caller supplies the CMP rule values):
    {
      "layer":        [10, 0],
      "window_um":    10.0,        // square analysis window side
      "step_um":      10.0,        // grid step (== window_um -> non-overlapping tiles)
      "max_gradient": 0.30,        // allowed |density_i - density_j| between neighbours
      "origin_um":    [0, 0]       // optional grid origin (default the layer bbox min)
    }

Invocation (KLayout has no argv — parameters come from the environment):
    CG_GDS=<in.gds> CG_CONFIG=<cfg.json> CG_OUT=<report.json> \
        [CG_CELL=<top>] klayout -b -r cmp_gradient.py

Report JSON:
    {"verdict":"PASS"|"FAIL","max_gradient":..,"max_allowed":..,
     "worst":{"a":{"col","row","density"},"b":{...},"delta":..},
     "windows":[{"col","row","density","bbox_um":[...]}...],
     "violations":[{"a":..,"b":..,"delta":..}...]}
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
            "cmp_gradient: KLayout Python module 'pya' not available "
            "(run inside the KLayout fork via `klayout -b -r`). DISCLOSED, not faked.\n")
        sys.exit(3)


def _li(ly, spec):
    n, d = int(spec[0]), int(spec[1])
    x = ly.find_layer(n, d)
    return x if x is not None else ly.layer(n, d)


def run(gds, cfg, cell_name=None):
    pya = _load_pya()
    ly = pya.Layout()
    ly.read(gds)
    dbu = ly.dbu
    top = ly.cell(cell_name) if cell_name else ly.top_cell()
    if top is None:
        return {"verdict": "ERROR", "error": "top cell not found: %s" % cell_name}

    metal = pya.Region(top.begin_shapes_rec(_li(ly, cfg["layer"]))).merged()
    if metal.is_empty():
        return {"verdict": "ERROR", "error": "no metal geometry on the layer"}

    win = int(round(float(cfg["window_um"]) / dbu))
    step = int(round(float(cfg.get("step_um", cfg["window_um"])) / dbu))
    max_grad = float(cfg["max_gradient"])
    if cfg.get("origin_um"):
        ox = int(round(cfg["origin_um"][0] / dbu))
        oy = int(round(cfg["origin_um"][1] / dbu))
    else:
        bb = metal.bbox()
        ox, oy = bb.left, bb.bottom

    bb = metal.bbox()
    ncol = max(1, -(-(bb.right - ox) // step))    # ceil division
    nrow = max(1, -(-(bb.top - oy) // step))
    warea = float(win) * float(win)

    dens = {}
    windows = []
    for r in range(nrow):
        for c in range(ncol):
            x0 = ox + c * step
            y0 = oy + r * step
            wb = pya.Box(x0, y0, x0 + win, y0 + win)
            a = (metal & pya.Region(wb)).area()
            d = a / warea
            dens[(c, r)] = d
            windows.append({"col": c, "row": r, "density": round(d, 6),
                            "bbox_um": [round(x0 * dbu, 4), round(y0 * dbu, 4),
                                        round((x0 + win) * dbu, 4), round((y0 + win) * dbu, 4)]})

    # rook adjacency (right + up neighbour of each window, so each pair once)
    violations = []
    worst = None
    for (c, r), d in dens.items():
        for cc, rr in ((c + 1, r), (c, r + 1)):
            if (cc, rr) not in dens:
                continue
            delta = abs(d - dens[(cc, rr)])
            pair = {"a": {"col": c, "row": r, "density": round(d, 6)},
                    "b": {"col": cc, "row": rr, "density": round(dens[(cc, rr)], 6)},
                    "delta": round(delta, 6)}
            if worst is None or delta > worst["delta"]:
                worst = pair
            #  strict '>' with a 1e-9 guard: densities resolve to ~1e-3 (a DBU of
            #  strip width in a 10 um window), so this only absorbs float noise
            #  (0.8 - 0.5 == 0.30000000000000004) and never blurs the real
            #  boundary (a 0.30 gradient PASSes, 0.301 FAILs).
            if delta > max_grad + 1e-9:
                violations.append(pair)

    verdict = "PASS" if not violations else "FAIL"
    return {"verdict": verdict, "gds": gds,
            "window_um": cfg["window_um"], "step_um": cfg.get("step_um", cfg["window_um"]),
            "max_allowed": max_grad,
            "max_gradient": round(worst["delta"], 6) if worst else 0.0,
            "worst": worst, "n_windows": len(windows), "windows": windows,
            "violations": violations}


def main():
    gds = os.environ.get("CG_GDS")
    cfg_path = os.environ.get("CG_CONFIG")
    out = os.environ.get("CG_OUT")
    cell = os.environ.get("CG_CELL") or None
    if not gds or not cfg_path:
        sys.stderr.write("cmp_gradient: set CG_GDS and CG_CONFIG (and CG_OUT).\n")
        return 2
    cfg = json.load(open(cfg_path))
    res = run(gds, cfg, cell)
    text = json.dumps(res, indent=2)
    if out:
        open(out, "w").write(text)
    print(text)
    return 0 if res["verdict"] in ("PASS", "FAIL") else 3


if __name__ == "__main__":
    sys.exit(main())
