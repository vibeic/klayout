# vibeic KLayout LEF/DEF streamout fixes — reproducible proof

Three design-agnostic fixes to the LEF/DEF importer + GDS streamout path
(`src/plugins/streamers/lefdef/db_plugin/`), matching the behaviour of
commercial streamout (Calibre / ICC2 / Innovus) and removing the two largest
false-DRC populations plus the LVS-breaking compact-layer GDS measured when
driving real silicon through the OSS flow.

| # | Fix | Default | Enable / control |
|---|-----|---------|------------------|
| A | Honor tech-LEF `MANUFACTURINGGRID` — snap instance-placement displacements + generated via/enclosure coords to the manufacturing grid (kills the ~76% OFFGRID false-DRC) | **ON** | `KLAYOUT_LEFDEF_MFG_GRID_SNAP=0` to disable; `KLAYOUT_LEFDEF_MFG_GRID=<microns>` to override the grid |
| B | Merge-abutting streamout — union same-layer polygons touching across instance boundaries at write time (kills the ~26% boundary false min-spacing/width DRC) | **OFF (opt-in)** | `KLAYOUT_LEFDEF_MERGE_ABUTTING=1` |
| C | Foundry layer-map instead of the compact 1..N fallback (fixes GDS that Magic can't read / LVS-breaking layers) | explicit map: **ON** · same-dir auto-discover: **OFF (opt-in)** | `KLAYOUT_LEFDEF_LAYERMAP=<path>` (explicit, always applied) · `KLAYOUT_LEFDEF_LAYERMAP_AUTODISCOVER=1` (scan `*.map`/`*.layermap` next to the DEF) |

All three preserve stock behaviour by default except Fix A (whose snap is a no-op
on grid-legal geometry, so it only corrects coordinates that are already off the
manufacturing grid). The upstream `dbLEFDEFImportTests` suite passes unchanged
(90 executed / 48 skipped in a Qt-less build, identical to stock).

## Reproduce (Qt-less build: `build.sh -without-qt -noruby -nolibgit2`)

```
export PYTHONPATH=<bin>/pymod LD_LIBRARY_PATH=<bin>
# Fix A — off-grid vertex count (grid = 5 nm)
KLAYOUT_LEFDEF_MFG_GRID_SNAP=0 python3 proof.py gridtest.def out.gds 5   # OFFGRID_VERTICES_TOTAL=8  (stock)
                               python3 proof.py gridtest.def out.gds 5   # OFFGRID_VERTICES_TOTAL=0  (fixed)
# Fix B — abutting polygons on met1 (1/3)
                                  python3 proof.py mergetest.def o.gds 5 # POLYGONS_AS_WRITTEN 1/3=2 (stock)
KLAYOUT_LEFDEF_MERGE_ABUTTING=1   python3 proof.py mergetest.def o.gds 5 # POLYGONS_AS_WRITTEN 1/3=1 (merged)
# Fix C — GDS layer numbers
                                              python3 proof.py gridtest.def o.gds 5 # met1.PIN = 1/2   (compact, stock)
KLAYOUT_LEFDEF_LAYERMAP=./foundry.layermap    python3 proof.py gridtest.def o.gds 5 # met1.PIN = 68/16 (foundry)
```

`proof.py` reads the DEF (with `mytech.lef`), writes a GDS, and reports, from the
flattened top cell: total off-grid vertices vs the manufacturing grid, per-layer
as-written vs truly-merged polygon counts, and the GDS layer/datatype numbers
produced.
