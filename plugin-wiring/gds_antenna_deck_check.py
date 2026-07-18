#!/usr/bin/env python3
"""gds_antenna_deck_check.py — plugin gate: independent GDS-geometry antenna sign-off.

REFERENCE wrapper to be landed by the Core agent into
`vibe-ic-marketplace/plugins/vibe-ic/programs/`. It wires the KLayout fork's
`gds-antenna/antenna_check.py` (staged-connectivity antenna DRC on the streamed GDS)
into the flow as a first-class gate, and cross-checks its count against the router's own
`antenna.rpt` via the fork's `gds-antenna/xcheck_router.py`.

Split of responsibility (tool vs plugin):
  * the ENGINE + model live in the KLayout fork (gds-antenna/*) — chip-AGNOSTIC;
  * this PLUGIN gate only locates the tool, feeds it the project's GDS + antenna
    config, and turns the two independent counts into a PASS/FAIL verdict.

Tool discovery (first hit wins):
  1. $VIBEIC_KLAYOUT_TOOLS/gds-antenna/antenna_check.py   (fork baked into vibeic-eda)
  2. a copy shipped next to this program (programs/gds_antenna/antenna_check.py)
KLayout runner: `strmrun` or `klayout -b -r` on PATH.

§4.05 honest behavior: no KLayout binary or no tool script -> HONEST-SKIP (rc 3), never
a vacuous PASS. A present antenna config whose deck FAILs, or a clean/dirty DISAGREEMENT
with the router, is a hard FAIL (rc 1).

Usage:
    python3 gds_antenna_deck_check.py <project_dir> --gds <gds> --config <antenna.json>
        [--router <antenna.rpt>] [--cell <top>] [--json OUT]
    main(argv) -> int : 0 PASS / 1 FAIL / 2 arg-IO error / 3 HONEST-SKIP
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


def _find_tool(name):
    env = os.environ.get("VIBEIC_KLAYOUT_TOOLS")
    cands = []
    if env:
        cands.append(Path(env) / "gds-antenna" / name)
    cands.append(_HERE / "gds_antenna" / name)      # plugin-shipped copy
    for c in cands:
        if c.is_file():
            return c
    return None


def run(project_dir: Path, gds: Path, config: Path, router: Path | None,
        cell: str | None):
    kl = _find_klayout()
    checker = _find_tool("antenna_check.py")
    if kl is None or checker is None:
        return {"verdict": "HONEST_SKIP",
                "reason": ("no KLayout binary on PATH" if kl is None
                           else "antenna_check.py tool not found "
                                "(set VIBEIC_KLAYOUT_TOOLS)")}
    if not gds.is_file() or not config.is_file():
        return {"verdict": "IO_ERROR",
                "error": f"missing gds ({gds}) or config ({config})"}
    out_json = project_dir / "antenna_deck.json"
    env = dict(os.environ, ANT_GDS=str(gds), ANT_CONFIG=str(config),
               ANT_OUT=str(out_json))
    if cell:
        env["ANT_CELL"] = cell
    binp, flags = kl
    try:
        subprocess.run([binp, *flags, str(checker)], env=env,
                       capture_output=True, timeout=1800, check=False)
    except (OSError, subprocess.TimeoutExpired) as e:
        return {"verdict": "IO_ERROR", "error": f"antenna_check run failed: {e}"}
    if not out_json.is_file():
        return {"verdict": "IO_ERROR", "error": "antenna_check produced no JSON"}
    deck = json.loads(out_json.read_text())
    res = {"verdict": deck.get("verdict"), "deck": deck}

    # cross-check vs router antenna.rpt if present
    xtool = _find_tool("xcheck_router.py")
    router = router or _discover_router(project_dir)
    if router and router.is_file() and xtool:
        sys.path.insert(0, str(xtool.parent))
        import importlib
        xr = importlib.import_module("xcheck_router")
        xres = xr.cross_check(out_json, router)
        res["cross_check"] = xres
        if xres.get("verdict") == "DISAGREE":
            res["verdict"] = "FAIL"
            res["fail_reason"] = xres.get("detail")
    else:
        res["cross_check"] = {"verdict": "SKIP", "reason": "no router antenna.rpt"}

    if res["verdict"] not in ("PASS", "FAIL", "HONEST_SKIP"):
        res["verdict"] = "FAIL" if deck.get("violations", 0) else "PASS"
    return res


def _discover_router(project_dir: Path):
    for pat in ("**/reports/phase3/antenna.rpt", "**/antenna*.rpt"):
        hits = sorted(project_dir.glob(pat))
        if hits:
            return hits[0]
    return None


def main(argv=None):
    ap = argparse.ArgumentParser(description="GDS-geometry antenna sign-off gate.")
    ap.add_argument("project_dir")
    ap.add_argument("--gds", required=True)
    ap.add_argument("--config", required=True)
    ap.add_argument("--router", default=None)
    ap.add_argument("--cell", default=None)
    ap.add_argument("--json", dest="json_out", default=None)
    ns = ap.parse_args(argv)
    res = run(Path(ns.project_dir), Path(ns.gds), Path(ns.config),
              Path(ns.router) if ns.router else None, ns.cell)
    text = json.dumps(res, indent=2)
    if ns.json_out:
        Path(ns.json_out).write_text(text)
    print(text)
    v = res.get("verdict")
    return {"PASS": 0, "FAIL": 1, "IO_ERROR": 2, "HONEST_SKIP": 3}.get(v, 1)


if __name__ == "__main__":
    sys.exit(main())
