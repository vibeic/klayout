#  gen_dfm_gds.py -- synthetic fixture for the DFM-scoring (#47) gate.
#
#  3 slivers (1x20 um, ratio 88.2 -> SR_SLIVER) + 2 tiny squares (0.5x0.5 um,
#  0.25 um^2 -> SR_MINAREA), all well separated so the unweighted HARD_SP spacing
#  rule sees no violation. Hand-computed counts: SR_SLIVER=3, SR_MINAREA=2.
#  Env: DFM_GDS output path (default /work/dfm.gds)
import pya, os
ly = pya.Layout(); ly.dbu = 0.001
top = ly.create_cell("TOP")
poly = ly.layer(10, 0)
def box(x0, y0, x1, y1):
    top.shapes(poly).insert(pya.Box(int(x0), int(y0), int(x1), int(y1)))

out = os.environ.get("DFM_GDS", "/work/dfm.gds")
#  3 slivers 1um x 20um (ratio 88.2)
box(0, 0, 1000, 20000)
box(5000, 0, 6000, 20000)
box(10000, 0, 11000, 20000)
#  2 tiny squares 0.5um x 0.5um (0.25 um^2)
box(0, -5000, 500, -4500)
box(5000, -5000, 5500, -4500)
ly.write(out)
print("ok", out)
