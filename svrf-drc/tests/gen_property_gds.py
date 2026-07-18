#  gen_property_gds.py -- synthetic fixtures for the eqDRC (#8) PROPERTY gate.
#
#  Builds a 5x5 um square (isoperimetric ratio 16, PASS) and, in "viol" mode, a
#  1x20 um sliver (ratio 88.2, FAIL). Both properties are hand-computable exactly.
#  Env:
#    PROP_MODE  "viol" (square+sliver, default) | "clean" (square only)
#    PROP_OUT   output path (default /work/property.gds)
import pya, os
ly = pya.Layout(); ly.dbu = 0.001
top = ly.create_cell("TOP")
poly = ly.layer(10, 0)
def box(x0, y0, x1, y1):
    top.shapes(poly).insert(pya.Box(int(x0), int(y0), int(x1), int(y1)))

mode = os.environ.get("PROP_MODE", "viol")
out  = os.environ.get("PROP_OUT", "/work/property.gds")

#  square 5um x 5um at origin : area 25 um^2, perimeter 20 um  -> P^2/A = 16.0
box(0, 0, 5000, 5000)
if mode == "viol":
    #  sliver 1um x 20um at x=10um : area 20 um^2, perimeter 42 um -> P^2/A = 88.2
    box(10000, 0, 11000, 20000)

ly.write(out)
print("ok", mode, out)
