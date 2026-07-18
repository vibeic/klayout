#  rve_load.py -- load an svrfdrc-emitted .lyrdb through KLayout's OWN native
#  rdb reader (pya.ReportDatabase) and print a canonical, sorted digest so the
#  gate asserts on the REAL parsed geometry, not on the raw XML text:
#     CAT <name>                       (one per category, sorted)
#     BBOX <category>:<l>,<b>,<r>,<t>   (one per item, um, sorted)
#     NITEMS <n>
#  Env: LYRDB = path to the .lyrdb to load.
import pya, os

rdb = pya.ReportDatabase("")
rdb.load(os.environ["LYRDB"])

cid2name = {c.rdb_id(): c.name() for c in rdb.each_category()}
for name in sorted(cid2name.values()):
    print("CAT", name)

rows = []
for it in rdb.each_item():
    cat = cid2name.get(it.category_id(), "?")
    for v in it.each_value():
        try:
            poly = v.polygon()
        except Exception:
            poly = None
        if poly is None:
            continue
        b = poly.bbox()
        def f(x):
            s = ("%.4f" % x).rstrip("0").rstrip(".")
            return s if s != "-0" else "0"
        rows.append("BBOX %s:%s,%s,%s,%s" % (cat, f(b.left), f(b.bottom), f(b.right), f(b.top)))
for r in sorted(rows):
    print(r)
print("NITEMS", rdb.num_items())
