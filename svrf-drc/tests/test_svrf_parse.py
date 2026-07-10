"""Tests for the SVRF deck parser (pure Python, synthetic decks only)."""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from svrf_klayout.svrf_parse import parse_deck, parse_layers  # noqa: E402

DECK = """
// synthetic SVRF-format deck (no vendor data)
LAYER metal1 34 0
LAYER metal2 36 0
LAYER via1   35 0
LAYER poly   10 0
LAYER nact   12 0

gate  = poly AND nact
bigm1 = metal1 SIZE 0.10
sel1  = metal1 INTERACT via1

a metal1-spacing rule {
  EXTERNAL metal1 < 0.14 ABUT < 90 REGION
}
M1a metal2-spacing rule {
  EXT metal1 metal2 < 0.16 OPPOSITE PARA ONLY REGION
}
M1.W.1 {
  INTERNAL metal1 < 0.13 ABUT > 0 < 90 SINGULAR WHOLE REGION
}
V1.EN.1 {
  ENCLOSURE via1 metal1 < 0.03
}
M1.A.1 {
  AREA metal1 < 0.05
}
WELL.NET.1 = EXTERNAL metal1 metal2 < 0.30 NOT CONNECTED REGION
SAME.NET.1 = EXTERNAL metal1 metal2 < 0.20 CONNECTED REGION
"""


def _by(deck, kind):
    return {x.name: x for x in getattr(deck, kind)}


def test_layers():
    lm = parse_layers(DECK)
    assert lm["metal1"] == (34, 0) and lm["via1"] == (35, 0)


def test_derivations_bool_size_select():
    d = _by(parse_deck(DECK), "derivations")
    assert d["gate"].kind == "bool_expr" and "poly" in d["gate"].operands
    assert d["bigm1"].kind == "size" and d["bigm1"].value == 0.10
    assert d["sel1"].kind == "select" and d["sel1"].select_op == "INTERACT"


def test_external_abut_maps_to_ignore_angle():
    r = _by(parse_deck(DECK), "rules")
    assert r["a metal1-spacing rule"].op == "EXTERNAL" and r["a metal1-spacing rule"].ignore_angle == 90.0
    assert r["a metal1-spacing rule"].region_out is True and r["a metal1-spacing rule"].metrics == "euclidian"


def test_projection_opposite_and_whole():
    r = _by(parse_deck(DECK), "rules")
    assert r["M1a metal2-spacing rule"].layer2 == "metal2"
    assert r["M1a metal2-spacing rule"].metrics == "projection"     # PARA ONLY -> projection
    assert r["M1a metal2-spacing rule"].opposite is True
    assert r["M1.W.1"].whole_edges is True
    assert r["M1.W.1"].singular is True
    assert r["M1.W.1"].ignore_angle == 90.0          # ABUT > 0 < 90 -> upper bound 90


def test_area_and_enclosure():
    r = _by(parse_deck(DECK), "rules")
    assert r["V1.EN.1"].op == "ENCLOSURE" and r["V1.EN.1"].layer2 == "metal1"
    assert r["M1.A.1"].op == "AREA" and r["M1.A.1"].value == 0.05


def test_connectivity_modes():
    r = _by(parse_deck(DECK), "rules")
    assert r["WELL.NET.1"].connectivity == "different"   # NOT CONNECTED
    assert r["SAME.NET.1"].connectivity == "same"        # CONNECTED


# ── edge pipeline / density / net-ratio classification (edges enhancement) ────
EDGE_DECK = """
LAYER a 11 0
LAYER b 12 0
me    = a EDGE
ain   = a INSIDE EDGE b
aout  = a OUTSIDE EDGE b
cae   = a COINCIDENT EDGE b
cin   = a COINCIDENT INSIDE EDGE b
longs = me LENGTH > 1.0
angs  = me ANGLE == 90
strip = a EXPAND EDGE OUTSIDE BY 0.1
nc    = a NET AREA RATIO b == 0
DEN.1 {
  DENSITY a < 0.2 WINDOW 100.0 STEP 50.0
}
"""


def test_edge_modifiers_are_edge_typed():
    d = _by(parse_deck(EDGE_DECK), "derivations")
    for name in ("me", "ain", "aout", "cae", "cin", "longs", "angs"):
        assert d[name].edge_typed is True, name
    assert d["me"].select_op == "EDGE"
    assert d["ain"].select_op == "INSIDE" and d["ain"].operands == ["a", "b"]
    assert d["aout"].select_op == "OUTSIDE"
    assert d["cae"].select_op == "COINCIDENT"
    assert d["cin"].select_op == "COINCIDENT" and d["cin"].params["inside"] is True


def test_length_angle_metric_select():
    d = _by(parse_deck(EDGE_DECK), "derivations")
    assert d["longs"].kind == "metric_select" and d["longs"].metric == "LENGTH"
    assert d["longs"].bounds == (1.0, None)
    assert d["angs"].kind == "metric_select" and d["angs"].metric == "ANGLE"
    assert d["angs"].bounds == (90.0, 90.0)   # exact angle


def test_expand_edge_is_region_not_edge():
    # EXPAND EDGE contains 'EDGE' but yields a polygon strip -> must NOT be edge-typed
    d = _by(parse_deck(EDGE_DECK), "derivations")
    assert d["strip"].kind == "expand" and d["strip"].edge_typed is False
    assert d["strip"].params["outside"] is True and d["strip"].value == 0.1


def test_net_area_ratio_is_derivation_not_area_rule():
    deck = parse_deck(EDGE_DECK)
    d = _by(deck, "derivations")
    # the embedded AREA token must NOT turn this into an AREA rule
    assert "nc" not in _by(deck, "rules")
    assert d["nc"].kind == "net_ratio" and d["nc"].operands == ["a", "b"]
    assert d["nc"].params == {"cmp": "==", "thr": 0.0}


def test_density_window_step_parsed():
    r = _by(parse_deck(EDGE_DECK), "rules")
    assert r["DEN.1"].op == "DENSITY" and r["DEN.1"].supported is True
    assert r["DEN.1"].window == 100.0 and r["DEN.1"].step == 50.0
