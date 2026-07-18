# plugin-wiring — turnkey references for the Core agent to land in the vibe-ic plugin

The engines (`gds-antenna/`, `metal-fill/`) are chip/PDK-AGNOSTIC KLayout-fork tools.
These two reference gate programs wire them into the flow. Per the five-agent
governance (**only the Core agent changes the plugin/MCP**), a fork-enhancement agent
does NOT push these to the `vibe-ic` plugin repo — they are provided here, tested, for
Core to land with a proper version bump.

## What to land

| copy this fork file | to plugin path | role |
|---|---|---|
| `plugin-wiring/gds_antenna_deck_check.py` | `plugins/vibe-ic/programs/gds_antenna_deck_check.py` | independent GDS-geometry antenna sign-off + router cross-check |
| `plugin-wiring/metal_fill_emit.py` | `plugins/vibe-ic/programs/metal_fill_emit.py` | per-layer density metal-fill at streamout |

Both mirror the plugin's gate conventions: `main(argv) -> int`, JSON output, §4.05
honest-skip (rc 3) when no KLayout binary / tool is present — never a vacuous PASS.

## Tool discovery (how the plugin finds the fork engines)

Both wrappers resolve the engine via, in order:
1. `$VIBEIC_KLAYOUT_TOOLS/{gds-antenna,metal-fill}/*.py` — set this to where the fork is
   baked in the `vibeic-eda` container;
2. a copy shipped next to the wrapper (`programs/gds_antenna/…`, `programs/metal_fill/…`).

**Container dependency:** the fork engines under `gds-antenna/` and `metal-fill/` must be
baked into the `vibeic-eda` image (or shipped as a plugin-local copy per (2)) before the
gates run there — coordinate the re-image with the integration-build owner. The KLayout
runner is `strmrun` or `klayout -b -r`, already on the container PATH.

## Where they hook in `phase3_one_shot_runner.py`

* **metal_fill_emit** — in streamout, AFTER the GDS is written and BEFORE
  `metal_layer_density_check` / sign-off DRC. Run it `--in-place` (or write a
  `*.filled.gds` and repoint the density check + DRC at it) so the density checker and
  DRC see the FILLED layout. It is DRC-safe by construction (proven by
  `metal-fill/tests/run_fill_tests.sh`). This complements OpenROAD `filler_placement`
  (cell-level fill): it targets **per-layer metal density**, which filler placement does
  not control directly.
* **gds_antenna_deck_check** — at antenna sign-off (Step 26 region), as an INDEPENDENT
  second gate beside the router-report consumer (`antenna_report_check.py`). It reads the
  streamed GDS + an antenna config, and cross-checks its geometry count against the
  router's `reports/phase3/antenna.rpt`. A clean/dirty DISAGREEMENT is a hard FAIL
  (surfaces a router-vs-geometry inconsistency instead of averaging it away).

The per-PDK antenna config (layer stack + per-metal ratio bounds) and the fill config
(per-layer target/max density + space/width) are supplied by the caller — the foundry
values for a real sign-off. `*.example.json` in each tool dir shows the shape with
generic, DISCLOSED placeholder numbers (no foundry data).

## Tested

Both wrappers were run end-to-end against the synthetic fixtures with
`VIBEIC_KLAYOUT_TOOLS` pointing at the fork root: the antenna gate returns FAIL +
cross-check AGREE on a violation fixture (deck 1 == router 1); the fill gate returns
PASS with the sparse layer raised 0.037 → 0.386.
