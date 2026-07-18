#  gen_pattern_gds.py -- synthetic fixture for geometric pattern matching (#21).
#
#  Pattern = an asymmetric L (bbox 3x2 um, notch at x=1) drawn on pattern layer
#  20/0. Target layer 10/0 holds, at HAND-KNOWN locations:
#     A  L same orientation @ (10,10)   -> bbox [10,10,13,12]   MATCH (translation)
#     B  L same orientation @ (20,10)   -> bbox [20,10,23,12]   MATCH (translation)
#     C  L same orientation @ (10,20)   -> bbox [10,20,13,22]   MATCH (translation)
#     R  L rotated 90 deg   @ (30,10)   -> MATCH only under "rigid" orientations
#     P  a DIFFERENT L (notch at x=2, SAME bbox 3x2) @ (20,20)  -> NEVER matches
#     S  a plain 3x2 rectangle @ (30,20)                        -> NEVER matches
#  So translation -> 3 matches {A,B,C}; rigid -> 4 matches {A,B,C,R}. The P shape
#  shares A's bbox but a different notch, so a bbox-only matcher would wrongly
#  count it -- proving the match is true geometric congruence. NO vendor data.
import pya, os

ly = pya.Layout(); ly.dbu = 0.001
top = ly.create_cell("TOP")
tgt = ly.layer(10, 0)
pat = ly.layer(20, 0)
U = 1000  # 1 um in dbu


def L(notch_x):
    #  L with bbox [0,0,3,2] um; vertical arm width = notch_x
    return pya.Polygon([pya.Point(0, 0), pya.Point(3 * U, 0), pya.Point(3 * U, 1 * U),
                        pya.Point(notch_x * U, 1 * U), pya.Point(notch_x * U, 2 * U),
                        pya.Point(0, 2 * U)])


def rect():
    return pya.Polygon([pya.Point(0, 0), pya.Point(3 * U, 0),
                        pya.Point(3 * U, 2 * U), pya.Point(0, 2 * U)])


def place(layer, poly, dx, dy, trans=None):
    p = poly
    if trans is not None:
        p = p.transformed(trans)
    b = p.bbox()
    p = p.moved(dx * U - b.left, dy * U - b.bottom)      # bbox LL -> (dx,dy) um
    top.shapes(layer).insert(p)


# PM_ALT_OUT: write a SEPARATE pattern GDS holding a T-shape (absent from the
# target) on layer 20/0 -> proves an absent pattern yields 0 matches.
alt = os.environ.get("PM_ALT_OUT")
if alt:
    aly = pya.Layout(); aly.dbu = 0.001
    atop = aly.create_cell("PAT")
    apat = aly.layer(20, 0)
    tshape = pya.Polygon([pya.Point(0, 0), pya.Point(3 * U, 0), pya.Point(3 * U, 1 * U),
                          pya.Point(2 * U, 1 * U), pya.Point(2 * U, 3 * U),
                          pya.Point(1 * U, 3 * U), pya.Point(1 * U, 1 * U), pya.Point(0, 1 * U)])
    atop.shapes(apat).insert(tshape)
    aly.write(alt)
    print("ok-alt", alt)
    raise SystemExit(0)


# reference pattern (notch at x=1)
place(pat, L(1), 0, 40)

# target instances
place(tgt, L(1), 10, 10)                     # A
place(tgt, L(1), 20, 10)                     # B
place(tgt, L(1), 10, 20)                     # C
place(tgt, L(1), 30, 10, pya.Trans(1, False, 0, 0))   # R (rot 90)
place(tgt, L(2), 20, 20)                     # P (different notch, same bbox)
place(tgt, rect(), 30, 20)                   # S (rectangle)

out = os.environ.get("PM_FIX_OUT", "/work/pattern.gds")
ly.write(out)
print("ok", out)
