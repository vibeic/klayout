#  gen_cheese_gds.py -- synthetic fixture for the cheesing/slotting (#39) gate.
#
#  A single SOLID 20x20 um metal square on layer 10/0. Its design-extent density
#  is exactly 1.0 (400/400 um^2) -> it FAILS a DENSITY>0.70 max-density rule.
#  Cheesing with hole=4um / wall=2um cuts a 3x3 grid of 4x4 um slots (144 um^2
#  removed) -> density 256/400 = 0.64 (HAND-COMPUTED) -> PASSES, walls all 2um.
#  Env: CHEESE_FIX_OUT (default /work/solid.gds)
import pya, os

ly = pya.Layout(); ly.dbu = 0.001
top = ly.create_cell("TOP")
met = ly.layer(10, 0)
top.shapes(met).insert(pya.Box(0, 0, 20000, 20000))     # 20x20 um solid
out = os.environ.get("CHEESE_FIX_OUT", "/work/solid.gds")
ly.write(out)
print("ok", out)
