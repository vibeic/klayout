#!/usr/bin/env python3
"""perc_latchup.py — geometry-derivable latch-up guard-ring checks (PERC-lite) on
KLayout's native Region engine.

The forkable [ALGO] half of Calibre PERC's latch-up guard-ring family: the checks
that are decidable from GEOMETRY alone, with no schematic and no device netlist.
For every sensitive device (a diffusion/active shape) the tool verifies that it is
protected by a substrate/well tap guard ring:

  * DISTANCE  — a tap must sit within max_dist of the device (well/substrate must
                be tied down close enough to quench the parasitic SCR). A device
                with no tap within max_dist is a violation.
  * WIDTH     — every guard-ring tap must be at least min_width wide (a too-thin
                ring has too much resistance to be an effective tie).
  * ENCLOSURE — the tap must SURROUND the device: the device must lie inside a
                HOLE of the merged tap geometry (a full ring has a hole; a tap on
                one side only does not, so a bar fails while a ring passes).

The RULE VALUES (max_dist, min_width) are foundry data (EXT); the geometric
decision is the [ALGO] half, and that is what this ships. Pure additive Python on
KLayout's Region engine -- 0 engine change.

Config (chip/PDK-AGNOSTIC — the caller supplies the rule values):
    {
      "device_layer": [12, 0],       // sensitive active/diffusion
      "tap_layer":    [13, 0],       // guard-ring / well-substrate tap
      "max_dist_um":  1.0,           // tap must be within this of the device
      "min_width_um": 0.4,           // guard-ring minimum width
      "checks": ["distance","width","enclosure"]   // optional subset
    }

Invocation (KLayout has no argv — parameters come from the environment):
    PL_GDS=<in.gds> PL_CONFIG=<cfg.json> PL_OUT=<report.json> \
        [PL_CELL=<top>] [PL_RVE=<viol.lyrdb>] klayout -b -r perc_latchup.py

Report JSON:
    {"verdict":"PASS"|"FAIL",
     "distance":{"verdict":..,"violations":N,"markers":[{"bbox_um":[...]}]},
     "width":   {"verdict":..,"violations":N,"markers":[...]},
     "enclosure":{"verdict":..,"violations":N,"markers":[...]}}
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
            "perc_latchup: KLayout Python module 'pya' not available "
            "(run inside the KLayout fork via `klayout -b -r`). DISCLOSED, not faked.\n")
        sys.exit(3)


def _li(ly, spec):
    n, d = int(spec[0]), int(spec[1])
    x = ly.find_layer(n, d)
    return x if x is not None else ly.layer(n, d)


def _region(pya, top, layer):
    return pya.Region(top.begin_shapes_rec(layer)).merged()


def _bbox_um(box, dbu):
    return [round(box.left * dbu, 4), round(box.bottom * dbu, 4),
            round(box.right * dbu, 4), round(box.top * dbu, 4)]


def _markers(region, dbu):
    out = [{"bbox_um": _bbox_um(p.bbox(), dbu)} for p in region.each()]
    out.sort(key=lambda m: (m["bbox_um"][0], m["bbox_um"][1]))
    return out


def _xml_escape(s):
    return (s.replace("&", "&amp;").replace("<", "&lt;")
             .replace(">", "&gt;").replace('"', "&quot;"))


def _rve_category(name):
    bare = name and all(c.isalnum() or c in "_$" for c in name)
    return name if bare else "'" + name.replace("'", "\\'") + "'"


def _write_rve(path, top_name, cats, dbu):
    """cats: list of (category_name, region). Emits one category per non-empty
    region, one item per polygon -- a valid empty DB when all are empty."""
    with open(path, "w") as o:
        o.write('<?xml version="1.0" encoding="utf-8"?>\n<report-database>\n')
        o.write(" <description>perc_latchup guard-ring violations</description>\n")
        o.write(" <original-file/>\n <generator>perc_latchup</generator>\n")
        o.write(" <top-cell>%s</top-cell>\n" % _xml_escape(top_name))
        o.write(" <tags>\n </tags>\n <categories>\n")
        for name, reg in cats:
            if not reg.is_empty():
                o.write("  <category>\n   <name>%s</name>\n   <description/>\n"
                        "   <categories>\n   </categories>\n  </category>\n" % _xml_escape(name))
        o.write(" </categories>\n <cells>\n  <cell>\n")
        o.write("   <name>%s</name>\n   <variant/>\n   <layout-name/>\n" % _xml_escape(top_name))
        o.write("   <references>\n   </references>\n  </cell>\n </cells>\n <items>\n")
        for name, reg in cats:
            for poly in reg.each():
                o.write("  <item>\n   <tags/>\n   <category>%s</category>\n"
                        % _xml_escape(_rve_category(name)))
                o.write("   <cell>%s</cell>\n   <visited>false</visited>\n"
                        "   <multiplicity>1</multiplicity>\n" % _xml_escape(top_name))
                o.write("   <values>\n    <value>polygon: %s</value>\n   </values>\n"
                        % poly.to_dtype(dbu).to_s())
                o.write("  </item>\n")
        o.write(" </items>\n</report-database>\n")


def run(gds, cfg, cell_name=None, rve_out=None):
    pya = _load_pya()
    ly = pya.Layout()
    ly.read(gds)
    dbu = ly.dbu
    top = ly.cell(cell_name) if cell_name else ly.top_cell()
    if top is None:
        return {"verdict": "ERROR", "error": "top cell not found: %s" % cell_name}

    dev = _region(pya, top, _li(ly, cfg["device_layer"]))
    tap = _region(pya, top, _li(ly, cfg["tap_layer"]))
    if dev.is_empty():
        return {"verdict": "ERROR", "error": "no device geometry found"}

    max_dist = int(round(float(cfg.get("max_dist_um", 1.0)) / dbu))
    min_width = int(round(float(cfg.get("min_width_um", 0.4)) / dbu))
    checks = cfg.get("checks", ["distance", "width", "enclosure"])

    res = {"verdict": "PASS", "gds": gds}
    rve_cats = []

    # DISTANCE: every device must have a tap within max_dist (gap <= max_dist OK).
    if "distance" in checks:
        grown = tap.sized(max_dist)
        close = dev.interacting(grown)
        toofar = dev - close                       # devices with no tap in reach
        n = toofar.count()
        res["distance"] = {"verdict": "PASS" if n == 0 else "FAIL",
                           "violations": n, "markers": _markers(toofar, dbu)}
        rve_cats.append(("LATCHUP_TAP_DIST", toofar))
        if n:
            res["verdict"] = "FAIL"

    # WIDTH: every guard-ring tap must be at least min_width wide.
    if "width" in checks:
        narrow = tap.width_check(min_width).polygons().merged()
        n = narrow.count()
        res["width"] = {"verdict": "PASS" if n == 0 else "FAIL",
                        "violations": n, "markers": _markers(narrow, dbu)}
        rve_cats.append(("LATCHUP_RING_WIDTH", narrow))
        if n:
            res["verdict"] = "FAIL"

    # ENCLOSURE: the tap must surround the device -- the device must lie inside a
    # HOLE of the merged tap (a full ring has a hole; a one-sided bar does not).
    if "enclosure" in checks:
        holes = tap.holes()
        enclosed = dev.interacting(holes) & dev     # devices touching a ring hole
        # a device is fully enclosed only if it lies ENTIRELY within a hole
        surrounded = pya.Region()
        for p in dev.each():
            pr = pya.Region(p)
            if not pr.interacting(holes).is_empty() and (pr - holes).is_empty():
                surrounded.insert(p)
        unprotected = dev - surrounded
        n = unprotected.count()
        res["enclosure"] = {"verdict": "PASS" if n == 0 else "FAIL",
                            "violations": n, "markers": _markers(unprotected, dbu)}
        rve_cats.append(("LATCHUP_NO_RING", unprotected))
        if n:
            res["verdict"] = "FAIL"

    if rve_out:
        _write_rve(rve_out, top.name, rve_cats, dbu)
    return res


def main():
    gds = os.environ.get("PL_GDS")
    cfg_path = os.environ.get("PL_CONFIG")
    out = os.environ.get("PL_OUT")
    cell = os.environ.get("PL_CELL") or None
    rve = os.environ.get("PL_RVE") or None
    if not gds or not cfg_path:
        sys.stderr.write("perc_latchup: set PL_GDS and PL_CONFIG (and PL_OUT).\n")
        return 2
    cfg = json.load(open(cfg_path))
    res = run(gds, cfg, cell, rve)
    text = json.dumps(res, indent=2)
    if out:
        open(out, "w").write(text)
    print(text)
    return 0 if res["verdict"] in ("PASS", "FAIL") else 3


if __name__ == "__main__":
    sys.exit(main())
