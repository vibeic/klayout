# gen_corner_gds.py -- SYNTHETIC fixture for the corner-to-corner §4.05 guard.
# NO vendor data.
import pya
ly = pya.Layout(); ly.dbu = 0.001
top = ly.create_cell("TOP")
M = ly.layer(20, 0)
def box(li, x0, y0, x1, y1):
    top.shapes(li).insert(pya.Box(int(x0), int(y0), int(x1), int(y1)))
# DIAGONAL corner-to-corner pair: nearest vertices (1000,1000)-(1150,1150),
# euclid = sqrt(150^2 + 150^2) = 212 dbu = 0.212um < 0.23 min-space -> REAL.
box(M, 0, 0, 1000, 1000)
box(M, 1150, 1150, 2150, 2150)
# FACING control pair: x-gap 150 dbu = 0.15um, full y-overlap (projection>0).
box(M, 5000, 0, 6000, 1000)
box(M, 6150, 0, 7150, 1000)
ly.write("/work/corner.gds"); print("wrote corner.gds")
