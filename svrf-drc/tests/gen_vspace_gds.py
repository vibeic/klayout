#  gen_vspace_gds.py -- synthetic fixtures for voltage-aware spacing (#12).
#
#  Layers: met1 4/0, voltage markers vdd18 20/0 (1.8 V) and vdd50 21/0 (5.0 V).
#  Three met1 rectangles, each an independent net (no cont/poly anywhere):
#
#     A = [0, 0    .. 2, 1   ]   marked vdd18 -> 1.8 V
#     B = [bx, 0   .. bx+2, 1]   marked vdd50 -> 5.0 V
#     C = [0, 1.30 .. 2, 2.30]   marked vdd18 -> 1.8 V
#
#  Hand-computed required spacings for  base 0.20 um + 0.05 um/V:
#     A-C : |1.8 - 1.8| = 0.0 V -> 0.20 + 0.05*0.0 = 0.20 um ; measured gap 0.30
#           -> LEGAL in every mode. Same 0.30 um that fails across domains.
#     A-B : |1.8 - 5.0| = 3.2 V -> 0.20 + 0.05*3.2 = 0.36 um ; measured gap bx-2
#     B-C : corner-to-corner sqrt((bx-2)^2 + 0.30^2) >= 0.42 for every bx used
#           -> never a violation.
#
#  Modes sweep the A-B gap ACROSS the 0.36 um boundary, to the DBU:
#     viol   bx = 2.300  -> gap 0.300 <  0.360  -> FAIL 1
#     edge   bx = 2.360  -> gap 0.360 == 0.360  -> PASS 0 (not below the limit)
#     justin bx = 2.359  -> gap 0.359 <  0.360  -> FAIL 1 (1 DBU inside)
#     clean  bx = 2.500  -> gap 0.500 >  0.360  -> PASS 0
#  and one mode that changes ONLY where a voltage marker sits:
#     nomark bx = 2.300, the vdd50 marker moved into empty space at [5,3..5.5,3.5]
#           so it touches NO net. B falls back to the unmarked 0 V default, the
#           requirement drops to 0.20 + 0.05*1.8 = 0.29 um, and the SAME 0.30 um
#           gap is now legal -> PASS 0. Identical met1 geometry, identical layer
#           inventory: only the marker's POSITION moved.
#  In EVERY mode the PER_VOLT 0 control rule must PASS: 0.30 and 0.359 both clear
#  the 0.20 um base, so any failure is attributable to the voltage term alone.
#
#  Env: VSP_MODE viol|edge|justin|clean|nomark (default viol), VSP_OUT
import pya, os

ly = pya.Layout(); ly.dbu = 0.001
top = ly.create_cell("TOP")
L = {n: ly.layer(g, 0) for n, g in
     (("poly", 2), ("cont", 3), ("met1", 4), ("vdd18", 20), ("vdd50", 21))}


def box(layer, x0, y0, x1, y1):
    top.shapes(L[layer]).insert(pya.Box(int(round(x0 * 1000)), int(round(y0 * 1000)),
                                        int(round(x1 * 1000)), int(round(y1 * 1000))))


mode = os.environ.get("VSP_MODE", "viol")
out = os.environ.get("VSP_OUT", "/work/vspace.gds")
bx = {"viol": 2.300, "edge": 2.360, "justin": 2.359,
      "clean": 2.500, "nomark": 2.300}[mode]

box("met1", 0, 0, 2, 1)                       # net A
box("vdd18", 0.5, 0.2, 1.0, 0.7)              # A -> 1.8 V

box("met1", bx, 0, bx + 2, 1)                 # net B
if mode == "nomark":
    box("vdd50", 5.0, 3.0, 5.5, 3.5)          # marker present but touching NO net
else:
    box("vdd50", bx + 0.5, 0.2, bx + 1.0, 0.7)  # B -> 5.0 V

box("met1", 0, 1.30, 2, 2.30)                 # net C
box("vdd18", 0.5, 1.5, 1.0, 2.0)              # C -> 1.8 V

ly.write(out)
print("ok", mode, "bx=%.3f" % bx, out)
