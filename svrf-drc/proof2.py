"""
proof2.py — FAIL<->PASS discrimination proof for the edge pipeline, windowed
DENSITY, and native NET AREA RATIO added in the edges/density/net enhancement.

Empty-layout dispatch coverage (100%) only proves each rule RUNS; this proof
builds targeted geometry and asserts each new check DISCRIMINATES: it FAILs on a
real violation and PASSes when clean. White-box checks additionally verify the
edge derivations build the geometrically-correct pya.Edges sets.

    klayout -b -r proof2.py -rd root=<svrf-drc dir>
"""
import os
import sys
import traceback

root = globals().get("root") or os.getcwd()
sys.path.insert(0, root)
import pya  # noqa: E402
from svrf_klayout.run_svrf_drc import Engine  # noqa: E402

BUILD = os.path.join(root, "build")
os.makedirs(BUILD, exist_ok=True)
FAILS = []


def _layout(layers):
    ly = pya.Layout(); ly.dbu = 0.001
    top = ly.create_cell("TOP")
    for (num, boxes) in layers:
        li = ly.layer(num, 0)
        for (x0, y0, x1, y1) in boxes:
            top.shapes(li).insert(pya.Box(x0, y0, x1, y1))
    return ly


def _run(deck, ly):
    gds = os.path.join(BUILD, "p2.gds"); ly.write(gds)
    eng = Engine(gds, deck)
    verd = {r.name: (v, info) for v, r, info in eng.execute()}
    return eng, verd


def expect(name, verd, want):
    got = verd.get(name, ("MISSING", None))
    if got[0] != want:
        FAILS.append(f"  {name}: expected {want}, got {got}")


def main():
    # 1. Edge-pipeline EXTERNAL (spacing on an EDGE-derived layer): two metal bars
    #    0.10um apart; edges of metal -> edge-spacing check.
    eng, v = _run("""
LAYER metal 10 0
me = metal EDGE
S.FAIL {
  EXTERNAL me < 0.15
}
S.PASS {
  EXTERNAL me < 0.05
}
""", _layout([(10, [(0, 0, 1000, 1000), (1100, 0, 2100, 1000)])]))
    expect("S.FAIL", v, "FAIL")     # 0.10um gap < 0.15 -> violation
    expect("S.PASS", v, "PASS")     # 0.10um gap not < 0.05 -> clean
    if "me" not in eng.edge_layers:
        FAILS.append("  'me' should be an edge layer")

    # 2. INSIDE/OUTSIDE EDGE build correctness (white-box): 2um bar; B covers middle.
    eng, v = _run("""
LAYER a 11 0
LAYER b 12 0
ain  = a INSIDE EDGE b
aout = a OUTSIDE EDGE b
""", _layout([(11, [(0, 0, 2000, 500)]), (12, [(500, -100, 1500, 600)])]))
    ein = eng.edges_ns.get("ain"); eout = eng.edges_ns.get("aout")
    full = eng.regions["a"].edges().length()
    if not ein or ein.length() <= 0:
        FAILS.append("  inside_part must be non-empty")
    if not eout or eout.length() <= 0:
        FAILS.append("  outside_part must be non-empty")
    if ein and eout and abs((ein.length() + eout.length()) - full) > 4:
        FAILS.append(f"  inside+outside must reconstruct all edges "
                     f"({ein.length()}+{eout.length()} vs {full})")

    # 3. LENGTH edge selection (white-box): 2um x 0.3um bar -> two 2um + two 0.3um edges.
    eng, v = _run("""
LAYER m 13 0
e     = m EDGE
longs = e LENGTH > 1.0
""", _layout([(13, [(0, 0, 2000, 300)])]))
    longs = eng.edges_ns.get("longs")
    if not longs or longs.count() != 2:
        FAILS.append(f"  LENGTH>1.0 should keep two 2um edges, got "
                     f"{None if longs is None else longs.count()}")

    # 4. Windowed DENSITY (min & max): metal covers exactly 50% of a 2um window.
    eng, v = _run("""
LAYER fill 14 0
D.MAX.FAIL {
  DENSITY fill > 0.4 WINDOW 2.0 STEP 2.0
}
D.MAX.PASS {
  DENSITY fill > 0.6 WINDOW 2.0 STEP 2.0
}
D.MIN.FAIL {
  DENSITY fill < 0.6 WINDOW 2.0 STEP 2.0
}
D.MIN.PASS {
  DENSITY fill < 0.4 WINDOW 2.0 STEP 2.0
}
""", _layout([(14, [(0, 0, 1000, 2000)])]))
    expect("D.MAX.FAIL", v, "FAIL")   # density 0.5 > 0.4
    expect("D.MAX.PASS", v, "PASS")   # 0.5 not > 0.6
    expect("D.MIN.FAIL", v, "FAIL")   # 0.5 < 0.6
    expect("D.MIN.PASS", v, "PASS")   # 0.5 not < 0.4

    # 5. Native NET AREA RATIO: net-1 A overlaps B (ratio 1.0); net-2 B alone (ratio 0).
    eng, v = _run("""
LAYER a 15 0
LAYER b 16 0
CONNECT a b
nc = a NET AREA RATIO b == 0
NC.FAIL {
  COPY nc
}
big = a NET AREA RATIO b > 100
NC.PASS {
  COPY big
}
""", _layout([
        (15, [(0, 0, 1000, 1000)]),
        (16, [(500, 0, 1500, 1000), (5000, 0, 6000, 1000)]),
    ]))
    expect("NC.FAIL", v, "FAIL")     # B-only net has ratio 0 -> flagged
    expect("NC.PASS", v, "PASS")     # no net has ratio > 100

    if FAILS:
        print("SVRF-DRC PROOF2: FAIL\n" + "\n".join(FAILS))
        raise SystemExit(1)
    print("SVRF-DRC PROOF2 (edge / density / net-ratio discrimination): PASS")


try:
    main()
except SystemExit:
    raise
except BaseException:
    print("SVRF-DRC PROOF2: EXCEPTION\n" + traceback.format_exc())
    raise SystemExit(1)
