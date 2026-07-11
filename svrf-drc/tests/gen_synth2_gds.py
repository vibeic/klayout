#!/usr/bin/env python3
"""SYNTHETIC GDS #2 -- forces separation/enclosure/notch/width/boolean to fire.
Disjoint regions per test so they don't interact. NO vendor data."""
import pya

ly = pya.Layout(); ly.dbu = 0.001
top = ly.create_cell("TOP")
def L(l, d): return ly.layer(l, d)
M1, M2, VIA, POLY = L(20, 0), L(21, 0), L(22, 0), L(23, 0)
def box(li, x0, y0, x1, y1): top.shapes(li).insert(pya.Box(int(x0), int(y0), int(x1), int(y1)))

# --- separation (m1 vs m2, gap 200nm < 0.50) ---
box(M1, 0, 0, 1000, 1000)
box(M2, 1200, 0, 2200, 1000)

# --- enclosure (via poorly enclosed by m1: 50nm < 0.20) ---
box(M1, 0, 3000, 2000, 5000)
box(VIA, 50, 3050, 150, 3150)

# --- notch (U-shaped m1: 200nm notch < 0.30) ---
# outer 0..2000 x 7000..9000 with a bite (900..1100)x(8200..9000) from the top
pts = [pya.Point(0, 7000), pya.Point(2000, 7000), pya.Point(2000, 9000),
       pya.Point(1100, 9000), pya.Point(1100, 8200), pya.Point(900, 8200),
       pya.Point(900, 9000), pya.Point(0, 9000)]
top.shapes(M1).insert(pya.Polygon(pts))

# --- width (thin 60nm m1 strip < 0.10) ---
box(M1, 4000, 0, 6000, 60)

# --- boolean band = poly AND m1 (poly overlaps separation-region m1) ---
box(POLY, 400, 0, 1400, 800)

ly.write("/work/synth2.gds")
print("wrote /work/synth2.gds")
