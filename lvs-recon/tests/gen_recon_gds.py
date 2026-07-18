#  gen_recon_gds.py -- synthetic fixtures for LVS-recon short isolation (#30).
#
#  Layers: poly 2/0, cont 3/0, met1 4/0, seed 30/0.  CONNECT poly met1 BY cont.
#
#  Two met1 rails that are SUPPOSED to be separate nets, each carrying one seed:
#     railA = [0, 0 .. 20, 2]    seedA = [1, 0.5 .. 1.5, 1.0]
#     railB = [0,10 .. 20,12]    seedB = [1,10.5 .. 1.5,11.0]
#  The 8 um gap between them is bridged differently per mode:
#
#   bridge   ONE met1 bridge [8,2 .. 10,10] -> SHORTED, path railA/bridge/railB,
#            exactly ONE culprit at the HAND-KNOWN bbox [8,2,10,10].
#   moved    the same single bridge at [14,2 .. 16,10] -> culprit tracks to
#            [14,2,16,10] (coordinates read from geometry, not constants).
#   twopath  TWO bridges, [8,2..10,10] AND [14,2..16,10] -> still SHORTED, but NO
#            single shape is a cut vertex -> 0 culprits + redundant_paths=true.
#            A tool that just named a shape on the path would wrongly blame one.
#   viapath  NO met1 bridge. The short runs DOWN the stack instead:
#              contA  [8,1.0 .. 9,1.5]   on railA
#              strap  (poly) [8,1.0 .. 9,11.0]
#              contB  [8,10.5 .. 9,11.0] on railB
#            -> SHORTED with a 5-shape path and THREE culprits -- contA, the
#            strap and contB are each a genuine cut vertex, since deleting any
#            one of them breaks the only chain. Proves CONNECT-stack traversal
#            (poly reaches met1 only BY cont), not just same-layer touching.
#   nearmiss the single bridge shortened by 1 DBU so its top edge sits at
#            y = 9.999 instead of 10.0 -> it no longer touches railB ->
#            verdict SEPARATE. Same layers, same shape count as `bridge`.
#   clean    no bridge at all -> SEPARATE, 0 culprits.
#   abut     railB lowered to [0,2 .. 20,4] so the two rails TOUCH directly ->
#            SHORTED with no intervening shape to delete: direct_abutment=true,
#            0 culprits, abutment bbox exactly [0,2,20,2].
#
#  Env: LR_MODE bridge|moved|twopath|viapath|nearmiss|clean|abut (default bridge),
#       LR_GDS_OUT
import pya, os

ly = pya.Layout(); ly.dbu = 0.001
top = ly.create_cell("TOP")
L = {n: ly.layer(g, 0) for n, g in (("poly", 2), ("cont", 3), ("met1", 4), ("seed", 30))}


def box(layer, x0, y0, x1, y1):
    top.shapes(L[layer]).insert(pya.Box(int(round(x0 * 1000)), int(round(y0 * 1000)),
                                        int(round(x1 * 1000)), int(round(y1 * 1000))))


mode = os.environ.get("LR_MODE", "bridge")
out = os.environ.get("LR_GDS_OUT", "/work/recon.gds")

by = 2.0 if mode == "abut" else 10.0          # railB's bottom edge

box("met1", 0, 0, 20, 2)                       # railA
box("met1", 0, by, 20, by + 2)                 # railB
box("seed", 1, 0.5, 1.5, 1.0)                  # seedA on railA
box("seed", 1, by + 0.5, 1.5, by + 1.0)        # seedB on railB

if mode in ("bridge", "twopath"):
    box("met1", 8, 2, 10, 10)                  # the culprit bridge
if mode == "moved":
    box("met1", 14, 2, 16, 10)
if mode == "twopath":
    box("met1", 14, 2, 16, 10)                 # a second, independent bridge
if mode == "nearmiss":
    box("met1", 8, 2, 10, 9.999)               # 1 DBU short of railB -> no touch
if mode == "viapath":
    box("poly", 8, 1.0, 9, 11.0)               # the strap that carries the short
    box("cont", 8, 1.0, 9, 1.5)                # railA -> strap
    box("cont", 8, 10.5, 9, 11.0)              # strap -> railB

ly.write(out)
print("ok", mode, out)
