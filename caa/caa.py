#!/usr/bin/env python3
"""caa.py — Critical Area Analysis (random-particle-defect yield) on KLayout's
native Region engine.

The [ALGO] half of Calibre YieldAnalyzer CAA: the critical-area GEOMETRY. For a
circular defect of diameter x (radius r = x/2), two conductors are SHORTED when
the defect touches both, i.e. when its centre lies in

    Ac_short(x) = area( grow(A, r) ∩ grow(B, r) )

(the OPEN critical area has the same L*(x - w) form with the wire's own two facing
edges in place of A and B; this tool ships the SHORT engine, which is the
dominant defect mechanism and the one with an exact closed form here.)

This tool ships that geometry engine and TAKES THE DEFECT-DENSITY DISTRIBUTION AS
A PARAMETER (the [EXT] half). For the canonical inverse-cube law

    D(x) = D0 * 2*x0^2 / x^3          (x >= x0),   ∫ D(x) dx = D0

the weighted (average) critical area is integrated EXACTLY: Ac(x) is piecewise
linear in x for straight facing edges, so between two measured samples the tool
uses the closed form  ∫ (a + b x) * 2 x0^2 / x^3 dx = 2 x0^2 [ -a/(2x^2) - b/x ],
plus an analytic tail beyond the last sample using its slope. The result is the
AWC (average number of faults per unit defect density) that feeds a yield model.

For two parallel facing edges of length W at spacing s (with x0 <= s), the exact
closed form is  AWC_short = W * D0 * x0^2 / s  -- a hand-checkable number, and the
per-diameter Ac_short(x) = W*(x - s) is exact to the DBU.

Config (chip/PDK-AGNOSTIC — the caller supplies the defect model):
    {
      "mode":       "short",              // "short" (A vs B)
      "layer_a":    [10, 0],
      "layer_b":    [11, 0],              // the other conductor
      "window_um":  [0, -5, 10, 10],      // optional interior measurement window
      "defect":     {"x0": 0.1, "d0": 1.0, "law": "inverse_cube"},
      "x_max_um":   4.0,                  // sweep upper bound (um)
      "x_step_um":  0.05                  // sweep step (um)
    }

Invocation (KLayout has no argv — parameters come from the environment):
    CAA_GDS=<in.gds> CAA_CONFIG=<cfg.json> CAA_OUT=<report.json> \
        [CAA_CELL=<top>] klayout -b -r caa.py

Report JSON:
    {"verdict":"OK","mode":..,"awc_um2":..,
     "per_diameter":[{"x_um":..,"ac_um2":..}, ...]}
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
            "caa: KLayout Python module 'pya' not available "
            "(run inside the KLayout fork via `klayout -b -r`). DISCLOSED, not faked.\n")
        sys.exit(3)


def _li(ly, spec):
    n, d = int(spec[0]), int(spec[1])
    x = ly.find_layer(n, d)
    return x if x is not None else ly.layer(n, d)


def _region(pya, top, layer):
    return pya.Region(top.begin_shapes_rec(layer)).merged()


def _ac_short(pya, A, B, r_dbu, window):
    """Critical area for a short by a defect of radius r_dbu: the locus of defect
    centres within r of BOTH A and B = grow(A,r) AND grow(B,r). Uses sized() with
    the default (mitred) corners; the caller clips to an interior window so the
    facing-edge overlap is measured without end/corner effects."""
    if r_dbu <= 0:
        return 0
    ga = A.sized(r_dbu)
    gb = B.sized(r_dbu)
    inter = ga & gb
    if window is not None:
        inter &= window
    return inter.area()


def _awc_inverse_cube(samples, x0, d0):
    """EXACT weighted integral of a piecewise-linear Ac(x) against
    D(x) = d0 * 2 x0^2 / x^3 over [x0, inf). `samples` = sorted [(x, ac)] in um^2.
    Ac is linear between straight-facing-edge samples, so per slice
        ∫ (a + b x) * 2 x0^2 / x^3 dx = 2 x0^2 [ -a/(2 x^2) - b/x ].
    The tail beyond the last sample continues with the last slope b."""
    k = 2.0 * x0 * x0 * d0
    total = 0.0
    for (x1, y1), (x2, y2) in zip(samples, samples[1:]):
        if x2 <= x1:
            continue
        b = (y2 - y1) / (x2 - x1)
        a = y1 - b * x1
        F = lambda x: -a / (2.0 * x * x) - b / x
        total += k * (F(x2) - F(x1))
    # analytic tail [x_last, inf): Ac keeps the final slope, F(inf) -> 0
    if len(samples) >= 2:
        (x1, y1), (x2, y2) = samples[-2], samples[-1]
        b = (y2 - y1) / (x2 - x1) if x2 > x1 else 0.0
        a = y2 - b * x2
        Ftail = lambda x: -a / (2.0 * x * x) - b / x
        total += k * (0.0 - Ftail(x2))
    return total


def run(gds, cfg, cell_name=None):
    pya = _load_pya()
    ly = pya.Layout()
    ly.read(gds)
    dbu = ly.dbu
    top = ly.cell(cell_name) if cell_name else ly.top_cell()
    if top is None:
        return {"verdict": "ERROR", "error": "top cell not found: %s" % cell_name}

    mode = cfg.get("mode", "short")
    if mode != "short":
        return {"verdict": "ERROR", "error": "only mode 'short' is supported"}
    A = _region(pya, top, _li(ly, cfg["layer_a"]))
    if not cfg.get("layer_b"):
        return {"verdict": "ERROR", "error": "short mode needs layer_b"}
    B = _region(pya, top, _li(ly, cfg["layer_b"]))

    window = None
    if cfg.get("window_um"):
        w = cfg["window_um"]
        window = pya.Region(pya.Box(int(round(w[0] / dbu)), int(round(w[1] / dbu)),
                                    int(round(w[2] / dbu)), int(round(w[3] / dbu))))

    defect = cfg.get("defect", {})
    x0 = float(defect.get("x0", 0.1))
    d0 = float(defect.get("d0", 1.0))
    law = defect.get("law", "inverse_cube")
    x_max = float(cfg.get("x_max_um", 4.0))
    x_step = float(cfg.get("x_step_um", 0.05))

    # sweep defect diameter x from x0 to x_max
    samples = []
    per = []
    x = x0
    xs = []
    while x <= x_max + 1e-9:
        xs.append(round(x, 6))
        x += x_step
    for xv in xs:
        r_dbu = int(round((xv / 2.0) / dbu))
        ac_dbu2 = _ac_short(pya, A, B, r_dbu, window)
        ac_um2 = ac_dbu2 * dbu * dbu
        samples.append((xv, ac_um2))
        per.append({"x_um": xv, "ac_um2": round(ac_um2, 6)})

    if law != "inverse_cube":
        return {"verdict": "ERROR", "error": "unsupported defect law: %s" % law}
    awc = _awc_inverse_cube(samples, x0, d0)

    return {"verdict": "OK", "gds": gds, "mode": mode,
            "x0_um": x0, "d0": d0, "x_max_um": x_max, "x_step_um": x_step,
            "awc_um2": round(awc, 6), "per_diameter": per}


def main():
    gds = os.environ.get("CAA_GDS")
    cfg_path = os.environ.get("CAA_CONFIG")
    out = os.environ.get("CAA_OUT")
    cell = os.environ.get("CAA_CELL") or None
    if not gds or not cfg_path:
        sys.stderr.write("caa: set CAA_GDS and CAA_CONFIG (and CAA_OUT).\n")
        return 2
    cfg = json.load(open(cfg_path))
    res = run(gds, cfg, cell)
    text = json.dumps(res, indent=2)
    if out:
        open(out, "w").write(text)
    print(text)
    return 0 if res["verdict"] == "OK" else 3


if __name__ == "__main__":
    sys.exit(main())
