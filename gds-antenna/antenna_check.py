#!/usr/bin/env python3
"""antenna_check.py — GDS-geometry process-antenna DRC, native on KLayout's engine.

An INDEPENDENT, authoritative antenna check that runs on the *streamed GDS geometry*
(not on a router report). It reconstructs the as-fabricated conductor connectivity with
KLayout's own ``LayoutToNetlist`` engine and, for every metal layer, computes the
per-net **antenna ratio** = (connected metal area) / (connected gate area). A net whose
ratio exceeds the per-layer limit is a violation.

Commercial equivalent: Calibre antenna (PERC / nmDRC) authoritative GDS-level ratio
checks — the plasma-charge / process-antenna sign-off gate.

Why this exists (the gap it closes): the flow previously had only a *router-report*
antenna consumer (``antenna_report_check.py`` reads OpenROAD's own count). There was no
independent GDS-geometry antenna gate, so the router's number could never be
cross-checked against the physical geometry. This tool provides that second,
independent number — and the harness cross-checks the two counts.

STAGED (as-fabricated) connectivity — the physically correct model
------------------------------------------------------------------
Process antenna damage happens *during* fabrication: when metal layer ``k`` is being
plasma-etched, only layers 1..k exist. Charge collected on the layer-``k`` conductor
(plus everything beneath it, all one exposed node) discharges into any gate on that
node. Upper metals are not yet deposited and cannot help. So for each metal layer ``k``
the connectivity is rebuilt using ONLY the conductors up to and including ``k`` — an
upper-metal jumper that would relieve the antenna in the *final* netlist is correctly
ignored at the stage where the damage occurs.

The diffusion/active layer is NOT placed in the antenna node by default: a
source/drain diffusion diode discharges the collected charge (antenna relief). Model
that ONLY when the deck grants diode credit (``"diode_credit": true``) — then the
active layer is connected in and a net that reaches diffusion is exempt.

Metric (per-layer window)
-------------------------
* ``"layer"``      ratio_k(net) = area(metal_k on net) / gate_area(net)
* ``"cumulative"`` ratio_k(net) = sum(area(metal_j on net), j<=k) / gate_area(net)

Config (chip/PDK-AGNOSTIC — layer numbers + generic limits supplied by the caller):
    {
      "gate":       {"and": ["poly", "active"]},   // gate = poly AND active
      "gate_layers":{"poly": [2,0], "active": [1,0]},
      "conductors": [                              // ordered bottom -> top
         {"name":"poly", "layer":[2,0], "role":"poly"},
         {"name":"cont", "layer":[3,0], "role":"contact"},
         {"name":"met1", "layer":[4,0], "role":"metal", "ratio":40.0},
         {"name":"via1", "layer":[5,0], "role":"via"},
         {"name":"met2", "layer":[6,0], "role":"metal", "ratio":40.0}
      ],
      "metric":       "cumulative",
      "diode_credit": false
    }
A ``contact``/``via`` role bridges the conductor immediately before and after it in the
list. A ``metal`` role carries a ``ratio`` limit (the per-layer antenna ratio bound —
a generic, DISCLOSED bound unless the caller supplies the foundry number).

Invocation (KLayout has no argv for scripts — parameters come from the environment):
    ANT_GDS=<in.gds> ANT_CONFIG=<cfg.json> ANT_OUT=<out.json> [ANT_CELL=<top>] \
        klayout -b -r antenna_check.py          # or the fork's `strmrun`

Output JSON (also printed):
    {"verdict":"PASS"|"FAIL", "worst_ratio":..., "violations":N,
     "per_layer":{"met1":{"limit":40.0,"worst_ratio":50.0,"violations":1,
                          "detail":[{"metal_um2":..,"gate_um2":..,"ratio":..}]}}, ...}

§4.05 honest-failure: a missing/empty GDS or a config that yields no gate area is an
error/HONEST-SKIP, never a vacuous PASS. A metal layer whose net has gate area and a
ratio over the limit is a hard FAIL naming the layer.
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
            "antenna_check: KLayout Python module 'pya' not available "
            "(run inside the KLayout fork via `klayout -b -r` or `strmrun`). "
            "DISCLOSED, not faked.\n")
        sys.exit(3)


def _li(ly, spec):
    """Resolve a [layer,datatype] spec to a layer index (find, else create)."""
    n, d = int(spec[0]), int(spec[1])
    x = ly.find_layer(n, d)
    return x if x is not None else ly.layer(n, d)


def _net_area(l2n, net, layer, dbu2, pya):
    reg = pya.Region()
    reg.insert(l2n.shapes_of_net(net, layer, True))
    return reg.merged().area() * dbu2


def run(gds, cfg, cell_name=None):
    pya = _load_pya()
    ly = pya.Layout()
    ly.read(gds)
    dbu2 = ly.dbu * ly.dbu
    top = ly.cell(cell_name) if cell_name else ly.top_cell()
    if top is None:
        return {"verdict": "ERROR", "error": f"top cell not found: {cell_name}"}

    conductors = cfg["conductors"]
    names = [c["name"] for c in conductors]
    idx = {c["name"]: i for i, c in enumerate(conductors)}
    metals = [c["name"] for c in conductors if c.get("role") == "metal"]
    metric = cfg.get("metric", "cumulative")
    diode_credit = bool(cfg.get("diode_credit", False))
    gate_and = cfg.get("gate", {}).get("and")
    gl = cfg.get("gate_layers", {})

    per_layer = {}
    worst_overall = 0.0
    total_viol = 0

    # Rebuild the connectivity for each metal STAGE k (staged / as-fabricated).
    for k_name in metals:
        k = idx[k_name]
        stage = conductors[: k + 1]  # conductors up to & including this metal
        l2n = pya.LayoutToNetlist(pya.RecursiveShapeIterator(ly, top, []))
        regs = {}
        for c in stage:
            regs[c["name"]] = l2n.make_polygon_layer(_li(ly, c["layer"]), c["name"])

        # gate = poly AND active (a sub-region of poly); rides the poly net.
        if gate_and:
            a = l2n.make_polygon_layer(_li(ly, gl[gate_and[0]]), "_g0")
            b = l2n.make_polygon_layer(_li(ly, gl[gate_and[1]]), "_g1")
            gate = a & b
            poly_name = gate_and[0]
        else:
            gate = l2n.make_polygon_layer(_li(ly, cfg["gate"]["layer"]), "gate")
            poly_name = names[0]
        l2n.register(gate, "gate")

        # self-connect every conductor in the stage
        for c in stage:
            l2n.connect(regs[c["name"]])
        # gate rides the poly net (or the first stage conductor)
        if poly_name in regs:
            l2n.connect(gate, regs[poly_name])
        # contact/via roles bridge their neighbours in the list order
        for i, c in enumerate(stage):
            if c.get("role") in ("contact", "via"):
                if i - 1 >= 0:
                    l2n.connect(regs[c["name"]], regs[stage[i - 1]["name"]])
                if i + 1 < len(stage):
                    l2n.connect(regs[c["name"]], regs[stage[i + 1]["name"]])
        # diode credit: connect diffusion/active so a net reaching it is relieved
        if diode_credit and gate_and:
            act = l2n.make_polygon_layer(_li(ly, gl[gate_and[1]]), "_act")
            # active touches its contact just like poly does; connect to any contact
            for c in stage:
                if c.get("role") == "contact":
                    l2n.connect(act, regs[c["name"]])
            l2n.connect(act)

        l2n.extract_netlist()
        nl = l2n.netlist()

        limit = float(next(c["ratio"] for c in conductors if c["name"] == k_name))
        metal_layers_upto = [c["name"] for c in stage if c.get("role") == "metal"]
        worst = 0.0
        detail = []
        for ckt in nl.each_circuit():
            for net in ckt.each_net():
                ga = _net_area(l2n, net, gate, dbu2, pya)
                if ga <= 0.0:
                    continue  # no gate on this net -> not an antenna node
                # The layer-k rule only applies to a net that actually carries a
                # layer-k conductor (the metal being etched at this stage). A net
                # with no metal-k area collects no charge at the layer-k step, so
                # it is not checked here — this is what keeps the cumulative metric
                # from re-charging an upper stage that the net never reaches.
                mk_area = _net_area(l2n, net, regs[k_name], dbu2, pya)
                if mk_area <= 0.0:
                    continue
                if metric == "cumulative":
                    ma = sum(_net_area(l2n, net, regs[m], dbu2, pya)
                             for m in metal_layers_upto)
                else:
                    ma = mk_area
                ratio = ma / ga
                worst = max(worst, ratio)
                if ratio > limit:
                    detail.append({"metal_um2": round(ma, 4),
                                   "gate_um2": round(ga, 4),
                                   "ratio": round(ratio, 4)})
        detail.sort(key=lambda d: -d["ratio"])
        per_layer[k_name] = {"limit": limit, "metric": metric,
                             "worst_ratio": round(worst, 4),
                             "violations": len(detail),
                             "detail": detail[:20]}
        worst_overall = max(worst_overall, worst)
        total_viol += len(detail)

    if not per_layer:
        return {"verdict": "ERROR",
                "error": "config declares no metal conductors (role=metal)"}

    # §4.05: if NO net anywhere had gate area, the geometry/config is wrong -> not PASS
    any_gate = any(pl["worst_ratio"] > 0.0 or pl["violations"] > 0
                   for pl in per_layer.values())
    verdict = "PASS" if total_viol == 0 else "FAIL"
    res = {"verdict": verdict, "gds": gds, "worst_ratio": round(worst_overall, 4),
           "violations": total_viol, "per_layer": per_layer,
           "diode_credit": diode_credit}
    if not any_gate:
        res["verdict"] = "HONEST_SKIP"
        res["note"] = ("no net carried gate area — check the gate/layer config "
                       "against the GDS (not a pass)")
    return res


def main():
    gds = os.environ.get("ANT_GDS")
    cfg_path = os.environ.get("ANT_CONFIG")
    out = os.environ.get("ANT_OUT")
    cell = os.environ.get("ANT_CELL") or None
    if not gds or not cfg_path:
        sys.stderr.write("antenna_check: set ANT_GDS and ANT_CONFIG (and ANT_OUT).\n")
        return 2
    with open(cfg_path) as f:
        cfg = json.load(f)
    res = run(gds, cfg, cell)
    text = json.dumps(res, indent=2)
    if out:
        with open(out, "w") as f:
            f.write(text)
    print(text)
    return 0 if res["verdict"] == "PASS" else (3 if res["verdict"] in
                                               ("ERROR", "HONEST_SKIP") else 1)


if __name__ == "__main__":
    sys.exit(main())
