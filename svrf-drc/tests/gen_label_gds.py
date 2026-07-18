#  gen_label_gds.py -- synthetic fixtures for text/label handling (#24).
#
#  Layers: met1 4/0, netname 40/0 (TEXT shapes).  LABEL netname met1.
#
#  Three met1 rails, each carrying a designer's net name as a TEXT shape:
#     railX = [ 0,0 .. 10,2]   text "VDD" at (5, 1)
#     railY = [20,0 .. 30,2]   text "VSS" at (25, 1)
#     railZ = [ 0,10 .. 10,12] text "CLK" at (5, 11)
#  railZ never participates: it carries exactly one name in every mode and must
#  never be flagged, so a rule that just counted labels would fail the test.
#
#  Modes differ only in what sits in the 10 um gap between railX and railY:
#     short    a met1 bridge [10,0 .. 20,2] joins them -> ONE net carries both
#              "VDD" and "VSS" -> FAIL 1. The marker is the whole shorted net,
#              railX+bridge+railY, bbox exactly [0,0,30,2].
#     nearmiss the same bridge 1 DBU short of railY ([10,0 .. 19.999,2]) -> the
#              nets stay separate -> PASS 0. Identical layer and label inventory.
#     clean    no bridge -> PASS 0.
#     dupname  no bridge, but a SECOND "VDD" text at (2,1) also on railX. One net,
#              two labels, ONE distinct name -> NOT a clash -> PASS 0. This is
#              what separates "two labels" from "two NAMES".
#
#  Env: LBL_MODE short|nearmiss|clean|dupname (default short), LBL_OUT
import pya, os

ly = pya.Layout(); ly.dbu = 0.001
top = ly.create_cell("TOP")
L = {n: ly.layer(g, 0) for n, g in
     (("poly", 2), ("cont", 3), ("met1", 4), ("netname", 40))}


def box(layer, x0, y0, x1, y1):
    top.shapes(L[layer]).insert(pya.Box(int(round(x0 * 1000)), int(round(y0 * 1000)),
                                        int(round(x1 * 1000)), int(round(y1 * 1000))))


def text(s, x, y):
    top.shapes(L["netname"]).insert(
        pya.Text(s, pya.Trans(pya.Vector(int(round(x * 1000)), int(round(y * 1000))))))


mode = os.environ.get("LBL_MODE", "short")
out = os.environ.get("LBL_OUT", "/work/label.gds")

box("met1", 0, 0, 10, 2);    text("VDD", 5, 1)      # railX
box("met1", 20, 0, 30, 2);   text("VSS", 25, 1)     # railY
box("met1", 0, 10, 10, 12);  text("CLK", 5, 11)     # railZ -- never involved

if mode == "short":
    box("met1", 10, 0, 20, 2)                       # bridges railX to railY
elif mode == "nearmiss":
    box("met1", 10, 0, 19.999, 2)                   # 1 DBU clear of railY
elif mode == "dupname":
    text("VDD", 2, 1)                               # a SECOND label, SAME name

ly.write(out)
print("ok", mode, out)
