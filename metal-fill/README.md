# metal-fill — per-layer density metal-fill (KLayout native fill engine)

A general, chip/PDK-AGNOSTIC dummy-metal-fill utility built on KLayout's own C++
`Region#fill` engine. For every metal layer it tiles the die into density windows,
finds the windows below the per-layer target density, and inserts dummy fill to raise
them — **without creating any new spacing/width DRC**. Meant to run at **streamout,
before sign-off DRC**.

**Commercial equivalent:** Calibre YieldEnhancer / ICC2 metal fill (per-layer dummy
metal insertion for CMP planarity).

## The gap this closes

The flow previously had only per-layer density **checkers**
(`metal_layer_density_check.py` / `metal_fill_density_check.py`): a sparse die FAILed the
CMP density rule and was **flagged, not fixed**. There was no general FILL **emitter**
(only a design-tuned dummy-fill proof of concept). This is that emitter.

## Drawn vs dummy datatype — why fill never breaks LVS

A foundry density rule measures the **result** metal = drawn signal metal ∪ the dedicated
**dummy**-fill datatype (gf180's `metal_result = metal_drawn + metal_dummy`), while the
LVS `connect` graph uses the **drawn** datatype only. Dummy fill on the dedicated
datatype therefore (a) counts toward the CMP density measurement and (b) is invisible to
LVS — what a real tapeout does. The engine keeps the four roles apart:

| role | layer |
|---|---|
| MEASURE | drawn ∪ fill-datatype (== the deck's density layer) |
| KEEP-OUT from circuit metal | `space_to_metal` (dummy-to-circuit rule) |
| KEEP-OUT from own fill | `space` (dummy-to-dummy rule) |
| PLACE | the per-layer `fill_datatype` |

With `fill_datatype` equal to the drawn datatype the union collapses to the drawn layer
and the two spacings collapse to one — back-compat with single-datatype fill.

## DRC-safety — why the fill never violates spacing, width or the grid

* fill cell = a `width × width` square, `width` snapped to `mfg_grid_um` and ≥ one grid
  step → on-grid and wide
* keep-out from circuit metal: `window − drawn.sized(space_to_metal)` (square/Chebyshev
  sizing, so euclidean clearance ≥ `space_to_metal`)
* keep-out from prior fill: `− fill.sized(space)` plus `fill_margin = (space, space)`
* row/column step = `pitch ≥ width + space`, snapped **up** to the grid → fill-to-fill
  spacing ≥ `space` and every fill edge on the manufacturing grid
* a fill **size ladder** (large → small) packs channels the big squares cannot enter; the
  floor is the smallest square that still clears the target in open area
* a ladder rung that fits **nowhere** leaves an un-instanced `FILL_*` cell, which in GDS
  is simply another root — and a sign-off deck refuses a multi-top layout *before a
  single rule executes* (`The layout has multiple top cells`), so the failure mode is an
  ABSENT DRC verdict, not a red one. Un-instanced fill cells are pruned before write.

The sign-off test drives the fork's own `svrfdrc` on the filled GDS and asserts **0 new
spacing/width violations**; a deliberately-too-tight fill **is** caught (14842 spacing
violations in the fixture), so the gate is not vacuous.

## Density guarantee (iterative densify) and honest infeasibility

A single uniform-pitch pass can undershoot (keep-out + margin losses), so the tool
**densifies iteratively**: each pass re-measures the worst under-target window,
recomputes a tighter pitch, and fills the still-deficient windows in the space left
between prior fill. It stops when every window reaches the target, at `min pitch`, or
after `max_passes`.

A fill grid of equal width and space tops out at 25 % area density
(`(w/(w+s))² = (0.5)² = 0.25`): if the target exceeds what the geometry allows, the tool
reports `reached:false` with the achieved worst-window density — **honest, not a silent
pass**. Use a fill `width` wider than `space` when the target needs it (e.g. `width 0.30,
space 0.14` → feasible density 0.47).

## Files

| file | role |
|---|---|
| `metal_fill.py` | KLayout-driven per-layer fill (iterative densify). Run via `klayout -b -r` / `strmrun`. |
| `gen_fixtures.py` | synthetic NDA-clean sparse-die fixture generator. |
| `fill_config.example.json` | example fill config (synthetic layer numbers). |
| `tests/fill_drc.rule` | synthetic space/width deck for the DRC assertion. |
| `tests/run_fill_tests.sh` | the unfakeable SHARED-datatype gate (3 checks). |
| `tests/run_fill_dummy_datatype_test.sh` | the unfakeable DUMMY-datatype gate (7 checks). |
| `tests/check_dummy_geometry.py` | asserts LVS-invisibility / grid / clearance on the streamed GDS. |
| `tests/check_single_top.py` | asserts the streamed GDS has exactly one top cell. |

## Run

```bash
FILL_GDS=in.gds FILL_CONFIG=fill.json FILL_OUT=filled.gds FILL_REPORT=fill.json \
    [FILL_CELL=top] strmrun metal_fill.py     # or: klayout -b -r metal_fill.py
```

Config keys — every number is supplied by the caller / derived from the PDK:

| key | meaning |
|---|---|
| `boundary_layer` | optional die-outline layer for the bbox; else the full layout bbox (== KLayout DRC `extent`) |
| `window_um` | density window edge; `null`/`0` → **one whole-die window**, identical to a whole-die coverage rule |
| `mfg_grid_um` | manufacturing grid; fill width and pitch are snapped to it |
| `fill_datatype` | global dummy-fill datatype override (per-layer `fill_datatype` wins) |
| per layer | `target`, `max`, `space` (dummy-to-dummy), `space_to_metal` (dummy-to-circuit, defaults to `space`), `width`, `fill_datatype` |

`fill_datatype`: leave `null` so dummy fill lands on the metal's **own** datatype — then
it counts toward the CMP density measurement and is checked by the spacing/width DRC
deck (both key on the drawing layer). Set a separate fill datatype when your density rule
measures drawn ∪ dummy and your LVS `connect` uses drawn only — the engine then measures
the union and places on the dummy layer, so the fill is invisible to LVS.

## Unfakeable gates

`tests/run_fill_tests.sh` — the SHARED-datatype path:

1. a sparse layer (~4 %) below the 30 % target is **raised to ≥ target** (worst-window
   density after ≥ target);
2. the filled GDS has **0 new spacing and 0 new width DRC** (`svrfdrc`, before == after
   == 0);
3. **negative control:** a deliberately-too-tight (0.05 µm) fill **is caught** by the
   same deck — so [2] is a real pass, not a vacuous one.

`tests/run_fill_dummy_datatype_test.sh` — the DUMMY-datatype path. Everything the engine
grew after the shared-datatype work is invisible to the test above: it passes
byte-for-byte against an engine with none of it, which is how `metal_fill.py` rotted
three commits behind its vendored copy without a single test going red. So:

1. `window_um: null` collapses to one whole-die window (worst-window == global, exactly);
2. dummy fill on a **separate** datatype reaches the target — the engine measures
   drawn ∪ dummy, not drawn alone;
3. the fill is **LVS-invisible**: the drawn layer's shape count is unchanged;
4. every dummy-fill edge lands on the declared manufacturing grid;
5. `space_to_metal` is honoured — plus a negative control that the proximity probe fires
   at all, so [5] is not vacuously true;
6. rungs that fit nowhere are **pruned**: the stream has exactly one top cell (the
   fixture is chosen so rungs really are left over — the report lists them);
7. **negative control:** an infeasible (0.99) target reports `reached:false`, so [2] is a
   real pass rather than a flag that is always true.

Both skip (green) if no KLayout / `svrfdrc` binary is on `PATH`.

## Real-node validation

On the routed **sky130 `spm`** design (open PDK), filling met1/met2/met3 to a 0.30
target (fill width 0.30, space 0.14):

| layer | global density before → after | worst-window after | reached | new DRC |
|---|---|---|---|---|
| met1 | 0.158 → 0.415 | 0.309 | ✅ | 0 spacing / 0 width |
| met2 | 0.052 → 0.406 | 0.324 | ✅ | — |
| met3 | 0.052 → 0.435 | 0.401 | ✅ | — |

`over_max` = false on every layer. The commercial-PDK + benchmark-IC validation is
owner-driven.
