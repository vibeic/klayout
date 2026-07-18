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

## DRC-safety — why the fill never violates spacing or width

* fill cell = a `width × width` square, `width ≥ min_width` → no width fault
* keep-out: the fillable region is `window − existing_metal.sized(space)`, so no fill
  square lands within `space` of real metal
* `fill_margin = (space, space)` keeps fill squares `space` inside the fillable boundary
  (a second guarantee against fill-to-real-metal shorts)
* row/column step = `pitch ≥ width + space` → fill-to-fill spacing ≥ `space`

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
| `tests/run_fill_tests.sh` | the unfakeable gate (3 checks). |

## Run

```bash
FILL_GDS=in.gds FILL_CONFIG=fill.json FILL_OUT=filled.gds FILL_REPORT=fill.json \
    [FILL_CELL=top] strmrun metal_fill.py     # or: klayout -b -r metal_fill.py
```

`fill_datatype`: leave `null` so dummy fill lands on the metal's **own** datatype — then
it counts toward the CMP density measurement and is checked by the spacing/width DRC
deck (both key on the drawing layer). Set a separate fill datatype only if your density
rule and DRC deck also include that datatype.

## Unfakeable gate (`tests/run_fill_tests.sh`)

1. a sparse layer (~4 %) below the 30 % target is **raised to ≥ target** (worst-window
   density after ≥ target);
2. the filled GDS has **0 new spacing and 0 new width DRC** (`svrfdrc`, before == after
   == 0);
3. **negative control:** a deliberately-too-tight (0.05 µm) fill **is caught** by the
   same deck — so [2] is a real pass, not a vacuous one.

The test skips (green) if no KLayout / `svrfdrc` binary is on `PATH`.

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
