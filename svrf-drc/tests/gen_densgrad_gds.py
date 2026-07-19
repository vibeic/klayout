#  gen_densgrad_gds.py -- synthetic fixtures for the CMP density-GRADIENT check (#49).
#
#  Layers: met1 4/0 (measured), frame 100/0 (a fixed [0,0..20,10] rectangle that
#  pins the layout extent -- and therefore the WINDOW 10 / STEP 10 tiling -- to
#  exactly two tiles T0=[0,0..10,10] and T1=[10,0..20,10] in EVERY mode). The frame
#  is never measured; it only fixes the extent so the fixtures differ ONLY in the
#  met1 fill.
#
#  Rule (densgrad.rule): DG.M1 = DENSITY met1 WINDOW 10 STEP 10 GRADIENT > 0.50
#  measures |density(T0) - density(T1)|. A met1 fill of [0,0..10,H] gives
#  density(T0)=H/10, density(T1)=0, so the gradient is exactly H/10:
#      viol    H=6.000  -> 0.600  > 0.50 -> FAIL 1   (marker = T0+T1 merged [0,0..20,10])
#      edge    H=5.000  -> 0.500  = 0.50 -> PASS 0   (not above the limit)
#      justin  H=5.001  -> 0.5001 > 0.50 -> FAIL 1   (one DBU inside)
#      uniform fill [0,0..20,6]: density(T0)=density(T1)=0.60 -> gradient 0 -> PASS 0,
#              yet the PLAIN DG.M1.NOGRAD density>0.50 FAILs on the same geometry --
#              the distinguisher that GRADIENT is not a relabelled density.
#      empty   no met1 -> gradient 0 -> PASS 0, NITEMS 0.
#
#  Env: DG_MODE viol|edge|justin|uniform|empty (default viol), DG_OUT
import pya, os

ly = pya.Layout(); ly.dbu = 0.001
top = ly.create_cell("TOP")
L = {n: ly.layer(g, 0) for n, g in (("met1", 4), ("frame", 100))}


def box(layer, x0, y0, x1, y1):
    top.shapes(L[layer]).insert(pya.Box(int(round(x0 * 1000)), int(round(y0 * 1000)),
                                        int(round(x1 * 1000)), int(round(y1 * 1000))))


mode = os.environ.get("DG_MODE", "viol")
out = os.environ.get("DG_OUT", "/work/densgrad.gds")

box("frame", 0, 0, 20, 10)                    # fixed extent -> identical tiling everywhere

if mode == "viol":
    box("met1", 0, 0, 10, 6.0)                # density(T0)=0.60, T1=0 -> grad 0.60
elif mode == "edge":
    box("met1", 0, 0, 10, 5.0)                # 0.50 exactly -> PASS
elif mode == "justin":
    box("met1", 0, 0, 10, 5.001)             # 0.5001 -> FAIL (1 DBU inside)
elif mode == "uniform":
    box("met1", 0, 0, 20, 6.0)                # both tiles 0.60 -> grad 0 -> PASS
elif mode == "empty":
    pass                                       # no met1 -> grad 0 -> PASS

ly.write(out)
