#!/usr/bin/env python3
"""vendored_sync_check.py — the vendored plugin copies must BE what they claim to be.

`gds-antenna/` and `metal-fill/` are engine directories that the vibe-ic plugin vendors
VERBATIM (so a clean plugin install reaches the capability with no container re-image).
Each plugin copy ships a `PROVENANCE.md` reading:

    * upstream: `vibeic/klayout` - `metal-fill/`
    * upstream commit: <sha>

That is a falsifiable claim, and nothing was falsifying it. `metal-fill/metal_fill.py`
sat three commits behind its own vendored copy while BOTH `PROVENANCE.md` files still
named the commit at which they had last been equal -- so the fork shipped an engine that
silently ignored a per-layer `fill_datatype` (dropping dummy fill onto the SIGNAL metal
layer and reporting PASS) for as long as nobody diffed the two trees by hand. A
provenance claim that is false is worse than no claim, because it is the thing people
read INSTEAD of diffing.

This check asserts BOTH halves of the claim:

  A. CONTENT     -- every vendored file is byte-identical to its fork counterpart;
  B. PROVENANCE  -- the commit named in `PROVENANCE.md` exists in the fork AND the fork
                    content AT that commit is what was vendored.

(B) is what catches the case (A) cannot: a copy that was re-vendored from a newer fork
state without refreshing the line, and -- the case that actually happened -- a copy that
diverged while the line stood still. When (A) is green and (B) is red, the remedy is
mechanical and this program prints it: the exact `upstream commit:` line to write.

Direction is deliberately NOT inferred. This program reports WHICH side is newer only as
far as git can prove it; deciding which content wins is a human call (the fork is the
upstream of record, so the normal repair is to port the newer content back INTO the fork
and re-vendor, not to overwrite the plugin).

Usage
-----
    python3 plugin-wiring/vendored_sync_check.py \
        [--fork-root DIR]              # default: the repo this file lives in
        [--plugin-programs DIR]        # default: $VIBEIC_PLUGIN_PROGRAMS, then a few
                                       #   conventional sibling checkout locations
        [--json OUT]

Exit codes (the fork's gate convention)
---------------------------------------
    0  PASS         every vendored dir is in sync AND its provenance line is true
    1  FAIL         a divergence or a false provenance claim
    2  usage / IO error
    3  HONEST-SKIP  no plugin checkout to compare against -- never a vacuous PASS
"""
from __future__ import annotations

import argparse
import fnmatch
import json
import os
import subprocess
import sys
from pathlib import Path

_HERE = Path(__file__).resolve().parent
_MANIFEST = _HERE / "VENDORED.json"

#: Conventional places a vibe-ic plugin checkout sits next to a fork checkout.
_PLUGIN_HINTS = (
    "~/vibe-ic/vibe-ic-marketplace/plugins/vibe-ic/programs",
    "~/.claude/plugins/vibe-ic/programs",
)

PASS, FAIL, ERR, SKIP = 0, 1, 2, 3


def _git(root: Path, *args: str):
    """Run git in `root`; return (rc, stdout-bytes). Never raises."""
    try:
        p = subprocess.run(("git", "-C", str(root)) + args,
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        return p.returncode, p.stdout
    except OSError:
        return 127, b""


def _ignored(name: str, patterns) -> bool:
    return any(fnmatch.fnmatch(name, pat) for pat in patterns)


def _find_plugin_programs(explicit: str | None):
    """-> (path|None, error|None).

    An EXPLICIT location that does not exist is an ERROR, never a fall-through to a
    conventional one: silently comparing against a different checkout than the one the
    caller named is how a green result stops meaning anything. Only the conventional
    hints are allowed to miss.
    """
    for src, cand in (("--plugin-programs", explicit),
                      ("$VIBEIC_PLUGIN_PROGRAMS",
                       os.environ.get("VIBEIC_PLUGIN_PROGRAMS"))):
        if cand:
            p = Path(cand).expanduser()
            if not p.is_dir():
                return None, f"{src}={cand} is not a directory"
            return p.resolve(), None
    for hint in _PLUGIN_HINTS:
        p = Path(hint).expanduser()
        if p.is_dir():
            return p.resolve(), None
    return None, None


def _provenance_commit(text: str) -> str | None:
    """The sha on the `upstream commit:` line, or None if the line is absent."""
    for line in text.splitlines():
        s = line.strip().lstrip("*-").strip()
        low = s.lower()
        if low.startswith("upstream commit"):
            sha = s.split(":", 1)[-1].strip().strip("`")
            return sha or None
    return None


def check_dir(fork_root: Path, plugin_programs: Path, entry: dict, ignore) -> dict:
    fork_dir = fork_root / entry["fork_dir"]
    plug_dir = plugin_programs / entry["plugin_dir"]
    res = {"fork_dir": entry["fork_dir"], "plugin_dir": entry["plugin_dir"],
           "status": "IN-SYNC", "files_compared": 0, "problems": [], "remedy": []}

    if not plug_dir.is_dir():
        res["status"] = "NOT-VENDORED"
        res["note"] = f"{plug_dir} does not exist -- this plugin does not vendor it"
        return res
    if not fork_dir.is_dir():
        res["status"] = "FAIL"
        res["problems"].append(
            f"the plugin vendors {entry['plugin_dir']}/ but the fork has no "
            f"{entry['fork_dir']}/ -- the vendored copy has no upstream at all")
        return res

    vendored = sorted(f for f in plug_dir.iterdir()
                      if f.is_file() and not _ignored(f.name, ignore))

    # --- A. CONTENT -------------------------------------------------------
    diverged, missing = [], []
    for f in vendored:
        counterpart = fork_dir / f.name
        res["files_compared"] += 1
        if not counterpart.is_file():
            missing.append(f.name)
        elif counterpart.read_bytes() != f.read_bytes():
            diverged.append(f.name)
    for n in missing:
        res["problems"].append(
            f"{entry['plugin_dir']}/{n} is vendored but {entry['fork_dir']}/{n} does not "
            f"exist in the fork")
    for n in diverged:
        res["problems"].append(
            f"{entry['plugin_dir']}/{n} DIFFERS from {entry['fork_dir']}/{n} -- the "
            f"'VERBATIM copy' claim is false")
    if diverged or missing:
        res["remedy"].append(
            f"reconcile the two copies (diff "
            f"{fork_dir / (diverged + missing)[0]} {plug_dir / (diverged + missing)[0]}), "
            f"then re-vendor and refresh {entry['plugin_dir']}/PROVENANCE.md")

    # --- B. PROVENANCE ----------------------------------------------------
    prov = plug_dir / "PROVENANCE.md"
    if not prov.is_file():
        res["problems"].append(
            f"{entry['plugin_dir']}/ is vendored with no PROVENANCE.md -- there is no "
            f"claim to check")
    else:
        claimed = _provenance_commit(prov.read_text(errors="replace"))
        res["claimed_commit"] = claimed
        if not claimed:
            res["problems"].append(
                f"{entry['plugin_dir']}/PROVENANCE.md has no 'upstream commit:' line")
        else:
            rc, out = _git(fork_root, "rev-parse", "--verify", "--quiet",
                           claimed + "^{commit}")
            if rc != 0:
                res["problems"].append(
                    f"{entry['plugin_dir']}/PROVENANCE.md names commit {claimed}, which "
                    f"does not exist in this fork checkout")
            else:
                full = out.decode().strip()
                res["claimed_commit_resolved"] = full
                stale = []
                for f in vendored:
                    rc2, blob = _git(fork_root, "show",
                                     f"{full}:{entry['fork_dir']}/{f.name}")
                    if rc2 != 0:
                        stale.append(f"{f.name} (absent at {claimed[:9]})")
                    elif blob != f.read_bytes():
                        stale.append(f.name)
                if stale:
                    res["problems"].append(
                        f"{entry['plugin_dir']}/PROVENANCE.md claims commit "
                        f"{claimed[:9]}, but the vendored bytes are NOT the fork content "
                        f"at that commit: " + ", ".join(sorted(stale)))

    # The sha the line SHOULD carry, once the content halves agree: the newest commit
    # that touched this fork directory. Only meaningful when the worktree is clean for
    # that directory, so say so rather than hand over a sha that describes nothing.
    rc, out = _git(fork_root, "log", "-1", "--format=%H", "--", entry["fork_dir"])
    if rc == 0 and out.strip():
        head_for_dir = out.decode().strip()
        res["fork_head_for_dir"] = head_for_dir
        rc2, dirty = _git(fork_root, "status", "--porcelain", "--", entry["fork_dir"])
        res["fork_dir_dirty"] = bool(dirty.strip()) if rc2 == 0 else None
        if res["problems"]:
            if res.get("fork_dir_dirty"):
                res["remedy"].append(
                    f"{entry['fork_dir']}/ has uncommitted changes -- commit them first; "
                    f"the PROVENANCE line must name a commit, not a worktree")
            else:
                res["remedy"].append(
                    f"set '* upstream commit: {head_for_dir}' in "
                    f"{plug_dir / 'PROVENANCE.md'}")

    if res["problems"]:
        res["status"] = "FAIL"
    return res


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--fork-root", default=None,
                    help="fork checkout root (default: the repo containing this file)")
    ap.add_argument("--plugin-programs", default=None,
                    help="the plugin's programs/ directory holding the vendored copies")
    ap.add_argument("--manifest", default=str(_MANIFEST))
    ap.add_argument("--json", default=None, help="write the full report here")
    a = ap.parse_args(argv)

    fork_root = Path(a.fork_root).expanduser().resolve() if a.fork_root \
        else _HERE.parent
    try:
        manifest = json.loads(Path(a.manifest).read_text())
    except (OSError, ValueError) as e:
        sys.stderr.write(f"vendored_sync_check: cannot read manifest {a.manifest}: {e}\n")
        return ERR
    ignore = manifest.get("ignore", [])

    plugin_programs, locate_err = _find_plugin_programs(a.plugin_programs)
    if locate_err:
        sys.stderr.write(f"vendored_sync_check: {locate_err}\n")
        return ERR
    if plugin_programs is None:
        sys.stderr.write(
            "vendored_sync_check: HONEST-SKIP -- no vibe-ic plugin checkout found. "
            "Point --plugin-programs (or $VIBEIC_PLUGIN_PROGRAMS) at the plugin's "
            "programs/ directory to compare. Skipping is NOT a pass.\n")
        return SKIP

    dirs = [check_dir(fork_root, plugin_programs, e, ignore)
            for e in manifest["vendored_dirs"]]
    failed = [d for d in dirs if d["status"] == "FAIL"]
    report = {
        "verdict": "FAIL" if failed else "PASS",
        "fork_root": str(fork_root),
        "plugin_programs": str(plugin_programs),
        "dirs": dirs,
    }
    text = json.dumps(report, indent=2)
    if a.json:
        try:
            Path(a.json).write_text(text)
        except OSError as e:
            sys.stderr.write(f"vendored_sync_check: cannot write {a.json}: {e}\n")
            return ERR

    for d in dirs:
        print(f"{d['status']:>12}  {d['fork_dir']}/ <-> {d['plugin_dir']}/ "
              f"({d['files_compared']} file(s))")
        for p in d["problems"]:
            print(f"              ! {p}")
        for r in d["remedy"]:
            print(f"              -> {r}")
    print(f"{report['verdict']} vendored_sync_check "
          f"({len(dirs) - len(failed)}/{len(dirs)} in sync)")
    return FAIL if failed else PASS


if __name__ == "__main__":
    sys.exit(main())
