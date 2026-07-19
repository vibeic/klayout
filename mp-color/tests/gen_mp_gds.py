#  gen_mp_gds.py -- synthetic fixtures for multi-patterning coloring (#25).
#
#  Layer 10/0, single-mask min spacing 0.20 um (= 200 DBU). A pair of shapes
#  CONFLICTS (needs different masks) iff their gap is BELOW 200 DBU; a gap of
#  exactly 200 DBU is clean (KLayout separation_check(d): gap < d flags).
#
#  All shapes are 1x1 um squares; only the GAPS matter, and every gap is chosen
#  to be hand-checkable:
#
#   triangle  three squares mutually within 0.10 um -> conflict graph is a
#             TRIANGLE (3-cycle) -> NOT 2-colorable -> UNCOLORABLE, odd cycle
#             length 3. The three squares:
#               A [0,0..1,1]  B [1.1,0..2.1,1]  C [0.55,1.1..1.55,2.1]
#             A-B gap 0.10, A-C and B-C gaps < 0.20 (diagonal, checked by engine).
#   chain     the SAME three squares but C pushed far up ([0.55,5..1.55,6]) so
#             only A-B conflict remains -> a single edge -> 2-colorable -> A and
#             B get different colors, C is unconstrained.
#   square4   four squares in a ring, each conflicting only with its two ring
#             neighbours -> a 4-CYCLE (even) -> 2-colorable (checkerboard).
#   five      five squares in a ring -> a 5-CYCLE (odd) -> UNCOLORABLE with 2
#             masks, odd cycle length 5. With n_colors=3 the same graph IS
#             colorable (a 5-cycle is 3-colorable) -> the tool must witness it.
#   edge      two squares at EXACTLY 0.20 um gap -> NOT below the minimum -> no
#             conflict edge -> trivially colorable. The DBU boundary.
#   justin    the same two squares 1 DBU closer (0.199 um) -> conflict edge ->
#             still 2-colorable (one edge) but bipartite=... it is just one edge,
#             so the point of this mode is the EDGE COUNT flips 0 -> 1 at the DBU.
#
#  Env: MP_MODE triangle|chain|square4|five|edge|justin (default triangle), MP_OUT
import pya, os

ly = pya.Layout(); ly.dbu = 0.001
top = ly.create_cell("TOP")
lyr = ly.layer(10, 0)


def sq(x, y, w=1.0, h=1.0):
    top.shapes(lyr).insert(pya.Box(int(round(x * 1000)), int(round(y * 1000)),
                                   int(round((x + w) * 1000)), int(round((y + h) * 1000))))


mode = os.environ.get("MP_MODE", "triangle")
out = os.environ.get("MP_OUT", "/work/mp.gds")

if mode in ("triangle", "chain"):
    sq(0, 0)                       # A
    sq(1.1, 0)                     # B  (A-B gap 0.10)
    sq(0.55, 1.1 if mode == "triangle" else 5.0)   # C
elif mode == "square4":
    #  ring with ORTHOGONAL gaps 0.15 um (< 0.20 -> neighbours conflict) but
    #  DIAGONAL gaps sqrt(0.15^2+0.15^2) = 0.212 um (> 0.20 -> corners are clean),
    #  so A-C and B-D do NOT conflict and the graph is EXACTLY a 4-cycle A-B-C-D-A
    #  (even) -> 2-colorable checkerboard.
    sq(0, 0)          # A
    sq(1.15, 0)       # B (right of A, gap 0.15)
    sq(1.15, 1.15)    # C (above B, gap 0.15)
    sq(0, 1.15)       # D (above A / left of C, gap 0.15)
elif mode == "five":
    #  five squares in a ring; consecutive pairs conflict (gaps 0.10 orthogonal
    #  or 0.141 diagonal, both < 0.20), non-consecutive pairs >= 0.65 apart ->
    #  the conflict graph is EXACTLY a 5-cycle S0-S1-S2-S3-S4-S0 (hand-verified).
    sq(0.00, 0.00)     # S0
    sq(1.10, 0.00)     # S1  (S0-S1 x-gap 0.10)
    sq(1.65, 1.10)     # S2  (S1-S2 y-gap 0.10)
    sq(0.55, 2.20)     # S3  (S2-S3 diag 0.141)
    sq(-0.55, 1.10)    # S4  (S3-S4 diag 0.141 ; S4-S0 y-gap 0.10)
elif mode in ("edge", "justin"):
    gap = 0.200 if mode == "edge" else 0.199
    sq(0, 0)
    sq(1.0 + gap, 0)

ly.write(out)
print("ok", mode, out)
