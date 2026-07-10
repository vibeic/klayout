"""
proof.py — self-contained FAIL->PASS proof for the SVRF-native DRC interpreter.
Run inside KLayout:

    klayout -b -r proof.py -rd root=<this svrf-drc dir>

Generates the geometric + connectivity test structures, runs the interpreter on
examples/demo.rule and examples/conn.rule, and asserts the expected verdicts.
Prints "SVRF-DRC PROOF: PASS" and exits 0 on success, else raises.
"""
import os
import sys

root = globals().get("root", os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, root)

import pya  # noqa: E402
from svrf_klayout.run_svrf_drc import Engine  # noqa: E402

BUILD = os.path.join(root, "build")
os.makedirs(BUILD, exist_ok=True)


def _gen(script, out):
    g = {"out": out, "__name__": "__gen__"}
    exec(open(os.path.join(root, "gen", script)).read(), g)


def _verdicts(deck_file, gds):
    text = open(os.path.join(root, "examples", deck_file)).read()
    eng = Engine(gds, text)
    return {r.name: (verdict, info) for verdict, r, info in eng.execute()}


def _check(got, expected):
    bad = []
    for name, exp in expected.items():
        v = got.get(name, ("MISSING", None))
        if v[0] != exp:
            bad.append(f"  {name}: expected {exp}, got {v}")
    return bad


def main():
    _gen("_gen_teststruct.py", os.path.join(BUILD, "test.gds"))
    _gen("_gen_conn.py", os.path.join(BUILD, "conn.gds"))

    geo = _verdicts("demo.rule", os.path.join(BUILD, "test.gds"))
    con = _verdicts("conn.rule", os.path.join(BUILD, "conn.gds"))

    bad = []
    bad += _check(geo, {
        "SPACE.M1.FAIL": "FAIL", "SPACE.M1.PASS": "PASS", "WIDTH.M1.PASS": "PASS",
        "AREA.M2.FAIL": "FAIL", "GATE.W.PASS": "PASS", "SPACE.M1M2.NET": "SKIP",
    })
    bad += _check(con, {"NOTCON.M1M2": "FAIL", "CONN.M1M2": "FAIL"})
    # connectivity must DISCRIMINATE: each net-aware rule flags exactly 1 pair
    if con.get("NOTCON.M1M2", (None, None))[1] != 1:
        bad.append(f"  NOTCON.M1M2 count: expected 1, got {con.get('NOTCON.M1M2')}")
    if con.get("CONN.M1M2", (None, None))[1] != 1:
        bad.append(f"  CONN.M1M2 count: expected 1, got {con.get('CONN.M1M2')}")

    for name, v in list(geo.items()) + list(con.items()):
        print(f"  {name:16s} -> {v}")
    if bad:
        raise SystemExit("SVRF-DRC PROOF: FAIL\n" + "\n".join(bad))
    print("SVRF-DRC PROOF: PASS")


main()
