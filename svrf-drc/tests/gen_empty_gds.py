import pya
ly=pya.Layout(); ly.dbu=0.001; top=ly.create_cell("TOP")
top.shapes(ly.layer(20,0)).insert(pya.Box(0,0,1000,1000))
top.shapes(ly.layer(21,0)).insert(pya.Box(500,500,1500,1500))
# layer 99 (e) intentionally EMPTY
ly.write("/work/empty.gds"); print("ok")
