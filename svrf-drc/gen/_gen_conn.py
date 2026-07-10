"""Generate a connectivity test-structure GDS (run inside KLayout).

    klayout -b -r _gen_conn.py -rd out=<conn.gds>

Two metal1/metal2 pairs, each 0.10um apart:
  * A1|B1 are bridged by a via  -> SAME net
  * A2|B2 have no via           -> DIFFERENT nets
So `... < 0.30 NOT CONNECTED` flags only A2|B2, and `CONNECTED` only A1|B1.
"""
import pya

out = globals().get("out", "conn.gds")
ly = pya.Layout(); ly.dbu = 0.001
top = ly.create_cell("TOP")
m1, m2, via = ly.layer(34, 0), ly.layer(36, 0), ly.layer(35, 0)
f = 1000.0


def bx(x0, y0, x1, y1):
    return pya.Box(int(x0 * f), int(y0 * f), int(x1 * f), int(y1 * f))


# same-net pair (bridged by a via)
top.shapes(m1).insert(bx(0.0, 0.0, 1.0, 1.0))       # A1
top.shapes(m2).insert(bx(1.10, 0.0, 2.10, 1.0))     # B1  (0.10 from A1)
top.shapes(via).insert(bx(0.90, 0.40, 1.30, 0.60))  # V1 overlaps A1 and B1
# different-net pair (no via)
top.shapes(m1).insert(bx(0.0, 3.0, 1.0, 4.0))       # A2
top.shapes(m2).insert(bx(1.10, 3.0, 2.10, 4.0))     # B2  (0.10 from A2, unbridged)

ly.write(out)
print(f"wrote {out}: A1|B1 via-bridged (same net), A2|B2 unbridged (different nets)")
