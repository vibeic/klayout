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
    assert d["gate"].kind == "bool" and d["gate"].bool_sym == "&"
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
