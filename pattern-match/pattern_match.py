#!/usr/bin/env python3
"""pattern_match.py — geometric pattern matching on KLayout's native Region engine.

Compiles a PATTERN (a single connected shape drawn on a pattern layer, or read from
a pattern GDS) into a translation-invariant signature and finds every EXACT
occurrence of it on a target layer of the design — the "known-bad shape" DFM
hotspot primitive the SVRF/DRC deck lacked. Matching is exact geometric congruence:
a candidate matches iff, after normalising its bbox lower-left to the origin, it is
identical to the pattern (optionally under any of the 8 rigid orientations —
4 rotations x mirror). A perturbed copy (one moved vertex) does NOT match, so the
gate is not vacuous.

Commercial equivalent: Calibre Pattern Matching / DFM hotspot library match.

Config (chip/PDK-AGNOSTIC — layer numbers supplied by the caller):
    {
      "target_layer":  [10, 0],           // layer to search
      "pattern_layer": [20, 0],           // layer holding the ONE reference shape
      "orientations":  "translation"      // "translation" (default) | "rigid" (all 8)
    }
  (Alternatively "pattern_gds":"file.gds" + "pattern_cell" reads the reference from
   a separate GDS; its first polygon on "pattern_layer" is the pattern.)

Invocation (KLayout has no argv for scripts — parameters come from the environment):
    PM_GDS=<in.gds> PM_CONFIG=<cfg.json> PM_OUT=<report.json> [PM_CELL=<top>] \
        klayout -b -r pattern_match.py

Output report JSON: {"pattern_vertices":N, "orientations":.., "matches":M,
"locations":[{"bbox_um":[l,b,r,t]}...]}.
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
            "pattern_match: KLayout Python module 'pya' not available "
            "(run inside the KLayout fork via `klayout -b -r`). DISCLOSED, not faked.\n")
        sys.exit(3)


def _li(ly, spec):
    n, d = int(spec[0]), int(spec[1])
    x = ly.find_layer(n, d)
    return x if x is not None else ly.layer(n, d)


def _norm(poly):
    """Normalise a polygon so its bbox lower-left sits at the origin (translation
    invariant). Returns a canonical pya.Polygon for exact == comparison."""
    b = poly.bbox()
    return poly.moved(-b.left, -b.bottom)


def _pattern_signatures(pya, patt, orientations):
    """The set of normalised polygons the pattern is allowed to match. For
    'translation' it is just the pattern; for 'rigid' it is the pattern under all
    8 rigid transforms (rot 0/90/180/270 x mirror), each re-normalised."""
    sigs = []
    if orientations == "rigid":
        seen = set()
        for mirror in (False, True):
            for rot in (0, 1, 2, 3):
                t = pya.Trans(rot, mirror, 0, 0)
                n = _norm(patt.transformed(t))
                key = n.to_s()
                if key not in seen:
                    seen.add(key)
                    sigs.append(n)
    else:
        sigs.append(_norm(patt))
    return sigs


def run(gds, cfg, cell_name=None):
    pya = _load_pya()
    ly = pya.Layout()
    ly.read(gds)
    dbu = ly.dbu
    top = ly.cell(cell_name) if cell_name else ly.top_cell()
    if top is None:
        return {"verdict": "ERROR", "error": f"top cell not found: {cell_name}"}

    orientations = cfg.get("orientations", "translation")
    tgt = pya.Region(top.begin_shapes_rec(_li(ly, cfg["target_layer"]))).merged()

    # the reference pattern: from a separate GDS, else from the pattern layer here.
    if cfg.get("pattern_gds"):
        ply = pya.Layout()
        ply.read(cfg["pattern_gds"])
        pcell = ply.cell(cfg["pattern_cell"]) if cfg.get("pattern_cell") else ply.top_cell()
        preg = pya.Region(pcell.begin_shapes_rec(_li(ply, cfg["pattern_layer"]))).merged()
    else:
        preg = pya.Region(top.begin_shapes_rec(_li(ly, cfg["pattern_layer"]))).merged()

    pats = [p for p in preg.each()]
    if len(pats) != 1:
        return {"verdict": "ERROR",
                "error": f"pattern layer must hold exactly ONE shape, got {len(pats)}"}
    patt = pats[0]
    sigs = _pattern_signatures(pya, patt, orientations)
    sig_keys = {s.to_s() for s in sigs}

    matches = []
    for cand in tgt.each():
        if _norm(cand).to_s() in sig_keys:
            b = cand.bbox()
            matches.append({"bbox_um": [round(b.left * dbu, 4), round(b.bottom * dbu, 4),
                                        round(b.right * dbu, 4), round(b.top * dbu, 4)]})
    matches.sort(key=lambda m: (m["bbox_um"][0], m["bbox_um"][1]))
    return {"verdict": "OK",
            "gds": gds,
            "pattern_vertices": patt.num_points(),
            "orientations": orientations,
            "matches": len(matches),
            "locations": matches}


def main():
    gds = os.environ.get("PM_GDS")
    cfg_path = os.environ.get("PM_CONFIG")
    out = os.environ.get("PM_OUT")
    cell = os.environ.get("PM_CELL") or None
    if not gds or not cfg_path:
        sys.stderr.write("pattern_match: set PM_GDS and PM_CONFIG (and PM_OUT).\n")
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
