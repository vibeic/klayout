#!/usr/bin/env python3
"""Generate a SYNTHETIC GDS exercising the layers coverage.rule references.
Runs inside the container (uses pya). NO vendor data -- arbitrary boxes.

GDS layers used by coverage.rule:
  34/0, 34/5 -> metal1 (idx 100)     36/0 -> metal2 (idx 101) / metal2b
  10/0 poly   12/0 nact   13/0 pact   35/0 via
"""
import pya

ly = pya.Layout()
ly.dbu = 0.001
top = ly.create_cell("TOP")

def L(l, d):
    return ly.layer(l, d)

def box(layer_idx, x0, y0, x1, y1):
    top.shapes(layer_idx).insert(pya.Box(int(x0), int(y0), int(x1), int(y1)))

# metal1 (34/0): two boxes 100nm apart horizontally (space violation vs 0.14),
# plus a thin 50nm strip (width violation vs 0.10), plus a tiny square (area).
box(L(34, 0), 0, 0, 1000, 1000)        # 1um x 1um
box(L(34, 0), 1100, 0, 2100, 1000)     # gap 100nm -> external spacing viol
box(L(34, 0), 0, 2000, 2000, 2050)     # 50nm tall strip -> internal width viol
box(L(34, 0), 5000, 5000, 5030, 5030)  # 30nm sq -> min-area viol
# a datatype-5 metal1 shape (tests the 34/5 -> idx100 union)
box(L(34, 5), 3000, 3000, 3400, 3400)

# metal2 (36/0): overlaps metal1 region for AND/XOR/OR; one box near metal1 for
# separation, and a via bridging them.
box(L(36, 0), 500, 500, 1500, 1500)
box(L(36, 0), 4000, 0, 5000, 1000)

# via (35/0) between metal1 and metal2 (CONNECT metal1 metal2 BY via)
box(L(35, 0), 600, 600, 700, 700)

# poly (10/0) and nact (12/0) overlap -> gate = poly AND nact
box(L(10, 0), 0, 0, 800, 800)
box(L(12, 0), 400, 0, 1200, 800)
# pact (13/0)
box(L(13, 0), 0, 4000, 500, 4500)

ly.write("/work/synth.gds")
print("wrote /work/synth.gds  dbu=", ly.dbu, " cells=", ly.cells())
