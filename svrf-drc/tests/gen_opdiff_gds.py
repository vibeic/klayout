import pya
ly = pya.Layout(); ly.dbu = 0.001
top = ly.create_cell("TOP")
def L(l,d): return ly.layer(l,d)
def box(li,x0,y0,x1,y1): top.shapes(li).insert(pya.Box(int(x0),int(y0),int(x1),int(y1)))
M1,M2,VIA,NW = L(20,0),L(21,0),L(22,0),L(23,0)
# m1 boxes: some interacting nw, some not; connected to m2 via 'via'
box(M1, 0,0, 1000,1000)         # interacts nw
box(M1, 5000,0, 6000,1000)      # isolated (no nw)
box(M2, 500,500, 1500,1500)     # overlaps m1#1 -> same net through via
box(VIA, 600,600, 700,700)
box(NW, 800,800, 2000,2000)     # overlaps m1#1
ly.write("/work/opdiff.gds"); print("ok")
