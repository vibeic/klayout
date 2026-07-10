"""Adversarial test structures for the first-principles edge-pair checker.

Each case encodes the PHYSICALLY-EXPECTED outcome of the standard geometry
(parallel plates, acute vs right corner, euclidian vs projection, kissing
corners, the angle_limit knob). Passing them proves the geometry is computed
correctly BY CONSTRUCTION — i.e. `golden` is a regression test, not an oracle.
"""
import math
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from svrf_klayout.edge_pair_check import (  # noqa: E402
    space_check, width_check, edges_of, _enclosed_angle, _seg_seg_dist,
)


def rect(x0, y0, x1, y1):
    return [(x0, y0), (x1, y0), (x1, y1), (x0, y1)]


# ── 1. parallel plates: the primary spacing rule ────────────────────────────

def test_parallel_plates_violation_and_clear():
    A = rect(0, 0, 1, 1)
    B_close = rect(0, 1.10, 1, 2.10)     # gap 0.10
    B_far = rect(0, 1.15, 1, 2.15)       # gap 0.15
    v = space_check([A], [B_close], value=0.12)
    assert len(v) == 1 and v[0].distance == pytest.approx(0.10, abs=1e-6)
    assert space_check([A], [B_far], value=0.12) == []


def test_tie_at_exactly_the_rule_value_is_pass():
    A = rect(0, 0, 1, 1)
    B_tie = rect(0, 1.12, 1, 2.12)       # gap exactly 0.12
    B_under = rect(0, 1.119, 1, 2.119)   # gap 0.119 (< 0.12)
    assert space_check([A], [B_tie], value=0.12) == []          # 0.12 < 0.12 is false
    assert len(space_check([A], [B_under], value=0.12)) == 1


# ── 2. euclidian vs projection: the metric matters at a diagonal corner ──────

def test_euclidian_flags_corner_projection_does_not():
    # two squares offset so the ONLY close approach is a corner-to-corner diagonal
    A = rect(0, 0, 1, 1)
    B = rect(1.07, 1.07, 2.07, 2.07)     # corner (1,1)->(1.07,1.07): diag ~0.099
    diag = math.hypot(0.07, 0.07)
    eu = space_check([A], [B], value=0.12, metric="euclidian")
    assert len(eu) >= 1
    assert min(x.distance for x in eu) == pytest.approx(diag, abs=1e-6)
    # projection: no projected overlap at the diagonal -> nothing measured
    assert space_check([A], [B], value=0.12, metric="projection") == []


# ── 3. the angle_limit gate (ABUT angle): right vs acute corner ─────────────

def test_right_angle_corner_not_flagged_acute_is():
    # width of a thin acute wedge vs a right corner, both far below the rule value
    # right-angle corner: a big square, corner interior angle 90 -> skipped at limit 90
    big = rect(0, 0, 5, 5)
    assert width_check([big], value=0.30) == []      # width 5 >> 0.30, corners 90 skipped

    # acute wedge (isoceles triangle, apex interior angle ~28.1 deg), thin tip
    wedge = [(0.0, 0.0), (2.0, 0.25), (2.0, -0.25)]
    vs = width_check([wedge], value=0.30)
    assert len(vs) >= 1                               # the acute apex converges -> flagged
    assert any(x.angle < 90 for x in vs)


def test_angle_limit_knob_changes_the_verdict():
    # a 60-degree convex corner: flagged at angle_limit=90, cleared at 45
    # equilateral-ish wedge, apex interior angle 60 deg
    h = math.tan(math.radians(30)) * 1.0             # half-width at base x=1 for 60deg apex
    wedge = [(0.0, 0.0), (1.0, h), (1.0, -h)]
    assert any(x.angle == pytest.approx(60, abs=1.0)
               for x in width_check([wedge], value=0.05, angle_limit=90))
    assert width_check([wedge], value=0.05, angle_limit=45) == []   # 60 >= 45 -> skipped


# ── 4. kissing corners (zero-distance single-point touch) ───────────────────

def test_kissing_corners_default_flags_waivable():
    A = rect(0, 0, 1, 1)
    B = rect(1, 1, 2, 2)                 # touch at the single point (1,1)
    default = space_check([A], [B], value=0.05)
    assert any(x.single_point_touch and x.distance == 0.0 for x in default)
    assert space_check([A], [B], value=0.05, without_touching_corners=True) == []


# ── 5. geometry primitives sanity (the standard, computed directly) ─────────

def test_enclosed_angle_definitions():
    # antiparallel facing edges (parallel plates) -> 0 deg
    e_bottom = edges_of(rect(0, 1.1, 1, 2.1))[0]     # dir +x
    e_top = edges_of(rect(0, 0, 1, 1))[2]            # top edge dir -x (facing)
    assert _enclosed_angle(e_bottom, e_top) == pytest.approx(0, abs=1e-6)
    # a right-angle corner of a square -> 90 deg
    es = edges_of(rect(0, 0, 1, 1))
    assert _enclosed_angle(es[0], es[1]) == pytest.approx(90, abs=1e-6)


def test_seg_seg_distance_matches_hand_calc():
    assert _seg_seg_dist((0, 0), (1, 0), (0, 0.3), (1, 0.3)) == pytest.approx(0.3)
    assert _seg_seg_dist((0, 0), (1, 0), (2, 0), (3, 0)) == pytest.approx(1.0)  # collinear gap


# ── 6. honest residual: unimplemented metric is refused, not guessed ────────

def test_square_metric_refused_not_silently_wrong():
    with pytest.raises(NotImplementedError):
        space_check([rect(0, 0, 1, 1)], [rect(0, 1.1, 1, 2.1)],
                    value=0.12, metric="square")
