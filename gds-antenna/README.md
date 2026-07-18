# gds-antenna — GDS-geometry process-antenna DRC (KLayout-native)

An **independent, authoritative** process-antenna sign-off gate that runs on the
*streamed GDS geometry* — not on a router report. It reconstructs the as-fabricated
conductor connectivity with KLayout's own `LayoutToNetlist` engine and, for every metal
layer, computes the per-net **antenna ratio** = (connected metal area) / (connected
gate area). A net whose ratio exceeds the per-layer limit is a violation.

**Commercial equivalent:** Calibre antenna (PERC / nmDRC) authoritative GDS-level ratio
checks — the plasma-charge / process-antenna sign-off gate.

## The gap this closes

The flow previously had only a *router-report* antenna consumer
(`antenna_report_check.py`, which reads OpenROAD's own count). There was **no
independent GDS-geometry antenna gate**, so the router's number could never be
cross-checked against the physical geometry. This tool provides that second,
independent number — and `xcheck_router.py` cross-checks the two counts.

## Why "staged" connectivity is the physically-correct model

Process-antenna damage happens *during* fabrication: when metal layer `k` is being
plasma-etched, only layers `1..k` exist. Charge on the layer-`k` conductor (plus
everything beneath it — all one exposed node) discharges into any gate on that node.
Upper metals are not yet deposited and cannot relieve it. So for each metal layer the
connectivity is rebuilt using **only** the conductors up to and including `k`. An
upper-metal jumper that would relieve the antenna in the *final* netlist is correctly
ignored at the stage where the damage occurs (test #3 proves this).

The diffusion/active layer is **not** placed in the antenna node by default: a proper
antenna-diode discharge path needs deliberate diode extraction (you cannot simply merge
all diffusion — that would short each gate net to its own source/drain). So the deck
runs **conservative** (`diode_credit:false`); modelling explicit antenna diodes is the
commercial-parity extension (Calibre PERC does it via a diode layer + connectivity).

## Files

| file | role |
|---|---|
| `antenna_check.py` | KLayout-driven checker (staged connectivity, per-layer + cumulative). Run via `klayout -b -r` / the fork's `strmrun`. |
| `xcheck_router.py` | pure-Python cross-check of the deck's count vs the OpenROAD `check_antennas` count. Hard gate = clean/dirty agreement. |
| `gen_fixtures.py` | synthetic NDA-clean fixture generator (hand-computable ratios). |
| `antenna_config.example.json` | example deck config (synthetic layer numbers). |
| `tests/run_antenna_tests.sh` | the unfakeable gate (4 checks). |

## Run

```bash
# checker (parameters via the environment — KLayout scripts have no argv)
ANT_GDS=design.gds ANT_CONFIG=antenna.json ANT_OUT=antenna.json [ANT_CELL=top] \
    strmrun antenna_check.py            # or: klayout -b -r antenna_check.py

# cross-check the geometry count against the router's count
python3 xcheck_router.py --deck antenna.json --router antenna.rpt
```

Config is chip/PDK-AGNOSTIC — layer numbers and the per-metal `ratio` bounds are
supplied by the caller (foundry values for a real sign-off; the committed example uses
generic placeholders).

## Unfakeable gate (`tests/run_antenna_tests.sh`)

1. a fabricated ratio-50 antenna (met1 area / gate area) is **FLAGGED**, and the
   reported ratio **equals the hand-computed 50.0**;
2. a clean ratio-10 structure **PASSes**;
3. the **staged model** keeps the met1-stage ratio at 50.0 even when a big met2 jumper
   is present (the jumper does not exist yet at the met1 etch stage);
4. the router **cross-check AGREEs** on clean/dirty, and a disagreement is caught.

The test skips (green) if no KLayout binary is on `PATH`.

## Real-node validation

On the routed **sky130 `spm`** design (open PDK): the deck ran on the real streamed GDS
and reported worst per-layer antenna ratios li1 = 31.5, met1 = 26.0, met2 = 14.7 —
verdict PASS, 0 violations; `xcheck_router.py` **AGREEs** with the router's own antenna
report (both 0). The commercial-PDK + benchmark-IC validation is owner-driven.
