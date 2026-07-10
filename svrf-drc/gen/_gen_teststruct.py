"""Generate a test-structure GDS with KNOWN geometry (run inside KLayout).

    klayout -b -r _gen_teststruct.py -rd out=<test.gds>

metal1 (34/0): two 1x1um squares 0.10um apart      -> space violation vs 0.14
metal2 (36/0): one 1x1um square + one 0.2x0.2 sq    -> the small one violates AREA<0.05
poly   (10/0): 1x2um rect  } overlap -> gate = poly AND nact = 1x1um (width 1.0)
nact   (12/0): 2x1um rect  }
All widths >= 1.0um, all corners 90deg.
"""
import pya

out = globals().get("out", "test.gds")

ly = pya.Layout()
ly.dbu = 0.001
top = ly.create_cell("TESTSTRUCT")
m1, m2, poly, nact = (ly.layer(34, 0), ly.layer(36, 0), ly.layer(10, 0), ly.layer(12, 0))


def box_um(x0, y0, x1, y1):
    f = 1.0 / ly.dbu
    return pya.Box(int(x0 * f), int(y0 * f), int(x1 * f), int(y1 * f))


# metal1: 0.10um gap between the two squares
top.shapes(m1).insert(box_um(0.0, 0.0, 1.0, 1.0))
top.shapes(m1).insert(box_um(1.10, 0.0, 2.10, 1.0))
# metal2: one normal + one tiny (area 0.04um^2 < 0.05)
top.shapes(m2).insert(box_um(0.0, 3.0, 1.0, 4.0))
top.shapes(m2).insert(box_um(3.0, 3.0, 3.2, 3.2))
# poly x nact overlap -> gate = 1x1um at [5..6, 5..6]
top.shapes(poly).insert(box_um(5.0, 4.0, 6.0, 6.0))
top.shapes(nact).insert(box_um(4.0, 5.0, 6.0, 6.0))

ly.write(out)
print(f"wrote {out}")
