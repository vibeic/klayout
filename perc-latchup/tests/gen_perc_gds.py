#  gen_perc_gds.py -- synthetic fixtures for latch-up guard-ring checks (#26).
#
#  device 12/0 (sensitive active), tap 13/0 (guard-ring / well tap).
#  Rules: max_dist 1.0 um, min_width 0.4 um.
#
#  The device is a 2x2 um square at [0,0..2,2] in every mode. What changes is the
#  tap:
#
#   clean  a full ring around the device: outer [-1,-1..3,3], inner hole
#          [-0.5,-0.5..2.5,2.5] -> ring WIDTH 0.5 um (>= 0.4), inner edge 0.5 um
#          from the device (<= 1.0), and it HAS a hole that contains the device.
#          -> all three checks PASS.
#   bar    tap only on the RIGHT: [2.5,-1..3,3] (width 0.5, gap 0.5). Distance and
#          width PASS, but a bar has NO hole -> the device is NOT enclosed ->
#          ONLY the enclosure check FAILs. Marker = the device [0,0,2,2].
#   far    a full ring pushed outward: inner hole [-1.5,-1.5..3.5,3.5] so the
#          inner edge is 1.5 um from the device (> 1.0) -> DISTANCE FAILs; the
#          device is still enclosed and the ring is still 0.5 um wide.
#   narrow a full ring only 0.3 um wide: outer [-0.8,-0.8..2.8,2.8], inner
#          [-0.5,-0.5..2.5,2.5] -> WIDTH FAILs (0.3 < 0.4); distance (0.5) and
#          enclosure PASS.
#   edge   a full ring whose inner edge is EXACTLY 1.0 um from the device: inner
#          hole [-1,-1..3,3], outer [-1.5,-1.5..3.5,3.5] -> gap == max_dist ->
#          distance PASSES (the boundary is "gap <= max_dist").
#   justin the same ring 1 DBU further out: inner hole [-1.001,..3.001] -> gap
#          1.001 um > 1.0 -> distance FAILs. One DBU decides it.
#
#  Env: PL_MODE clean|bar|far|narrow|edge|justin (default clean), PL_OUT
import pya, os

ly = pya.Layout(); ly.dbu = 0.001
top = ly.create_cell("TOP")
DEV = ly.layer(12, 0)
TAP = ly.layer(13, 0)


def box(layer, x0, y0, x1, y1):
    top.shapes(layer).insert(pya.Box(int(round(x0 * 1000)), int(round(y0 * 1000)),
                                     int(round(x1 * 1000)), int(round(y1 * 1000))))


def ring(outer, inner):
    def bx(t):
        return pya.Box(int(round(t[0] * 1000)), int(round(t[1] * 1000)),
                       int(round(t[2] * 1000)), int(round(t[3] * 1000)))
    r = pya.Region(bx(outer)) - pya.Region(bx(inner))
    for p in r.each():
        top.shapes(TAP).insert(p)


mode = os.environ.get("PL_MODE", "clean")
out = os.environ.get("PL_OUT", "/work/perc.gds")

box(DEV, 0, 0, 2, 2)                                  # the sensitive device

if mode == "clean":
    ring((-1, -1, 3, 3), (-0.5, -0.5, 2.5, 2.5))      # width 0.5, gap 0.5, ring
elif mode == "bar":
    box(TAP, 2.5, -1, 3, 3)                            # one-sided: no hole
elif mode == "far":
    ring((-2, -2, 4, 4), (-1.5, -1.5, 3.5, 3.5))      # gap 1.5 > max_dist
elif mode == "narrow":
    ring((-0.8, -0.8, 2.8, 2.8), (-0.5, -0.5, 2.5, 2.5))  # width 0.3 < min_width
elif mode == "edge":
    ring((-1.5, -1.5, 3.5, 3.5), (-1, -1, 3, 3))      # gap == max_dist 1.0
elif mode == "justin":
    ring((-1.5, -1.5, 3.5, 3.5), (-1.001, -1.001, 3.001, 3.001))  # gap 1.001

ly.write(out)
print("ok", mode, out)
