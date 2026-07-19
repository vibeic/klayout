#  gen_mask_gds.py -- synthetic fixtures for multi-patterning colorability (#25).
#
#  Layer: mp 40/0. Rule (mask.rule): MP.DECOMP = MASK mp SPACING 0.30 -- two shapes
#  closer than 0.30 conflict (must go on different masks); the layer is decomposable
#  iff the conflict graph is 2-colourable. Every conflict below is a FACING-EDGE
#  orthogonal gap (unambiguous for the engine's Euclidean space check); every
#  non-edge is > 0.30 apart, so the graph structure is exact.
#
#  tri  (ODD cycle -> undecomposable -> FAIL 3):
#     A=[0,0..2,0.5]  B=[0,0.75..0.5,2]  C=[0.75,0.75..2,1.3]
#       A-B facing gap 0.25 (A top 0.5 vs B bottom 0.75, x-overlap [0,0.5])
#       A-C facing gap 0.25 (A top 0.5 vs C bottom 0.75, x-overlap [0.75,2])
#       B-C facing gap 0.25 (B right 0.5 vs C left 0.75, y-overlap [0.75,1.3])
#     -> triangle, odd cycle, all 3 shapes reported.
#  edgeBC (widen ONLY the B-C gap to exactly 0.30 -> that edge DROPS -> path A-B-A-C):
#     C left moved to 0.80 -> B-C gap 0.30 (== d, NOT < d) -> 2-colourable -> PASS 0.
#  justBC (B-C gap one DBU inside): C left 0.799 -> gap 0.299 < 0.30 -> triangle -> FAIL 3.
#  quad (4-CYCLE ring, even -> 2-colourable -> PASS 0), diagonals 0.354 um > 0.30:
#     P=[0,0..0.5,0.5] Q=[0.75,0..1.25,0.5] R=[0.75,0.75..1.25,1.25] S=[0,0.75..0.5,1.25]
#       ring edges P-Q, Q-R, R-S, S-P all facing gap 0.25; diagonals P-R, Q-S are
#       corner-to-corner 0.354 um apart (> 0.30) -> NOT edges -> even 4-cycle.
#  clean (all far apart, no conflict) -> PASS 0, NITEMS 0.
#
#  Env: MP_MODE tri|edgeBC|justBC|quad|clean (default tri), MP_OUT
import pya, os

ly = pya.Layout(); ly.dbu = 0.001
top = ly.create_cell("TOP")
mp = ly.layer(40, 0)


def box(x0, y0, x1, y1):
    top.shapes(mp).insert(pya.Box(int(round(x0 * 1000)), int(round(y0 * 1000)),
                                  int(round(x1 * 1000)), int(round(y1 * 1000))))


mode = os.environ.get("MP_MODE", "tri")
out = os.environ.get("MP_OUT", "/work/mask.gds")

if mode in ("tri", "edgeBC", "justBC"):
    box(0, 0, 2, 0.5)                          # A
    box(0, 0.75, 0.5, 2)                       # B
    cleft = {"tri": 0.75, "edgeBC": 0.80, "justBC": 0.799}[mode]
    box(cleft, 0.75, 2, 1.3)                   # C (only its LEFT edge moves)
elif mode == "quad":
    box(0, 0, 0.5, 0.5)                        # P
    box(0.75, 0, 1.25, 0.5)                    # Q
    box(0.75, 0.75, 1.25, 1.25)               # R
    box(0, 0.75, 0.5, 1.25)                   # S
elif mode == "clean":
    box(0, 0, 0.5, 0.5)                        # three shapes, all > 0.30 apart
    box(1, 0, 1.5, 0.5)
    box(2, 0, 2.5, 0.5)
elif mode == "single":
    box(0, 0, 0.5, 0.5)                        # one shape -> nothing to decompose -> SKIP

ly.write(out)
