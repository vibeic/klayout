#!/usr/bin/env python3
"""lvs_recon.py — short isolation / LVS recon on KLayout's native geometry engine.

Answers the question an LVS MISMATCH leaves open: *where* are these two nets
shorted? Given a layout, a CONNECT stack and two seed markers sitting on the two
nets that are supposed to be SEPARATE, this builds the shape-level adjacency graph
(one node per DRAWN polygon, not per merged net), path-traces the shortest
connection between the seeds, and then isolates the CULPRITS — the shapes whose
removal actually breaks the connection (graph cut vertices on the A-B paths).

The culprit test is the whole point, and it is honest in both directions:

  * a shape is reported ONLY if deleting it genuinely disconnects the two seeds,
    so the answer is checkable by construction ("delete this, re-extract, the
    short is gone");
  * when TWO independent bridges short the nets, NO single shape is a cut vertex,
    so the tool reports zero culprits and sets redundant_paths — it will not name
    an arbitrary shape off the path just to produce an answer.

The seed-attached shapes themselves (the rails being probed) are never culprits:
deleting the net you are probing is not a fix.

Commercial equivalent: Calibre LVS Recon short isolation.

Config (chip/PDK-AGNOSTIC — layer numbers supplied by the caller):
    {
      "conductors": {"poly": [2,0], "cont": [3,0], "met1": [4,0]},
      "connects":   [["poly","met1","cont"]],   // [a, b, via] ; via null => a-b
      "seed_layer": [30, 0]                     // EXACTLY two marker shapes
    }
  Two shapes on the same conductor layer that touch are always connected; two
  shapes on different layers are connected only when the CONNECT stack allows it
  (a via layer bridges a-via and b-via, never a-b directly).

Invocation (KLayout has no argv for scripts — parameters come from the environment):
    LR_GDS=<in.gds> LR_CONFIG=<cfg.json> LR_OUT=<report.json> \
        [LR_CELL=<top>] [LR_RVE=<markers.lyrdb>] klayout -b -r lvs_recon.py

Report JSON:
    {"verdict": "SHORTED" | "SEPARATE",
     "path": [{"layer":..,"bbox_um":[l,b,r,t]}, ...],   // shortest seed-to-seed
     "culprits": [{"layer":..,"bbox_um":[...]}, ...],   // cut vertices (may be [])
     "redundant_paths": bool,                           // shorted, but no cut vertex
     "direct_abutment": bool, "abutment_bbox_um": [...] // the two rails touch
    }
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
            "lvs_recon: KLayout Python module 'pya' not available "
            "(run inside the KLayout fork via `klayout -b -r`). DISCLOSED, not faked.\n")
        sys.exit(3)


def _li(ly, spec):
    n, d = int(spec[0]), int(spec[1])
    x = ly.find_layer(n, d)
    return x if x is not None else ly.layer(n, d)


def _bbox_um(box, dbu):
    return [round(box.left * dbu, 4), round(box.bottom * dbu, 4),
            round(box.right * dbu, 4), round(box.top * dbu, 4)]


def _adjacent(pya, pa, pb):
    """True when two DRAWN polygons are electrically one piece — overlapping OR
    merely touching along an edge/corner (interacting, not just intersecting)."""
    if not pa.bbox().touches(pb.bbox()):
        return False
    return not pya.Region(pa).interacting(pya.Region(pb)).is_empty()


def _allowed_layer_pairs(cfg):
    """The unordered layer-name pairs the CONNECT stack permits an edge across.
    Same-layer is always allowed; a via bridges a-via and b-via, never a-b."""
    pairs = set()
    for name in cfg["conductors"]:
        pairs.add((name, name))
    for rel in cfg.get("connects", []):
        a, b = rel[0], rel[1]
        via = rel[2] if len(rel) > 2 else None
        if via:
            pairs.add(tuple(sorted((a, via))))
            pairs.add(tuple(sorted((b, via))))
        else:
            pairs.add(tuple(sorted((a, b))))
    return pairs


def _reachable(adj, starts, targets, banned=frozenset()):
    seen = set(starts) - banned
    if not seen:
        return False
    q = collections.deque(seen)
    tset = set(targets)
    if seen & tset:
        return True
    while q:
        u = q.popleft()
        for v in adj[u]:
            if v in banned or v in seen:
                continue
            if v in tset:
                return True
            seen.add(v)
            q.append(v)
    return False


def _shortest_path(adj, starts, targets):
    """BFS shortest node path from any start to any target (inclusive)."""
    prev = {}
    seen = set(starts)
    q = collections.deque(starts)
    tset = set(targets)
    hit = next((s for s in starts if s in tset), None)
    if hit is not None:
        return [hit]
    while q:
        u = q.popleft()
        for v in adj[u]:
            if v in seen:
                continue
            seen.add(v)
            prev[v] = u
            if v in tset:
                path = [v]
                while path[-1] not in starts:
                    path.append(prev[path[-1]])
                path.reverse()
                return path
            q.append(v)
    return None


def _xml_escape(s):
    return (s.replace("&", "&amp;").replace("<", "&lt;")
             .replace(">", "&gt;").replace('"', "&quot;"))


def _rve_category(name):
    """KLayout's rdb reader parses <item><category> as a '.'-separated PATH, so a
    name that is not a bare word must be quoted the way rdb::Category::path() does."""
    bare = name and all(c.isalnum() or c in "_$" for c in name)
    return name if bare else "'" + name.replace("'", "\\'") + "'"


def _write_rve(path, top_name, cat, markers, dbu):
    """Emit the culprits as a KLayout-loadable marker database (same shape as the
    engine's #9 RVE export). An empty culprit list yields an empty, valid DB."""
    with open(path, "w") as o:
        o.write('<?xml version="1.0" encoding="utf-8"?>\n<report-database>\n')
        o.write(" <description>lvs_recon short isolation</description>\n")
        o.write(" <original-file/>\n <generator>lvs_recon</generator>\n")
        o.write(" <top-cell>%s</top-cell>\n" % _xml_escape(top_name))
        o.write(" <tags>\n </tags>\n <categories>\n")
        if markers:
            o.write("  <category>\n   <name>%s</name>\n   <description/>\n"
                    "   <categories>\n   </categories>\n  </category>\n" % _xml_escape(cat))
        o.write(" </categories>\n <cells>\n  <cell>\n")
        o.write("   <name>%s</name>\n   <variant/>\n   <layout-name/>\n"
                % _xml_escape(top_name))
        o.write("   <references>\n   </references>\n  </cell>\n </cells>\n <items>\n")
        for poly in markers:
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

    # --- nodes: one per DRAWN polygon (never merged -- the drawn shape IS the
    # unit a designer would delete to fix the short) ---------------------------
    nodes = []                                   # [(layer_name, polygon)]
    by_layer = collections.defaultdict(list)     # layer_name -> [node index]
    for lname, spec in cfg["conductors"].items():
        it = top.begin_shapes_rec(_li(ly, spec))
        while not it.at_end():
            sh = it.shape()
            if sh.is_polygon() or sh.is_box() or sh.is_path():
                poly = sh.polygon.transformed(it.trans())
                by_layer[lname].append(len(nodes))
                nodes.append((lname, poly))
            it.next()
    if not nodes:
        return {"verdict": "ERROR", "error": "no conductor geometry found"}

    # --- edges: only across CONNECT-permitted layer pairs ----------------------
    adj = collections.defaultdict(set)
    pairs = _allowed_layer_pairs(cfg)
    for la, lb in pairs:
        ia, ib = by_layer.get(la, []), by_layer.get(lb, [])
        for i in ia:
            for j in ib:
                if j <= i and la == lb:
                    continue                      # same layer: each pair once
                if i == j:
                    continue
                if _adjacent(pya, nodes[i][1], nodes[j][1]):
                    adj[i].add(j)
                    adj[j].add(i)

    # --- seeds: exactly two markers, each attached to the shapes it touches ----
    seeds = []
    it = top.begin_shapes_rec(_li(ly, cfg["seed_layer"]))
    while not it.at_end():
        sh = it.shape()
        if sh.is_polygon() or sh.is_box() or sh.is_path():
            seeds.append(sh.polygon.transformed(it.trans()))
        it.next()
    if len(seeds) != 2:
        return {"verdict": "ERROR",
                "error": "seed_layer must hold EXACTLY two markers, got %d" % len(seeds)}
    seeds.sort(key=lambda p: (p.bbox().left, p.bbox().bottom))

    attach = []
    for sp in seeds:
        hit = {i for i, (_, poly) in enumerate(nodes) if _adjacent(pya, sp, poly)}
        if not hit:
            return {"verdict": "ERROR",
                    "error": "seed marker at %s touches no conductor"
                             % _bbox_um(sp.bbox(), dbu)}
        attach.append(hit)

    res = {"verdict": "SEPARATE", "gds": gds,
           "seed_a_bbox_um": _bbox_um(seeds[0].bbox(), dbu),
           "seed_b_bbox_um": _bbox_um(seeds[1].bbox(), dbu),
           "shapes": len(nodes), "path": [], "path_length": 0,
           "culprits": [], "redundant_paths": False, "direct_abutment": False}

    if not _reachable(adj, attach[0], attach[1]):
        if rve_out:
            _write_rve(rve_out, top.name, "LVS_SHORT", [], dbu)
        return res                                # the two nets really are separate

    res["verdict"] = "SHORTED"
    path = _shortest_path(adj, attach[0], attach[1]) or []
    res["path"] = [{"layer": nodes[i][0], "bbox_um": _bbox_um(nodes[i][1].bbox(), dbu)}
                   for i in path]
    res["path_length"] = len(path)

    # --- culprits: shapes whose removal genuinely breaks the connection --------
    probed = attach[0] | attach[1]
    culprits = []
    for i in path:
        if i in probed:
            continue                              # never blame the probed rails
        if not _reachable(adj, attach[0], attach[1], banned={i}):
            culprits.append(i)
    res["culprits"] = [{"layer": nodes[i][0], "bbox_um": _bbox_um(nodes[i][1].bbox(), dbu)}
                       for i in culprits]
    res["culprits"].sort(key=lambda c: (c["bbox_um"][0], c["bbox_um"][1]))

    if len(path) == 2 and path[0] in attach[0] and path[1] in attach[1]:
        # the two probed shapes touch each other -- there is no intervening shape
        # to delete; report where they abut instead.
        res["direct_abutment"] = True
        bb = nodes[path[0]][1].bbox() & nodes[path[1]][1].bbox()
        res["abutment_bbox_um"] = _bbox_um(bb, dbu)
    elif not culprits:
        # shorted through two or more independent routes: no single shape is a cut
        # vertex, and naming one anyway would be a lie.
        res["redundant_paths"] = True

    if rve_out:
        _write_rve(rve_out, top.name, "LVS_SHORT",
                   [nodes[i][1] for i in culprits], dbu)
    return res


def main():
    gds = os.environ.get("LR_GDS")
    cfg_path = os.environ.get("LR_CONFIG")
    out = os.environ.get("LR_OUT")
    cell = os.environ.get("LR_CELL") or None
    rve = os.environ.get("LR_RVE") or None
    if not gds or not cfg_path:
        sys.stderr.write("lvs_recon: set LR_GDS and LR_CONFIG (and LR_OUT).\n")
        return 2
    cfg = json.load(open(cfg_path))
    res = run(gds, cfg, cell, rve)
    text = json.dumps(res, indent=2)
    if out:
        open(out, "w").write(text)
    print(text)
    return 0 if res["verdict"] in ("SHORTED", "SEPARATE") else 3


if __name__ == "__main__":
    sys.exit(main())
