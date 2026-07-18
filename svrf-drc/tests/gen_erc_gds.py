#  gen_erc_gds.py -- synthetic fixtures for the native ERC op (#13) gate.
#
#  Layers (synthetic placeholders, match erc.rule):
#     active 1/0   poly 2/0   cont 3/0   met1 4/0   via1 5/0   met2 6/0
#  Deck: gate = poly AND active ; tie = met1 AND cont
#        CONNECT poly met1 BY cont ; CONNECT met1 met2 BY via1
#
#  Three independent structures, every coordinate hand-computable:
#
#    NET A (driven)   active_A [0,0..6,2] + poly_A [2,-1..3,6]  -> gate_A [2,0..3,2]
#                     cont_A [2.2,4..2.8,4.6] INSIDE poly_A and INSIDE
#                     met1_A [1.5,3.5..3.5,5] -> tie_A = met1 AND cont != empty.
#                     Net A = {poly, cont, met1} : carries a gate AND a tie -> OK.
#
#    NET B (floating) active_B [10,0..16,2] + poly_B [12,-1..13,6] -> gate_B
#                     [12,0..13,2]. Whether B is tied depends ONLY on whether
#                     cont_B geometrically TOUCHES poly_B:
#                       viol/moved : no cont_B at all          -> FLOATING
#                       fixed      : cont_B INSIDE poly_B      -> tied, PASS
#                       nearmiss   : cont_B 1 DBU to the RIGHT of poly_B's
#                                    x=13 edge -- same layers present, same
#                                    met1_B on top -- still FLOATING.
#
#    NET C (island)   met1_C [30,10..32,12] wired to nothing -> UNCONNECTED.
#                     In `clean` a poly_C + cont_C stub connects it -> PASS.
#
#  Expected engine verdicts:
#     viol     : ERC.FLOATGATE FAIL 1 (marker [12,0,13,2])  ERC.ISLAND FAIL 1
#     moved    : ERC.FLOATGATE FAIL 1 (marker [22,0,23,2])  ERC.ISLAND FAIL 1
#     fixed    : ERC.FLOATGATE PASS 0                       ERC.ISLAND FAIL 1
#     nearmiss : ERC.FLOATGATE FAIL 1                       ERC.ISLAND FAIL 1
#     clean    : ERC.FLOATGATE PASS 0                       ERC.ISLAND PASS 0
#  ERC.NOTIE (one-operand FLOATING) is an honest SKIP in every mode.
#
#  Env: ERC_MODE viol|moved|fixed|nearmiss|clean (default viol), ERC_OUT
import pya, os

ly = pya.Layout(); ly.dbu = 0.001
top = ly.create_cell("TOP")
L = {n: ly.layer(g, 0) for n, g in
     (("active", 1), ("poly", 2), ("cont", 3), ("met1", 4), ("via1", 5), ("met2", 6))}


def box(layer, x0, y0, x1, y1):
    top.shapes(L[layer]).insert(pya.Box(int(round(x0 * 1000)), int(round(y0 * 1000)),
                                        int(round(x1 * 1000)), int(round(y1 * 1000))))


mode = os.environ.get("ERC_MODE", "viol")
out = os.environ.get("ERC_OUT", "/work/erc.gds")

# ---- NET A: a properly tied gate (present and identical in every mode) ------
box("active", 0, 0, 6, 2)
box("poly", 2, -1, 3, 6)                 # gate_A = [2,0..3,2]
box("met1", 1.5, 3.5, 3.5, 5.0)
box("cont", 2.2, 4.0, 2.8, 4.6)          # inside poly_A AND met1_A -> tie

# ---- NET B: the gate under test --------------------------------------------
bx = 10.0 if mode != "moved" else 20.0   # `moved` shifts structure B by +10 um
box("active", bx, 0, bx + 6, 2)
box("poly", bx + 2, -1, bx + 3, 6)       # gate_B = [bx+2,0 .. bx+3,2]

if mode in ("fixed", "clean"):
    box("met1", bx + 1.5, 3.5, bx + 4.0, 5.0)
    box("cont", bx + 2.2, 4.0, bx + 2.8, 4.6)      # INSIDE poly_B -> tied
elif mode == "nearmiss":
    box("met1", bx + 1.5, 3.5, bx + 4.0, 5.0)
    #  1 DBU (0.001 um) clear of poly_B's right edge at x = bx+3 -> NOT connected
    box("cont", bx + 3.001, 4.0, bx + 3.601, 4.6)

# ---- NET C: the isolated met1 island ---------------------------------------
box("met1", 30, 10, 32, 12)
if mode == "clean":
    box("poly", 30.5, 9, 31.5, 13)                 # no active here -> not a gate
    box("cont", 30.7, 10.5, 31.3, 11.1)            # ties the island to poly

ly.write(out)
print("ok", mode, out)
