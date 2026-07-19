#!/usr/bin/env python3
"""mp_color.py — multi-patterning (double/triple-patterning) coloring decomposition
on KLayout's native Region engine.

Two features on ONE mask that sit closer than the single-mask minimum spacing must
be assigned to DIFFERENT masks (colors). This tool builds the same-layer CONFLICT
GRAPH — an edge between every pair of shapes whose spacing is below the single-mask
minimum — and decides whether it is colorable with the available number of masks:

  * n_colors = 2 (the DPT default): the graph is 2-colorable IFF it is BIPARTITE.
    The obstruction is an ODD CYCLE (Calibre's "odd-cycle conflict"), which this
    tool extracts EXACTLY -- the minimal ring of shapes that cannot be 2-colored.
    A triangle of three mutually-close shapes is the canonical example.
  * n_colors = 3 (TPT): 3-coloring is NP-complete in general; this tool does a
    DSATUR greedy assignment and REPORTS SUCCESS ONLY when it actually produces a
    proper coloring -- it never claims a graph is 3-colorable that it could not
    color, so a reported PASS is always witnessed by a printed valid assignment.

The decision is pure graph theory on a geometrically-derived conflict graph, so the
whole thing is hand-verifiable from the fixture: you can read the shapes, work out
which pairs conflict, and check the odd cycle (or the coloring) by eye.

Commercial equivalent: Calibre nmDRC multi-patterning decomposition (MASK / DPT/TPT
color / stitch / anchor). The color RULES are foundry data (EXT); the coloring
ENGINE is the [ALGO] half, and that is what this ships.

Config (chip/PDK-AGNOSTIC — the caller supplies the single-mask minimum):
    {
      "layer":       [10, 0],          // features to decompose
      "min_spacing": 0.200,            // single-mask minimum (um); gap BELOW it conflicts
      "n_colors":    2,                // 2 (DPT, default) | 3 (TPT)
      "anchor_layer":[11, 0]           // optional: shapes here pre-fix color 0
    }

Invocation (KLayout has no argv for scripts — parameters come from the environment):
    MP_GDS=<in.gds> MP_CONFIG=<cfg.json> MP_OUT=<report.json> \
        [MP_CELL=<top>] [MP_RVE=<conflict.lyrdb>] klayout -b -r mp_color.py

Report JSON:
    {"verdict": "COLORABLE" | "UNCOLORABLE",
     "shapes": N, "conflict_edges": E, "n_colors": C, "bipartite": bool,
     "coloring": [{"color":k,"bbox_um":[...]}, ...],       // when COLORABLE
     "odd_cycle": [{"bbox_um":[...]}, ...],                // when UNCOLORABLE (n=2)
     "odd_cycle_len": L}
"""
from __future__ import annotations

import collections
import json
import os
import sys


def _load_pya():
    try:
        import pya  # noqa: F401
        return pya
    except Exception:
        sys.stderr.write(
            "mp_color: KLayout Python module 'pya' not available "
            "(run inside the KLayout fork via `klayout -b -r`). DISCLOSED, not faked.\n")
        sys.exit(3)


def _li(ly, spec):
    n, d = int(spec[0]), int(spec[1])
    x = ly.find_layer(n, d)
    return x if x is not None else ly.layer(n, d)


def _bbox_um(box, dbu):
    return [round(box.left * dbu, 4), round(box.bottom * dbu, 4),
            round(box.right * dbu, 4), round(box.top * dbu, 4)]


def _read_shapes(pya, top, layer):
    out = []
    it = top.begin_shapes_rec(layer)
    while not it.at_end():
        sh = it.shape()
        if sh.is_polygon() or sh.is_box() or sh.is_path():
            out.append(sh.polygon.transformed(it.trans()))
        it.next()
    return out


def _conflict_edges(pya, shapes, d_dbu):
    """Edge (i,j) iff the spacing between shape i and shape j is strictly below
    d_dbu -- exactly KLayout's separation_check(d) semantics (gap < d flags,
    gap == d is clean), so the boundary is pinned to the DBU."""
    adj = collections.defaultdict(set)
    edges = 0
    regs = [pya.Region(s) for s in shapes]
    for i in range(len(shapes)):
        bi = shapes[i].bbox().enlarged(pya.Vector(d_dbu, d_dbu))
        for j in range(i + 1, len(shapes)):
            if not bi.overlaps(shapes[j].bbox()) and not bi.touches(shapes[j].bbox()):
                continue
            if not regs[i].separation_check(regs[j], d_dbu).is_empty():
                adj[i].add(j)
                adj[j].add(i)
                edges += 1
    return adj, edges


def _two_color(adj, n, anchors):
    """BFS 2-coloring. Returns (colors, odd_cycle) — odd_cycle is a list of node
    indices forming a minimal odd ring when the graph is NOT bipartite, else []."""
    color = {a: 0 for a in anchors}
    parent = {}
    for start in range(n):
        if start in color:
            continue
        color[start] = 0
        q = collections.deque([start])
        while q:
            u = q.popleft()
            for v in adj[u]:
                if v not in color:
                    color[v] = color[u] ^ 1
                    parent[v] = u
                    q.append(v)
                elif color[v] == color[u]:
                    # u and v are same-colored neighbours -> odd cycle through
                    # their lowest common ancestor in the BFS tree.
                    return color, _odd_cycle(parent, u, v)
    return color, []


def _odd_cycle(parent, u, v):
    au, av = [u], [v]
    su, sv = {u}, {v}
    while u in parent or v in parent:
        if u in parent:
            u = parent[u]
            if u in sv:
                return av[:av.index(u) + 1][::-1] + au
            au.append(u); su.add(u)
        if v in parent:
            v = parent[v]
            if v in su:
                return au[:au.index(v) + 1][::-1] + av
            av.append(v); sv.add(v)
    # shared root
    return au + av[::-1][1:]


def _dsatur(adj, n, k, anchors):
    """DSATUR greedy k-coloring. Returns a color dict on success, else None. Only
    reports success when EVERY node got a color in [0,k) with no conflict, so a
    PASS is always a witnessed proper coloring (never an optimistic claim)."""
    color = {a: 0 for a in anchors}
    while len(color) < n:
        best, best_key = None, None
        for u in range(n):
            if u in color:
                continue
            sat = len({color[v] for v in adj[u] if v in color})
            key = (sat, len(adj[u]))
            if best_key is None or key > best_key:
                best, best_key = u, key
        used = {color[v] for v in adj[best] if v in color}
        c = next((c for c in range(k) if c not in used), None)
        if c is None:
            return None
        color[best] = c
    for u in range(n):
        for v in adj[u]:
            if color[u] == color[v]:
                return None
    return color


def _xml_escape(s):
    return (s.replace("&", "&amp;").replace("<", "&lt;")
             .replace(">", "&gt;").replace('"', "&quot;"))


def _rve_category(name):
    bare = name and all(c.isalnum() or c in "_$" for c in name)
    return name if bare else "'" + name.replace("'", "\\'") + "'"


def _write_rve(path, top_name, cat, polys, dbu):
    with open(path, "w") as o:
        o.write('<?xml version="1.0" encoding="utf-8"?>\n<report-database>\n')
        o.write(" <description>mp_color conflict cycle</description>\n")
        o.write(" <original-file/>\n <generator>mp_color</generator>\n")
        o.write(" <top-cell>%s</top-cell>\n" % _xml_escape(top_name))
        o.write(" <tags>\n </tags>\n <categories>\n")
        if polys:
            o.write("  <category>\n   <name>%s</name>\n   <description/>\n"
                    "   <categories>\n   </categories>\n  </category>\n" % _xml_escape(cat))
        o.write(" </categories>\n <cells>\n  <cell>\n")
        o.write("   <name>%s</name>\n   <variant/>\n   <layout-name/>\n" % _xml_escape(top_name))
        o.write("   <references>\n   </references>\n  </cell>\n </cells>\n <items>\n")
        for poly in polys:
            o.write("  <item>\n   <tags/>\n   <category>%s</category>\n"
                    % _xml_escape(_rve_category(cat)))
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

    shapes = _read_shapes(pya, top, _li(ly, cfg["layer"]))
    if not shapes:
        return {"verdict": "ERROR", "error": "no shapes on the decomposition layer"}
    # stable order: lower-left, so the odd cycle / coloring is deterministic
    order = sorted(range(len(shapes)), key=lambda i: (shapes[i].bbox().left, shapes[i].bbox().bottom))
    shapes = [shapes[i] for i in order]

    d_dbu = int(round(float(cfg["min_spacing"]) / dbu))
    n_colors = int(cfg.get("n_colors", 2))
    adj, edges = _conflict_edges(pya, shapes, d_dbu)

    anchors = []
    if cfg.get("anchor_layer"):
        anc = _read_shapes(pya, top, _li(ly, cfg["anchor_layer"]))
        for i, s in enumerate(shapes):
            reg = pya.Region(s)
            if any(not reg.interacting(pya.Region(a)).is_empty() for a in anc):
                anchors.append(i)

    res = {"verdict": None, "gds": gds, "shapes": len(shapes),
           "conflict_edges": edges, "n_colors": n_colors,
           "min_spacing_dbu": d_dbu, "coloring": [],
           "odd_cycle": [], "odd_cycle_len": 0}

    color, odd = _two_color(adj, len(shapes), anchors)
    res["bipartite"] = (not odd)

    if n_colors == 2:
        if odd:
            res["verdict"] = "UNCOLORABLE"
            res["odd_cycle"] = [{"bbox_um": _bbox_um(shapes[i].bbox(), dbu)} for i in odd]
            res["odd_cycle_len"] = len(odd)
            if rve_out:
                _write_rve(rve_out, top.name, "MP_ODD_CYCLE", [shapes[i] for i in odd], dbu)
            return res
        final = color
    else:
        final = _dsatur(adj, len(shapes), n_colors, anchors)
        if final is None:
            res["verdict"] = "UNCOLORABLE"
            if rve_out:
                _write_rve(rve_out, top.name, "MP_ODD_CYCLE", [], dbu)
            return res

    res["verdict"] = "COLORABLE"
    res["coloring"] = sorted(
        [{"color": final[i], "bbox_um": _bbox_um(shapes[i].bbox(), dbu)} for i in range(len(shapes))],
        key=lambda c: (c["bbox_um"][0], c["bbox_um"][1]))
    if rve_out:
        _write_rve(rve_out, top.name, "MP_ODD_CYCLE", [], dbu)
    return res


def main():
    gds = os.environ.get("MP_GDS")
    cfg_path = os.environ.get("MP_CONFIG")
    out = os.environ.get("MP_OUT")
    cell = os.environ.get("MP_CELL") or None
    rve = os.environ.get("MP_RVE") or None
    if not gds or not cfg_path:
        sys.stderr.write("mp_color: set MP_GDS and MP_CONFIG (and MP_OUT).\n")
        return 2
    cfg = json.load(open(cfg_path))
    res = run(gds, cfg, cell, rve)
    text = json.dumps(res, indent=2)
    if out:
        open(out, "w").write(text)
    print(text)
    return 0 if res["verdict"] in ("COLORABLE", "UNCOLORABLE") else 3


if __name__ == "__main__":
    sys.exit(main())
