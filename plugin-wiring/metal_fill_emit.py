#!/usr/bin/env python3
"""metal_fill_emit.py — plugin gate: per-layer density metal-fill at streamout.

REFERENCE wrapper to be landed by the Core agent into
`vibe-ic-marketplace/plugins/vibe-ic/programs/`. It wires the KLayout fork's
`metal-fill/metal_fill.py` (native fill-engine per-layer density fill) into streamout,
BEFORE sign-off DRC / the per-layer density check (`metal_layer_density_check.py`), so a
sparse die is FIXED (fill inserted) rather than only flagged.

Split of responsibility (tool vs plugin):
  * the fill ENGINE + density-target algorithm live in the KLayout fork (metal-fill/*) —
    chip-AGNOSTIC;
  * this PLUGIN gate only locates the tool, feeds it the streamed GDS + fill config,
    writes the filled GDS back into the streamout path, and reports per-layer
    before/after density so the downstream density checker sees the filled layout.

Ordering contract: run AFTER the GDS is streamed and BEFORE `metal_layer_density_check`
/ sign-off DRC. The tool is DRC-safe by construction (keep-out + fill_margin + pitch),
proven by the fork's `metal-fill/tests/run_fill_tests.sh`.

Tool discovery / KLayout runner: identical scheme to gds_antenna_deck_check.py
($VIBEIC_KLAYOUT_TOOLS/metal-fill/metal_fill.py, or a plugin-shipped copy).

§4.05 honest behavior: no KLayout binary / tool -> HONEST-SKIP (rc 3). A run that cannot
reach the target on some layer returns PARTIAL (rc 1) with the achieved worst-window
density DISCLOSED — never a silent "filled".

Usage:
    python3 metal_fill_emit.py --gds <in.gds> --config <fill.json> --out <filled.gds>
        [--cell <top>] [--report R.json] [--in-place]
    main(argv) -> int : 0 PASS(all reached) / 1 PARTIAL / 2 IO / 3 HONEST-SKIP
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

_HERE = Path(__file__).resolve().parent


def _find_klayout():
    for c in ("strmrun", "klayout"):
        p = shutil.which(c)
        if p:
            return (p, []) if c == "strmrun" else (p, ["-b", "-r"])
    return None


def _find_tool():
    env = os.environ.get("VIBEIC_KLAYOUT_TOOLS")
    cands = []
    if env:
        cands.append(Path(env) / "metal-fill" / "metal_fill.py")
    cands.append(_HERE / "metal_fill" / "metal_fill.py")
    for c in cands:
        if c.is_file():
            return c
    return None


def run(gds: Path, config: Path, out: Path, cell: str | None, report: Path | None):
    kl = _find_klayout()
    tool = _find_tool()
    if kl is None or tool is None:
        return {"verdict": "HONEST_SKIP",
                "reason": ("no KLayout binary on PATH" if kl is None
                           else "metal_fill.py tool not found "
                                "(set VIBEIC_KLAYOUT_TOOLS)")}
    if not gds.is_file() or not config.is_file():
        return {"verdict": "IO_ERROR",
                "error": f"missing gds ({gds}) or config ({config})"}
    rep = report or out.with_suffix(".fill.json")
    env = dict(os.environ, FILL_GDS=str(gds), FILL_CONFIG=str(config),
               FILL_OUT=str(out), FILL_REPORT=str(rep))
    if cell:
        env["FILL_CELL"] = cell
    binp, flags = kl
    try:
        subprocess.run([binp, *flags, str(tool)], env=env,
                       capture_output=True, timeout=3600, check=False)
    except (OSError, subprocess.TimeoutExpired) as e:
        return {"verdict": "IO_ERROR", "error": f"metal_fill run failed: {e}"}
    if not rep.is_file():
        return {"verdict": "IO_ERROR", "error": "metal_fill produced no report"}
    return json.loads(rep.read_text())


def main(argv=None):
    ap = argparse.ArgumentParser(description="Per-layer density metal-fill (streamout).")
    ap.add_argument("--gds", required=True)
    ap.add_argument("--config", required=True)
    ap.add_argument("--out", default=None,
                    help="filled GDS out (default: alongside input, .filled.gds)")
    ap.add_argument("--in-place", action="store_true",
                    help="overwrite the input GDS with the filled layout")
    ap.add_argument("--cell", default=None)
    ap.add_argument("--report", default=None)
    ap.add_argument("--json", dest="json_out", default=None)
    ns = ap.parse_args(argv)
    gds = Path(ns.gds)
    out = Path(ns.out) if ns.out else (
        gds if ns.in_place else gds.with_suffix(".filled.gds"))
    res = run(gds, Path(ns.config), out, ns.cell,
              Path(ns.report) if ns.report else None)
    text = json.dumps(res, indent=2)
    if ns.json_out:
        Path(ns.json_out).write_text(text)
    print(text)
    v = res.get("verdict")
    return {"PASS": 0, "PARTIAL": 1, "IO_ERROR": 2, "HONEST_SKIP": 3, "ERROR": 2}.get(
        v, 1)


if __name__ == "__main__":
    sys.exit(main())
