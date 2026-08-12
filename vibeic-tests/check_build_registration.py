#!/usr/bin/env python3
"""check_build_registration.py -- the fork's own sources must stay in the build
that produces the shipped binaries.

THE DEFECT THIS EXISTS FOR
--------------------------
The image is built with qmake (`build.sh` -> `tools/klayout/Dockerfile`). The
fork's largest patch, the native SVRF DRC engine (6,673 lines over 23 commits),
reaches that build through exactly ONE line of registration:

    src/buddies/src/bd/bd.pro
      SOURCES = ... $$PWD/../../../plugins/tools/svrf_drc/db_plugin/dbSVRFEngine.cc

`src/plugins/tools/svrf_drc/svrf_drc.pro` is a deliberately EMPTY subdirs project
-- it exists only so the `$$files($$PWD/*)` auto-glob in tools.pro accepts the
folder -- so nothing else names those files. bd.pro is a file upstream edits too.
If a merge drops our two lines from its SOURCES list, the tree still configures,
still builds and still ships, with the engine silently gone; the wheel CI would
not notice either, because setup.py globs `src/*/**/pysetup.toml` and svrf_drc
has none.

That is the same shape as OpenROAD's `test-registration-parity`: code registered
in one build system while a different one is what actually runs.

WHAT IT ASSERTS
---------------
  A. every `path` in vibeic-tests/FORK_SOURCES.json exists in the tree;
  B. its `built_by` project exists and NAMES it (directly, or through a .pri the
     project includes);
  C. that project is reachable from `src/klayout.pro` -- walking SUBDIRS,
     including the `$$files($$PWD/*)` auto-globs the plugin tree uses -- so a
     project that is present but no longer part of the build graph is red too;
  D. DRIFT, when the upstream ref is resolvable: every source the fork adds under
     src/ is in the manifest, and every TEST() the fork adds under
     src/*/unit_tests is in `cpp_tests`. A guard the fork adds and forgets to
     declare is the defect that produced this file.

(D) needs git and an upstream ref. Where it cannot be answered it is reported as
a NAMED partial skip -- (A)-(C) still run and still decide the exit code, so the
check is never vacuous, and the skip is never silent.

No build, no container, no PDK: pure text over the .pro/.pri graph.

Usage: python3 vibeic-tests/check_build_registration.py [--root DIR] [--json OUT]
Exit:  0 PASS   1 FAIL   2 usage / the manifest could not be read
"""
from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

PASS, FAIL, ERR = 0, 1, 2

#: A qmake assignment we care about, with `\`-continuations already joined.
_ASSIGN = re.compile(r"^\s*(\w+)\s*[-+*]?=\s*(.*)$")
_INCLUDE = re.compile(r"include\s*\(\s*([^)]*?)\s*\)")
_TEST_DECL = re.compile(r"^\+TEST\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*\)")


def _join_continuations(text: str) -> list[str]:
    """qmake line-continuations (`\\` at end of line) folded into one line."""
    out, buf = [], ""
    for raw in text.splitlines():
        line = raw.split("#", 1)[0].rstrip()
        if line.endswith("\\"):
            buf += line[:-1] + " "
            continue
        out.append(buf + line)
        buf = ""
    if buf:
        out.append(buf)
    return out


def _values(text: str, key: str) -> list[str]:
    """Every token assigned to `key` anywhere in `text` (= and += alike).

    Deliberately blind to `equals(HAVE_QT, "0")` guards: this check asks whether
    our file is in the graph AT ALL, and being permissive about which
    configuration reaches it keeps it from going red on a build option.
    """
    vals: list[str] = []
    for line in _join_continuations(text):
        m = _ASSIGN.match(line)
        if m and m.group(1) == key:
            vals += m.group(2).split()
    return vals


def _project_text(pro: Path) -> str:
    """`pro`'s text plus the text of every .pri it includes (one level deep,
    which is how deep this tree nests)."""
    text = pro.read_text(errors="replace")
    for inc in _INCLUDE.findall(text):
        rel = inc.replace("$$PWD", str(pro.parent)).strip()
        p = Path(rel) if Path(rel).is_absolute() else pro.parent / rel
        try:
            text += "\n" + p.read_text(errors="replace")
        except OSError:
            pass
    return text


def _child_projects(pro: Path) -> list[Path]:
    """The .pro files `pro` pulls in through SUBDIRS, globs included."""
    text = pro.read_text(errors="replace")
    here = pro.parent
    entries: list[str] = []

    # `SUBDIRS = $$SUBDIR_LIST` where SUBDIR_LIST came from $$files($$PWD/*):
    # the plugin tree registers folders by GLOB, so a literal SUBDIRS scan would
    # wrongly report every plugin as unreachable.
    globbed = "$$files(" in text and "SUBDIRS" in text
    for key in ("SUBDIRS", "SUBDIR_LIST"):
        entries += [e for e in _values(text, key) if not e.startswith("$$")]

    children: list[Path] = []
    names = set(entries)
    if globbed:
        names |= {d.name for d in here.iterdir() if d.is_dir()}
    for name in sorted(names):
        d = here / name
        if not d.is_dir():
            continue
        cand = d / (name + ".pro")
        if cand.is_file():
            children.append(cand)
            continue
        pros = sorted(d.glob("*.pro"))
        if len(pros) == 1:
            children.append(pros[0])
    return children


def reachable_projects(root: Path) -> set[Path]:
    """Every .pro reachable from src/klayout.pro through SUBDIRS."""
    top = root / "src" / "klayout.pro"
    seen: set[Path] = set()
    stack = [top] if top.is_file() else []
    while stack:
        pro = stack.pop()
        if pro in seen:
            continue
        seen.add(pro)
        stack += [c for c in _child_projects(pro) if c not in seen]
    return seen


def _git(root: Path, *args: str):
    try:
        p = subprocess.run(("git", "-C", str(root)) + args,
                           capture_output=True, text=True, timeout=120)
        return p.returncode, p.stdout
    except (OSError, subprocess.SubprocessError):
        return 127, ""


def _fork_delta_under_src(root: Path, ref: str):
    """-> (added, modified, tests, why-not) for what the fork does on top of `ref`.

    ADDED and MODIFIED are separated on purpose. A file the fork ADDS is
    registered only by us and must be declared in the manifest -- that is the
    SVRF-engine case this program exists for. A file the fork only MODIFIES is
    registered by upstream; demanding a manifest entry for each would add churn
    on every merge and would name upstream's build as ours. It still has to be
    IN the build, so it is checked directly instead.
    """
    rc, _ = _git(root, "rev-parse", "--verify", "--quiet", ref + "^{commit}")
    if rc != 0:
        return None, None, None, (
            f"the upstream ref `{ref}` does not resolve in this checkout, so "
            f"'what does the fork add' has no answer here (a CI checkout has no "
            f"upstream remote; the gatekeeper clones do)")
    added: list[str] = []
    modified: list[str] = []
    for status, bucket in (("A", added), ("M", modified)):
        rc, out = _git(root, "diff", "--name-only", f"--diff-filter={status}",
                       f"{ref}...HEAD", "--", "src")
        if rc != 0:
            return None, None, None, "git diff against the upstream ref failed"
        bucket += [f for f in out.split() if f.endswith((".cc", ".h"))]
    tests: list[str] = []
    for f in added + modified:
        if "/unit_tests/" not in f or not f.endswith(".cc"):
            continue
        rc, d = _git(root, "diff", f"{ref}...HEAD", "--", f)
        if rc == 0:
            for line in d.splitlines():
                m = _TEST_DECL.match(line)
                if m:
                    tests.append(m.group(1))
    return added, modified, tests, None


def _named_by_any_project(root: Path, rel: str, reach: set[Path]) -> bool:
    """Is `rel`'s basename named by any project inside the build graph?"""
    name = Path(rel).name
    return any(name in _project_text(p) for p in reach)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--root", default=str(Path(__file__).resolve().parents[1]))
    ap.add_argument("--upstream-ref", default=None,
                    help="override the manifest's upstream_ref")
    ap.add_argument("--json", default=None)
    a = ap.parse_args(argv)

    root = Path(a.root).resolve()
    manifest_path = root / "vibeic-tests" / "FORK_SOURCES.json"
    try:
        manifest = json.loads(manifest_path.read_text())
    except (OSError, ValueError) as exc:
        print(f"ERROR: cannot read {manifest_path}: {exc}")
        return ERR

    sources = manifest.get("sources") or []
    cpp_tests = manifest.get("cpp_tests") or []
    if not sources:
        print("ERROR: FORK_SOURCES.json declares no `sources`. An empty manifest "
              "makes this check pass while asserting nothing.")
        return ERR

    reach = reachable_projects(root)
    problems: list[str] = []
    rows = []

    for s in sources:
        rel, by = s.get("path", ""), s.get("built_by", "")
        f, pro = root / rel, root / by
        row = {"path": rel, "built_by": by, "ok": True}
        if not f.is_file():
            problems.append(f"{rel}: not in the tree")
            row["ok"] = False
        elif not pro.is_file():
            problems.append(f"{rel}: its `built_by` project {by} is not in the tree")
            row["ok"] = False
        else:
            if Path(rel).name not in _project_text(pro):
                problems.append(
                    f"{rel}: {by} no longer names it -- it is in the repository "
                    f"but nothing compiles it, so the shipped binary loses it "
                    f"silently")
                row["ok"] = False
            if pro not in reach:
                problems.append(
                    f"{rel}: {by} is not reachable from src/klayout.pro through "
                    f"SUBDIRS, so the project itself is outside the build graph")
                row["ok"] = False
        rows.append(row)
        print(("     OK  " if row["ok"] else "   FAIL  ") + f"{rel}  <- {by}")

    ref = a.upstream_ref or manifest.get("upstream_ref") or "upstream/master"
    added, modified, tests, why_not = _fork_delta_under_src(root, ref)
    drift_note = None
    if why_not:
        drift_note = why_not
        print(f"  SKIP drift checks: {why_not}")
    else:
        before = len(problems)
        declared = {s.get("path") for s in sources}
        # TRANSLATION UNITS ONLY. A .cc that is not in any SOURCES list is
        # silently not compiled -- that is the whole defect. A header is
        # different: qmake's HEADERS is an IDE/moc convenience with no effect on
        # what gets built, and a header that actually went missing breaks the
        # compile loudly. Demanding manifest entries for headers would buy
        # nothing and would make the manifest churn.
        undeclared = sorted(f for f in added
                            if f.endswith(".cc") and f not in declared)
        if undeclared:
            problems.append(
                "these sources are ADDED by the fork under src/ but are not in "
                "FORK_SOURCES.json, so nothing asserts they are compiled: "
                + ", ".join(undeclared))
        # A file the fork only patches is registered by upstream, but it still
        # has to be in the build for the patch to exist in the binary.
        dropped = sorted(f for f in modified
                         if f.endswith(".cc") and f not in declared
                         and not _named_by_any_project(root, f, reach))
        if dropped:
            problems.append(
                "the fork patches these sources, but no project inside the "
                "build graph names them any more, so our changes are not in the "
                "shipped binary: " + ", ".join(dropped))
        declared_tests = {t.get("test") for t in cpp_tests}
        unwired = sorted(set(tests) - declared_tests)
        if unwired:
            problems.append(
                "these C++ tests are added by the fork but are not in "
                "`cpp_tests`, so run_cpp_unit_tests.sh would never run them: "
                + ", ".join(unwired))
        if len(problems) == before:
            print(f"     OK  drift: {len(added)} added + {len(modified)} patched "
                  f"fork file(s) under src/, {len(tests)} fork TEST() -- all "
                  f"declared and all in the build graph")
        else:
            print("   FAIL  drift")

    if a.json:
        Path(a.json).write_text(json.dumps(
            {"rows": rows, "problems": problems, "drift_skipped": drift_note},
            indent=2))

    if problems:
        print("FAIL check_build_registration")
        for p in problems:
            print("  ! " + p)
        return FAIL
    print(f"PASS check_build_registration ({len(sources)} fork source(s) "
          f"compiled by a project inside the build graph"
          + ("" if drift_note else f", {len(cpp_tests)} C++ guard(s) wired") + ")")
    return PASS


if __name__ == "__main__":
    sys.exit(main())
