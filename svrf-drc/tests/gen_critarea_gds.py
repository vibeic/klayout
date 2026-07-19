#  gen_critarea_gds.py -- synthetic fixtures for shorts critical-area analysis (#46).
#
#  Layer: met1 4/0. Two parallel rails, length L=10 (y in [2,12]), width 2, separated
#  by a horizontal gap s. Rule (critarea.rule) probes defect radius rho=1.5:
#      CA = (2*rho - s) * (L + 2*rho) = (3 - s) * 13     [um^2]
#  because KLayout's default (mode-2) sizing grows each orthogonal rail to an exact
#  rectangle, so the shorts locus (railA (+)rho) INTERSECT (railB (+)rho) is the exact
#  rectangle of width (2*rho - s) and height (L + 2*rho)=13.
#
#      base   s=2.000  railB=[4,2..6,12]      -> CA = 1.000*13 = 13.000
#      justin s=1.999  railB=[3.999,2..5.999] -> CA = 1.001*13 = 13.013  (1 DBU closer)
#      near   s=1.000  railB=[3,2..5,12]      -> CA = 2.000*13 = 26.000
#      far    s=4.000  railB=[6,2..8,12]      -> 2*rho=3 < s -> no overlap -> CA = 0.000
#
#  On the > 13.0 rule (CA.M1.PASS): base 13.0 PASSes at exactly the limit, justin
#  13.013 FAILs one DBU in, near 26.0 FAILs, far 0.0 PASSes. The marker for a FAIL is
#  the shorts locus itself; for base it is [2.5,0.5 .. 3.5,13.5] (grown railA right
#  edge 3.5 meets grown railB left edge 2.5; y = [2-1.5, 12+1.5]).
#
#  Env: CA_MODE base|justin|near|far (default base), CA_OUT
import pya, os

ly = pya.Layout(); ly.dbu = 0.001
top = ly.create_cell("TOP")
met1 = ly.layer(4, 0)


def box(x0, y0, x1, y1):
    top.shapes(met1).insert(pya.Box(int(round(x0 * 1000)), int(round(y0 * 1000)),
                                    int(round(x1 * 1000)), int(round(y1 * 1000))))


mode = os.environ.get("CA_MODE", "base")
out = os.environ.get("CA_OUT", "/work/critarea.gds")

box(0, 2, 2, 12)                               # rail A (fixed), right edge x=2
bx = {"base": 4.0, "justin": 3.999, "near": 3.0, "far": 6.0}[mode]
box(bx, 2, bx + 2, 12)                         # rail B, left edge x=bx (gap = bx-2)

ly.write(out)
