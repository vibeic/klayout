#  gen_cmp_gds.py -- synthetic fixtures for the CMP density-gradient check (#49).
#
#  Metal on layer 10/0, analysis window 10x10 um (area 100 um^2). Density in a
#  window = metal_area / 100. Each 10 um tile is filled to an EXACT density by a
#  full-height metal strip of the chosen width:
#     density d  <=>  strip width (10*d) um in that tile.
#
#  Tiles are laid left-to-right starting at x=0; every gradient is hand-computed:
#
#   step   two tiles: left density 0.80 (8 um strip), right 0.20 (2 um strip) ->
#          |0.80 - 0.20| = 0.60. With max_gradient 0.30 -> FAIL, worst delta 0.60.
#   graded three tiles 0.80 / 0.50 / 0.20 -> neighbour deltas 0.30 and 0.30. With
#          max_gradient 0.30 and a STRICT '>' test, 0.30 is NOT a violation ->
#          PASS. Same total 0..0.8 swing, but ramped so no step exceeds the limit.
#   flat   two tiles both 0.50 -> delta 0.0 -> PASS (baseline).
#   edge   two tiles 0.80 / 0.50 -> delta EXACTLY 0.30 -> PASS at the boundary.
#   justin two tiles 0.80 / 0.499 (4.99 um strip) -> delta 0.301 > 0.30 -> FAIL.
#          One part in 1000 of density decides it.
#
#  Env: CG_MODE step|graded|flat|edge|justin (default step), CG_OUT
import pya, os

ly = pya.Layout(); ly.dbu = 0.001
top = ly.create_cell("TOP")
M = ly.layer(10, 0)


def strip(tile_col, width_um):
    #  a full-height (10 um) strip of the given width, left-aligned in the tile,
    #  so the tile's density is exactly width_um / 10.
    x0 = tile_col * 10.0
    top.shapes(M).insert(pya.Box(int(round(x0 * 1000)), 0,
                                 int(round((x0 + width_um) * 1000)), 10000))


mode = os.environ.get("CG_MODE", "step")
out = os.environ.get("CG_OUT", "/work/cmp.gds")

if mode == "step":
    strip(0, 8.0); strip(1, 2.0)              # 0.80, 0.20 -> delta 0.60
elif mode == "graded":
    strip(0, 8.0); strip(1, 5.0); strip(2, 2.0)  # 0.80,0.50,0.20 -> deltas 0.30,0.30
elif mode == "flat":
    strip(0, 5.0); strip(1, 5.0)              # 0.50, 0.50 -> delta 0.0
elif mode == "edge":
    strip(0, 8.0); strip(1, 5.0)              # 0.80, 0.50 -> delta exactly 0.30
elif mode == "justin":
    strip(0, 8.0); strip(1, 4.99)             # 0.80, 0.499 -> delta 0.301

ly.write(out)
print("ok", mode, out)
