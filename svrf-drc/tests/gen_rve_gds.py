#  gen_rve_gds.py -- synthetic fixtures for the RVE result-DB (#9) gate.
#
#  poly layer 10/0. Two 0.5x0.5 um squares (area 0.25 um^2) FAIL AREA<0.3 and
#  become the two error markers; one 5x5 um square PASSES (area 25) and must NOT
#  appear in the RVE DB. Every marker bbox is hand-computable exactly:
#     viol : sqA=[2,2,2.5,2.5]  sqB=[8,8,8.5,8.5]              (2 markers)
#     moved: sqA=[2,2,2.5,2.5]  sqB=[12,12,12.5,12.5]          (2 markers, B moved)
#     clean: (no small squares)                                (0 markers)
#  The big passing square lives at [20,20,25,25] in every mode.
#  Env: RVE_MODE viol|moved|clean (default viol), RVE_OUT (default /work/rve.gds)
import pya, os

ly = pya.Layout(); ly.dbu = 0.001
top = ly.create_cell("TOP")
poly = ly.layer(10, 0)

def box_um(x0, y0, x1, y1):
    top.shapes(poly).insert(pya.Box(int(x0 * 1000), int(y0 * 1000),
                                    int(x1 * 1000), int(y1 * 1000)))

mode = os.environ.get("RVE_MODE", "viol")
out  = os.environ.get("RVE_OUT", "/work/rve.gds")

#  passing big square (area 25 um^2 -> not a min-area marker)
box_um(20, 20, 25, 25)

if mode in ("viol", "moved"):
    box_um(2, 2, 2.5, 2.5)                 # square A -> marker [2,2,2.5,2.5]
    if mode == "viol":
        box_um(8, 8, 8.5, 8.5)             # square B -> marker [8,8,8.5,8.5]
    else:
        box_um(12, 12, 12.5, 12.5)         # square B moved -> [12,12,12.5,12.5]

ly.write(out)
print("ok", mode, out)
