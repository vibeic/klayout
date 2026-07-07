#!/usr/bin/env python3
# Proof harness for the vibeic KLayout streamout fixes.
#   python3 proof.py <def-file> <out-gds> [grid_nm]
# Reads the DEF (with mytech.lef), writes GDS, then reports, from the FLATTENED
# top cell:
#   - off-grid vertex count vs the manufacturing grid (grid_nm, default 5)
#   - per (layer/datatype) polygon count (after a boolean merge, to expose
#     un-merged abutting shapes as >1 polygon)
#   - the set of GDS layer/datatype numbers produced
import sys
import klayout.db as db

def_file = sys.argv[1]
out_gds  = sys.argv[2]
grid_nm  = int(sys.argv[3]) if len(sys.argv) > 3 else 5

opts = db.LoadLayoutOptions()
cfg = opts.lefdef_config
cfg.lef_files = ["/work/mytech.lef"]
cfg.read_lef_with_def = False
cfg.dbu = 0.001
cfg.create_other_layers = True
opts.lefdef_config = cfg

ly = db.Layout()
ly.read(def_file, opts)
ly.write(out_gds)

dbu_nm = round(ly.dbu * 1000)  # dbu in nm (should be 1)
top = ly.top_cell()

# ---- collect layer numbers produced ----
layers = []
for li in ly.layer_indexes():
    info = ly.get_info(li)
    layers.append((info.layer, info.datatype, info.name))
layers.sort()

# ---- off-grid vertex count (flattened) + per-layer merged polygon count ----
g = grid_nm  # grid in dbu (dbu is 1nm here)
total_offgrid = 0
percell_polys = {}
merged_polys = {}
for li in ly.layer_indexes():
    info = ly.get_info(li)
    key = "%d/%d" % (info.layer, info.datatype)
    # recursive (flattened) shapes for this layer under the top cell.
    # merged_semantics=False => report polygons exactly as written (abutting
    # shapes stay separate), which is what exposes the un-merged-boundary defect.
    reg = db.Region(top.begin_shapes_rec(li))
    reg.merged_semantics = False
    # count off-grid vertices (on the raw, as-written polygons)
    off = 0
    for poly in reg.each():
        for pt in poly.each_point_hull():
            if (pt.x % g) != 0 or (pt.y % g) != 0:
                off += 1
        for h in range(poly.holes()):
            for pt in poly.each_point_hole(h):
                if (pt.x % g) != 0 or (pt.y % g) != 0:
                    off += 1
    total_offgrid += off
    # polygon count as-written (abutting => separate polys)
    percell_polys[key] = reg.count()
    # polygon count AFTER a real geometric merge (true shape count)
    rm = reg.dup(); rm.merged_semantics = True; rm.merge()
    merged_polys[key] = rm.count()

print("=== %s -> %s ===" % (def_file, out_gds))
print("dbu_nm=%d  manufacturing_grid_nm=%d" % (dbu_nm, g))
print("LAYERS (gds_layer/datatype  name):")
for (l, d, n) in layers:
    print("    %d/%d  %s" % (l, d, n))
print("OFFGRID_VERTICES_TOTAL=%d" % total_offgrid)
print("POLYGONS_AS_WRITTEN (per layer): " +
      ", ".join("%s=%d" % (k, v) for k, v in sorted(percell_polys.items())))
print("POLYGONS_TRUE_MERGED (per layer): " +
      ", ".join("%s=%d" % (k, v) for k, v in sorted(merged_polys.items())))
