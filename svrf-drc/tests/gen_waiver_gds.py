#  gen_waiver_gds.py -- synthetic fixture for the waiver-management (#10) gate.
#
#  Two undersized squares (both < 0.5 um^2 -> both are min-area violations):
#    A: 0.5x0.5 um at (0,0)   -> 0.25 um^2, marker bbox [0,0,0.5,0.5]
#    B: 0.6x0.6 um at (10,10) -> 0.36 um^2, marker bbox [10,10,10.6,10.6]
#  Env: WV_OUT output path (default /work/waiver.gds)
import pya, os
ly = pya.Layout(); ly.dbu = 0.001
top = ly.create_cell("TOP")
m1 = ly.layer(20, 0)
def box(x0, y0, x1, y1):
    top.shapes(m1).insert(pya.Box(int(x0), int(y0), int(x1), int(y1)))

out = os.environ.get("WV_OUT", "/work/waiver.gds")
box(0, 0, 500, 500)              # A: 0.25 um^2
box(10000, 10000, 10600, 10600)  # B: 0.36 um^2
ly.write(out)
print("ok", out)
