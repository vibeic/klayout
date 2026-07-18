# gen_antenna_gds.py -- SYNTHETIC fixture for the native in-engine ANTENNA op (#20).
# NO vendor data. Hand-computable one-transistor antenna:
#   gate  = poly (0.5um) over active (2um)          -> gate area = 1.0 um^2
#   met1  = 0.5um-wide wire, length 100um           -> metal area = 50.0 um^2
#   antenna ratio (met1) = 50.0 / 1.0 = 50 > 40     -> ANT.M1 FAIL
# No met2 present, so at the met2 etch stage no net carries met2 -> ANT.M2 PASS.
# Layer numbers match svrf-drc/examples/antenna.rule (synthetic placeholders).
import pya
ly = pya.Layout(); ly.dbu = 0.001
top = ly.create_cell("ANT")
U = 1000  # 1um in dbu
active = ly.layer(1, 0); poly = ly.layer(2, 0); cont = ly.layer(3, 0); met1 = ly.layer(4, 0)
def box(li, x0, y0, x1, y1):
    top.shapes(li).insert(pya.Box(int(x0), int(y0), int(x1), int(y1)))
# active 2x2um; poly 0.5um strip crossing it -> gate = 0.5um*2um = 1.0 um^2
box(active, 0, 0, 2 * U, 2 * U)
box(poly, int(0.75 * U), -1 * U, int(1.25 * U), 3 * U)
box(poly, int(0.75 * U), 3 * U, int(1.25 * U), int(3.5 * U))   # poly up to the contact
box(cont, int(0.85 * U), int(3.1 * U), int(1.15 * U), int(3.4 * U))  # poly->met1 contact
# met1 antenna wire: 0.5um wide, 100um long -> 50.0 um^2
y0 = int(3.0 * U)
box(met1, int(0.85 * U), y0, int(0.85 * U) + 100 * U, y0 + int(0.5 * U))
ly.write("/work/antenna.gds"); print("wrote antenna.gds")
