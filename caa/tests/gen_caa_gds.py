#  gen_caa_gds.py -- synthetic fixtures for Critical Area Analysis (#46).
#
#  Two parallel horizontal rails on layers 10/0 (A) and 11/0 (B), width 1 um,
#  length 20 um (x from -5 to 15), so they extend well beyond the interior
#  measurement window x in [0,10] on both ends. The vertical facing gap is s:
#     railA = [-5, 0    .. 15, 1    ]        (top edge y = 1)
#     railB = [-5, 1+s  .. 15, 2+s  ]        (bottom edge y = 1+s)
#
#  For a defect of diameter x (radius r = x/2), the short critical area within the
#  window is the facing overlap band, height (2r - s), length W = 10 um:
#     Ac_short(x) = W * (x - s)   for x > s,   else 0.        (HAND-COMPUTED, exact)
#
#  With s = 0.5 um and W = 10 um:
#     x = 0.5 (= s)  -> Ac = 0
#     x = 1.0        -> Ac = 10 * 0.5 = 5.0 um^2
#     x = 2.0        -> Ac = 10 * 1.5 = 15.0 um^2
#  Weighted critical area for D(x) = D0 * 2 x0^2 / x^3 (x0 <= s):
#     AWC_short = W * D0 * x0^2 / s.   With W=10, D0=1, x0=0.1: 10*1*0.01/0.5 = 0.2.
#
#  Modes:
#     s05  s = 0.5 um  (the reference above)
#     s10  s = 1.0 um  -> Ac_short(1.0) = 0 (2r = s, no overlap); Ac(2.0)=10*1.0=10;
#          AWC = 10*1*0.01/1.0 = 0.1  (doubling s halves AWC -- geometry-derived)
#     wide W doubled: window x in [0,20], rails x from -5..25, s=0.5 ->
#          Ac_short(1.0) = 20*0.5 = 10.0 ; AWC = 20*1*0.01/0.5 = 0.4 (W scales it)
#     clean the two rails 5 um apart (s = 5.0) -> for x_max=4 um every defect has
#          2r < s -> Ac = 0 at every diameter -> AWC = 0.
#
#  Env: CAA_MODE s05|s10|wide|clean (default s05), CAA_OUT
import pya, os

ly = pya.Layout(); ly.dbu = 0.001
top = ly.create_cell("TOP")
A = ly.layer(10, 0)
B = ly.layer(11, 0)


def box(layer, x0, y0, x1, y1):
    top.shapes(layer).insert(pya.Box(int(round(x0 * 1000)), int(round(y0 * 1000)),
                                     int(round(x1 * 1000)), int(round(y1 * 1000))))


mode = os.environ.get("CAA_MODE", "s05")
out = os.environ.get("CAA_OUT", "/work/caa.gds")

s = {"s05": 0.5, "s10": 1.0, "wide": 0.5, "clean": 5.0}[mode]
xlo, xhi = (-5, 25) if mode == "wide" else (-5, 15)

box(A, xlo, 0, xhi, 1)                 # railA, top edge y=1
box(B, xlo, 1 + s, xhi, 2 + s)         # railB, bottom edge y=1+s

ly.write(out)
print("ok", mode, "s=%.1f" % s, out)
