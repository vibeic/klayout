"""
svrf2klayout.edge_pair_check — a FROM-FIRST-PRINCIPLES geometric DRC edge-pair
spacing / width checker.

Why this exists
---------------
The angle-conditioned spacing measurement (SVRF `EXTERNAL ... ABUT < angle`,
`INTERNAL`, projection/parallel/opposite modifiers) is NOT a tool-private
definition — it is the STANDARD polygon edge-pair measurement that every DRC
engine implements, with a published geometry:

  * error is generated for an edge pair whose measured-side half-planes face each
    other ("back to back") and whose distance is below the rule value;
  * the distance uses one of a small NAMED metric set — euclidian (default),
    square, projection;
  * an `angle_limit` (SVRF ABUT angle; default 90 deg) gates WHICH edge pairs are
    measured: "for edges having an angle >= angle_limit, no check is performed"
    — the reason it exists is that two edges meeting at a corner always converge
    to zero distance at the vertex, so without the angle gate every corner would
    be flagged. The gate turns that into a deliberate ACUTE-CORNER check.

Because the geometry is a public standard, the answer is determined by the
geometry, not by any vendor tool. This module computes it directly. A reference
run then serves as a REGRESSION test (does the implementation have a bug?), not
as a semantic oracle (what does the rule mean?).

Scope of THIS module (v1): the deterministic geometric core — euclidian &
projection metrics, the angle_limit gate, width (INTERNAL) and space (EXTERNAL,
one- and two-layer) checks, and the `without_touching_corners` waiver. The
genuinely convention-defined residual is NOT silently guessed — it is surfaced:
  * square (L-inf) metric, `shielded` vs `transparent`, rectangle/opposite error
    filtering  -> not yet implemented (raise / flagged);
  * connectivity (CONNECTED / NOT CONNECTED) -> needs net extraction (separate);
  * partial-facing over a sub-span (whole_edges) -> midpoint-facing approximation.

Contains NO vendor data. Pure geometry, pure Python, no third-party deps.
"""
from __future__ import annotations

import math
from dataclasses import dataclass
from typing import Optional

# database grid: coordinates are snapped to this before comparison, so an
# "exactly at the rule value" tie resolves deterministically (matches DB-unit
# snapping in real tools). 1 nm default, expressed in micron.
GRID_UM = 0.001


# ── vector helpers (2-tuples in micron) ─────────────────────────────────────
def _sub(a, b): return (a[0] - b[0], a[1] - b[1])
def _dot(a, b): return a[0] * b[0] + a[1] * b[1]
def _cross(a, b): return a[0] * b[1] - a[1] * b[0]
def _norm(a): return math.hypot(a[0], a[1])
def _neg(a): return (-a[0], -a[1])
def _snap(p): return (round(p[0] / GRID_UM) * GRID_UM, round(p[1] / GRID_UM) * GRID_UM)


def _angle_between(u, v) -> float:
    """Unsigned angle between two direction vectors, in degrees [0, 180]."""
    nu, nv = _norm(u), _norm(v)
    if nu == 0 or nv == 0:
        return 0.0
    c = max(-1.0, min(1.0, _dot(u, v) / (nu * nv)))
    return math.degrees(math.acos(c))


def _pt_seg_dist(p, a, b) -> float:
    """Euclidean distance from point p to segment ab."""
    ab = _sub(b, a)
    L2 = _dot(ab, ab)
    if L2 == 0:
        return _norm(_sub(p, a))
    t = max(0.0, min(1.0, _dot(_sub(p, a), ab) / L2))
    proj = (a[0] + t * ab[0], a[1] + t * ab[1])
    return _norm(_sub(p, proj))


def _seg_seg_intersect(p1, p2, p3, p4) -> bool:
    d1 = _cross(_sub(p4, p3), _sub(p1, p3))
    d2 = _cross(_sub(p4, p3), _sub(p2, p3))
    d3 = _cross(_sub(p2, p1), _sub(p3, p1))
    d4 = _cross(_sub(p2, p1), _sub(p4, p1))
    if ((d1 > 0) != (d2 > 0)) and ((d3 > 0) != (d4 > 0)):
        return True
    return False


def _seg_seg_dist(p1, p2, p3, p4) -> float:
    """Minimum Euclidean distance between segment p1p2 and segment p3p4."""
    if _seg_seg_intersect(p1, p2, p3, p4):
        return 0.0
    return min(
        _pt_seg_dist(p1, p3, p4), _pt_seg_dist(p2, p3, p4),
        _pt_seg_dist(p3, p1, p2), _pt_seg_dist(p4, p1, p2),
    )


# ── edge model ──────────────────────────────────────────────────────────────
@dataclass(frozen=True)
class Edge:
    p1: tuple            # start (µm), polygon traversed CCW -> interior on LEFT
    p2: tuple            # end

    @property
    def dir(self):
        return _sub(self.p2, self.p1)

    @property
    def mid(self):
        return ((self.p1[0] + self.p2[0]) / 2, (self.p1[1] + self.p2[1]) / 2)

    @property
    def outward_normal(self):
        # CCW polygon: interior on left of dir; outward (empty space) = right = (dy,-dx)
        dx, dy = self.dir
        return (dy, -dx)

    @property
    def inward_normal(self):
        dx, dy = self.dir
        return (-dy, dx)


def _signed_area(poly) -> float:
    s = 0.0
    n = len(poly)
    for i in range(n):
        x1, y1 = poly[i]
        x2, y2 = poly[(i + 1) % n]
        s += x1 * y2 - x2 * y1
    return s / 2.0


def edges_of(poly) -> list[Edge]:
    """Directed edges of a simple polygon, normalised to CCW (interior on left)."""
    pts = [_snap(p) for p in poly]
    if _signed_area(pts) < 0:            # CW -> reverse to CCW
        pts = list(reversed(pts))
    n = len(pts)
    return [Edge(pts[i], pts[(i + 1) % n]) for i in range(n)]


# ── the measurement ─────────────────────────────────────────────────────────
@dataclass
class Violation:
    e1: Edge
    e2: Edge
    distance: float
    angle: float
    single_point_touch: bool


def _shared_vertex(e1: Edge, e2: Edge) -> Optional[tuple]:
    for a in (e1.p1, e1.p2):
        for b in (e2.p1, e2.p2):
            if a == b:
                return a
    return None


def _enclosed_angle(e1: Edge, e2: Edge) -> float:
    """Angle of the wedge of the measured medium between the two edges (degrees).

    * abutting (share a vertex): the interior corner angle — the angle between
      the two rays pointing AWAY from the shared vertex along each edge. A right
      corner = 90, an acute spike < 90, a shallow bend > 90.
    * non-abutting facing edges: angle between e1's direction and e2's REVERSED
      direction, so antiparallel facing edges (parallel plates) measure 0.
    """
    v = _shared_vertex(e1, e2)
    if v is not None:
        ray1 = e1.dir if v == e1.p1 else _neg(e1.dir)
        ray2 = e2.dir if v == e2.p1 else _neg(e2.dir)
        return _angle_between(ray1, ray2)
    return _angle_between(e1.dir, _neg(e2.dir))


def _faces(e_from: Edge, e_to: Edge, side: str, tol: float = 1e-9) -> bool:
    """Is e_to on the measured side (outward for space / inward for width) of e_from?"""
    n = e_from.outward_normal if side == "space" else e_from.inward_normal
    return _dot(_sub(e_to.mid, e_from.mid), n) > tol


def _projection_distance(e1: Edge, e2: Edge) -> Optional[float]:
    """Perpendicular distance over the region where the two edges' projections
    overlap. Returns None when there is no projected overlap (corner-to-corner)."""
    u = e1.dir
    Lu = _norm(u)
    if Lu == 0:
        return None
    ux = (u[0] / Lu, u[1] / Lu)
    # parametrize e1 as [0, Lu]; project e2 endpoints onto e1's line
    t3 = _dot(_sub(e2.p1, e1.p1), ux)
    t4 = _dot(_sub(e2.p2, e1.p1), ux)
    lo, hi = max(0.0, min(t3, t4)), min(Lu, max(t3, t4))
    if hi - lo <= GRID_UM:               # no real projected overlap
        return None
    # perpendicular distance from e2's line, evaluated at the overlap endpoints
    def perp(t):
        foot = (e1.p1[0] + t * ux[0], e1.p1[1] + t * ux[1])
        return _pt_seg_dist(foot, e2.p1, e2.p2)
    return min(perp(lo), perp(hi))


def _pair_check(e1: Edge, e2: Edge, value: float, side: str, metric: str,
                angle_limit: float, without_touching_corners: bool) -> Optional[Violation]:
    if not (_faces(e1, e2, side) and _faces(e2, e1, side)):
        return None
    # "kissing corner" degenerate: for a SPACE check, two different shapes touching
    # at a single point are a zero-distance violation independent of the wedge angle
    # (documented default: report; waivable via without_touching_corners). For a
    # WIDTH check a shared vertex is a normal corner -> governed by the angle gate.
    if side == "space" and _shared_vertex(e1, e2) is not None:
        if _seg_seg_dist(e1.p1, e1.p2, e2.p1, e2.p2) <= 1e-12:
            if without_touching_corners:
                return None
            return Violation(e1, e2, 0.0, _enclosed_angle(e1, e2), True)
    ang = _enclosed_angle(e1, e2)
    if ang >= angle_limit - 1e-9:        # angle gate (ABUT): >= limit -> no check
        return None
    if metric == "euclidian":
        dist = _seg_seg_dist(e1.p1, e1.p2, e2.p1, e2.p2)
    elif metric == "projection":
        dist = _projection_distance(e1, e2)
        if dist is None:
            return None
    else:
        raise NotImplementedError(f"metric {metric!r} not implemented in v1 "
                                  "(square / shielded are convention-defined residual)")
    if dist >= value - 1e-9:             # not below the rule value -> pass
        return None
    single_pt = (dist == 0.0 and _shared_vertex(e1, e2) is not None)
    if single_pt and without_touching_corners:
        return None
    return Violation(e1, e2, dist, ang, single_pt)


def space_check(layerA: list, layerB: Optional[list] = None, *, value: float,
                metric: str = "euclidian", angle_limit: float = 90.0,
                without_touching_corners: bool = False) -> list[Violation]:
    """EXTERNAL / SPACE: minimum empty-space distance between material edges.

    layerA / layerB are lists of polygons (each a list of (x,y) in µm). One-layer
    (layerB=None) = SPACE within a layer; two-layer = EXTERNAL between layers.
    """
    edgesA = [e for poly in layerA for e in edges_of(poly)]
    if layerB is None:
        edgesB, same = edgesA, True
    else:
        edgesB, same = [e for poly in layerB for e in edges_of(poly)], False
    out: list[Violation] = []
    for i, e1 in enumerate(edgesA):
        for j, e2 in enumerate(edgesB):
            if same and j <= i:
                continue
            v = _pair_check(e1, e2, value, "space", metric, angle_limit,
                            without_touching_corners)
            if v:
                out.append(v)
    return out


def width_check(layer: list, *, value: float, metric: str = "euclidian",
                angle_limit: float = 90.0) -> list[Violation]:
    """INTERNAL / WIDTH: minimum material thickness. Acute convex corners are
    reported only when their interior angle < angle_limit (default 90)."""
    out: list[Violation] = []
    for poly in layer:
        edges = edges_of(poly)
        n = len(edges)
        for i in range(n):
            for j in range(i + 1, n):
                v = _pair_check(edges[i], edges[j], value, "width", metric,
                                angle_limit, without_touching_corners=False)
                if v:
                    out.append(v)
    return out
