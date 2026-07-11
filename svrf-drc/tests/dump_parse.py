#!/usr/bin/env python3
"""Canonical parse-dump of a SVRF deck via the reference Python parser.

Emits a deterministic, line-oriented dump used as the GOLDEN to cross-check the
native C++ parser (dbSVRFDeck) byte-for-byte. Format (stable):

  LAYER <name> <g>/<d>[,<g>/<d>...]                       (sorted by name)
  CONNECT <a> <b> <via|->                                 (source order)
  STMT <i> RULE  <name>|<op>|<l1>|<l2|->|<cmp>|<val>|m=<metrics>|ia=<ignore_angle|->|opp=<0/1>|whole=<0/1>|conn=<none/same/different>|win=<w|->|step=<s|->|sup=<0/1>
  STMT <i> DERIV <name>|<kind>|ops=<o1,o2,..>|bool=<sym|->|sop=<size_op|->|val=<v|->|sel=<select_op|->|metric=<m|->|bounds=<lo,hi>|neq=<n|->|edge=<0/1>|sup=<0/1>
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from svrf_klayout.svrf_parse import parse_deck, Rule, Derivation  # noqa: E402


def _f(x):
    if x is None:
        return "-"
    if isinstance(x, float):
        # match a stable representation: integers without trailing .0
        return str(int(x)) if x == int(x) else repr(x)
    return str(x)


def _bounds(b):
    if not b:
        return "-,-"
    lo, hi = b
    return f"{_f(lo)},{_f(hi)}"


def dump(deck) -> str:
    out = []
    for name in sorted(deck.layers):
        purposes = ",".join(f"{g}/{d}" for g, d in deck.layers[name])
        out.append(f"LAYER {name} {purposes}")
    for a, b, via in deck.connects:
        out.append(f"CONNECT {a} {b} {via or '-'}")
    for i, st in enumerate(deck.statements):
        if isinstance(st, Rule):
            out.append(
                f"STMT {i} RULE {st.name}|{st.op}|{st.layer1}|{_f(st.layer2)}|"
                f"{st.cmp}|{_f(st.value)}|m={st.metrics}|ia={_f(st.ignore_angle)}|"
                f"opp={int(st.opposite)}|whole={int(st.whole_edges)}|"
                f"conn={st.connectivity or 'none'}|win={_f(st.window)}|"
                f"step={_f(st.step)}|sup={int(st.supported)}")
        elif isinstance(st, Derivation):
            ops = ",".join(st.operands or [])
            out.append(
                f"STMT {i} DERIV {st.name}|{st.kind}|ops={ops}|"
                f"bool={st.bool_sym or '-'}|sop={st.size_op or '-'}|"
                f"val={_f(st.value)}|sel={st.select_op or '-'}|"
                f"metric={st.metric or '-'}|bounds={_bounds(st.bounds)}|"
                f"neq={_f(st.neq)}|edge={int(st.edge_typed)}|sup={int(st.supported)}")
    return "\n".join(out) + "\n"


if __name__ == "__main__":
    text = open(sys.argv[1], encoding="utf-8", errors="replace").read()
    sys.stdout.write(dump(parse_deck(text)))
