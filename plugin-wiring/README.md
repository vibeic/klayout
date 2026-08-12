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

Those two wrappers are handed over for Core to **own and evolve** — once landed, the
plugin's copy is the live one and this fork copy is a historical reference. That is the
opposite of the vendored *engines* below, which must stay verbatim.

## Vendored engines — the plugin copies must BE what they claim

The plugin also vendors the engine directories themselves, so a clean install reaches
the capability with no container re-image. Each vendored copy carries a `PROVENANCE.md`:

```
* upstream: `vibeic/klayout` — `metal-fill/`
* upstream commit: <sha>
```

That is a **falsifiable claim**, and for three commits nothing falsified it:
`metal-fill/metal_fill.py` sat behind its own vendored copy while both `PROVENANCE.md`
files still named the commit at which the two had last been equal. The fork therefore
shipped an engine that **silently ignored a per-layer `fill_datatype`** — dropping dummy
fill onto the signal-metal layer, reporting `fill_datatype: 0` and `verdict: PASS` — for
as long as nobody diffed the two trees by hand. A provenance claim that is false is worse
than no claim, because it is the thing people read *instead of* diffing.

`vendored_sync_check.py` asserts both halves of the claim:

| | assertion |
|---|---|
| **A. content** | every vendored file is byte-identical to its fork counterpart |
| **B. provenance** | the commit named in `PROVENANCE.md` exists **and** the fork content *at that commit* is what was vendored |

(B) is what catches what (A) cannot: a copy re-vendored from a newer fork state without
refreshing the line. When (A) is green and (B) is red the fix is mechanical, and the
program prints the exact line to write.

```bash
# from the fork root
python3 plugin-wiring/vendored_sync_check.py \
    --plugin-programs ~/vibe-ic/vibe-ic-marketplace/plugins/vibe-ic/programs
#  rc 0 PASS · 1 FAIL · 2 usage/IO · 3 HONEST-SKIP (no plugin checkout — never a pass)
```

`VENDORED.json` lists the directories under the verbatim claim. **Add a directory here
the moment the plugin starts vendoring it**, otherwise it is unguarded — the check can
only police what it is told about.

### When you change a vendored engine

1. change it **in the fork** (never in place in the plugin — that is exactly how this
   divergence started) and land it;
2. re-vendor: copy the fork files over the plugin copy;
3. refresh `upstream commit:` in that copy's `PROVENANCE.md` to the sha
   `vendored_sync_check.py` prints;
4. re-run the check — it must be `PASS`.

Steps 2–4 are the Core agent's (only Core changes the plugin), so a fork-side change to a
vendored engine is not finished when it lands in the fork: it is finished when the plugin
copy and its provenance line follow. Until then the check is honestly red.

`tests/run_vendored_sync_test.sh` is the gate for the checker itself — hermetic
(throw-away git repo in `$TMPDIR`, no KLayout, no real plugin checkout), and it proves
all three verdicts plus the honest-skip, so a green run means the checker can still fail.

## Tool discovery (how the plugin finds the fork engines)

Both wrappers resolve the engine via, in order:
1. `$VIBEIC_KLAYOUT_TOOLS/{gds-antenna,metal-fill}/*.py` — set this to where the fork is
   baked in the `vibeic-eda` container, or to a fork checkout;
2. a copy shipped next to the wrapper (`programs/gds_antenna/…`, `programs/metal_fill/…`).

**Directory spelling.** This fork names its engine directories with hyphens
(`gds-antenna/`, `metal-fill/`, and likewise `mp-color/`, `perc-latchup/`,
`cmp-gradient/` …); the plugin names the same engines with underscores, because its
program directories sit next to importable Python. `_klayout_launch.find_engine`
therefore tries BOTH spellings under `$VIBEIC_KLAYOUT_TOOLS` (the caller's first), so
pointing the variable at a fork checkout resolves — it did not before vibeic/klayout#113
finding 5, where it matched nothing and fell back to the vendored copy in silence. If the
override is set and carries no such engine, the miss is now printed on stderr; a silent
fall-through is what let the mismatch survive.

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
