"""
svrf_klayout.svrf_parse — read a Calibre/SVRF-format deck into interpreter-ready
rule + derivation objects. Pure Python (no pya), so it is unit-testable outside
KLayout.

This is NOT a transcoder: it emits no other language. It parses the deck into
structured objects whose modifiers are carried as native-check parameters
(metrics / ignore_angle / opposite / projection / connectivity). The KLayout
driver (`run_svrf_drc.py`) then executes each object DIRECTLY on KLayout's own
DRC engine via `pya.Region.*` — no intermediate `.drc` file, no separate tool.

Contains NO vendor data.
"""
from __future__ import annotations

import re
from dataclasses import dataclass, field
from typing import Optional

_LINE_COMMENT = re.compile(r'//[^\n]*')
_BLOCK_COMMENT = re.compile(r'/\*.*?\*/', re.DOTALL)

_LAYER_RE = re.compile(
    r'^\s*LAYER\s+([A-Za-z_][\w.$]*)\s+(\d+)(?:\s+(\d+))?\s*$',
    re.IGNORECASE | re.MULTILINE)

# a geometric measurement (rule) with an optional modifier tail
_MEAS_RE = re.compile(
    r'\b(INTERNAL|INT|EXTERNAL|EXT|ENCLOSURE|ENC|WIDTH|SPACE|AREA|NOTCH|DENSITY|ANTENNA)\b\s+'
    r'([A-Za-z_][\w.$]*)(?:\s+([A-Za-z_][\w.$]*))?\s*'
    r'(<=|<|==|>=|>)\s*([0-9]*\.?[0-9]+)\s*(.*)$',
    re.IGNORECASE)

# ABUT [singular] [rel] angle [ < angle ] — capture the UPPER angle bound
_ABUT_RE = re.compile(
    r'\bABUT\b[^\n]*?([0-9]+(?:\.[0-9]+)?)(?:\s*<\s*([0-9]+(?:\.[0-9]+)?))?',
    re.IGNORECASE)
# PROJECTING [rel] length — capture an optional projection-length bound
_PROJLEN_RE = re.compile(r'\bPROJECTING\b\s*(?:<=|<|>=|>)?\s*([0-9]*\.?[0-9]+)', re.IGNORECASE)

_BLOCK_OPEN = re.compile(r'^\s*([A-Za-z0-9_.\-/]+)\s*\{\s*$')
_ASSIGN_HEAD = re.compile(r'^\s*([A-Za-z_][\w.$]*)\s*=\s*(.+)$')
_COPY_RE = re.compile(r'\bCOPY\b\s+([A-Za-z_][\w.$]*)', re.IGNORECASE)
# CONNECT a b [BY via]  /  SCONNECT — the deck's connectivity stack (feeds net extraction)
_CONNECT_RE = re.compile(
    r'^\s*S?CONNECT\s+([A-Za-z_][\w.$]*)\s+([A-Za-z_][\w.$]*)(?:\s+BY\s+([A-Za-z_][\w.$]*))?',
    re.IGNORECASE | re.MULTILINE)

_BOOL_OPS = {"AND": "&", "OR": "|", "NOT": "-", "XOR": "^"}
_SIZE_OPS = {"SIZE", "GROW", "SHRINK", "OVERSIZE", "UNDERSIZE"}
_SELECT_OPS = {"INTERACT", "INSIDE", "OUTSIDE", "TOUCH", "CUT", "ENCLOSE"}


def strip_comments(text: str) -> str:
    return _LINE_COMMENT.sub('', _BLOCK_COMMENT.sub('', text))


def parse_layers(text: str) -> dict[str, tuple[int, int]]:
    out: dict[str, tuple[int, int]] = {}
    for m in _LAYER_RE.finditer(text):
        out[m.group(1)] = (int(m.group(2)), int(m.group(3) or 0))
    return out


# ── rule / derivation objects ───────────────────────────────────────────────
@dataclass
class Rule:
    name: str
    op: str                         # EXTERNAL | INTERNAL | ENCLOSURE | NOTCH | AREA | ...
    layer1: str
    layer2: Optional[str]
    cmp: str
    value: float
    # native-check parameters (consumed directly by KLayout's engine)
    metrics: str = "euclidian"      # euclidian | projection | square
    ignore_angle: Optional[float] = None   # ABUT angle -> pya ignore_angle
    min_projection: Optional[float] = None
    max_projection: Optional[float] = None
    opposite: bool = False          # OPPOSITE -> opposite_filter
    whole_edges: bool = False       # WHOLE -> whole_edges
    shielded: Optional[bool] = None # SHIELDED / TRANSPARENT
    region_out: bool = False        # REGION -> output polygons
    singular: bool = False
    connectivity: Optional[str] = None   # None | 'same' (CONNECTED) | 'different' (NOT CONNECTED)
    window: Optional[float] = None       # DENSITY WINDOW w (um)
    step: Optional[float] = None         # DENSITY STEP s (um)
    supported: bool = True
    reason: str = ""
    raw: str = ""
    notes: list = field(default_factory=list)


@dataclass
class Derivation:
    name: str
    kind: str      # bool|size|select|passthrough|empty|holes|rectangles|extents|
                   # metric_select|edge|unknown
    expr: str
    operands: list = field(default_factory=list)
    bool_sym: Optional[str] = None  # & | - ^
    size_op: Optional[str] = None   # SIZE | GROW | SHRINK
    value: Optional[float] = None
    select_op: Optional[str] = None
    metric: Optional[str] = None    # AREA | LENGTH | ANGLE | PERIMETER (for metric_select)
    bounds: tuple = None            # (min_or_None, max_or_None) for metric_select
    neq: Optional[float] = None     # `!= N` value (with_*(N, inverse=True))
    params: dict = None             # extra params (bbox width/height/aspect, expand dir, ...)
    edge_typed: bool = False        # result is edges, not polygons -> polygon DRC can't consume
    supported: bool = True
    reason: str = ""
    raw: str = ""


@dataclass
class Deck:
    layers: dict
    derivations: list
    rules: list
    connects: list = field(default_factory=list)   # (layer_a, layer_b, via_or_None)
    statements: list = field(default_factory=list)  # derivations + rules in SOURCE ORDER
                                                    # (one unified namespace: drawn / derived /
                                                    #  error layers, executed top-to-bottom)


_OPCANON = {"EXT": "EXTERNAL", "INT": "INTERNAL", "ENC": "ENCLOSURE", "WIDTH": "INTERNAL"}


def _parse_modifiers(rule: Rule, tail: str) -> None:
    up = tail.upper()
    if re.search(r'\bPROJECTING\b|\bPARA(?:LLEL)?\s+ONLY\b|\bPARALLEL\b', up):
        rule.metrics = "projection"
    if re.search(r'\bSQUARE\b', up):
        rule.metrics = "square"
    m = _ABUT_RE.search(tail)
    if m:
        rule.ignore_angle = float(m.group(2) or m.group(1))
    pl = _PROJLEN_RE.search(tail)
    if pl:
        rule.max_projection = float(pl.group(1))
    if re.search(r'\bOPPOSITE\b', up):
        rule.opposite = True
    if re.search(r'\bWHOLE\b', up):
        rule.whole_edges = True
    if re.search(r'\bSHIELDED\b', up):
        rule.shielded = True
    elif re.search(r'\bTRANSPARENT\b', up):
        rule.shielded = False
    if re.search(r'\bREGION\b', up):
        rule.region_out = True
    if re.search(r'\bSINGULAR\b', up):
        rule.singular = True
    if re.search(r'\bNOT\s+CONNECTED\b', up):
        rule.connectivity = "different"
    elif re.search(r'\bCONNECTED\b', up):
        rule.connectivity = "same"


def _make_rule(name: str, meas: re.Match) -> Rule:
    op = meas.group(1).upper()
    op = _OPCANON.get(op, op)
    r = Rule(name=name, op=op, layer1=meas.group(2), layer2=meas.group(3),
             cmp=meas.group(4), value=float(meas.group(5)), raw=meas.group(0).strip())
    _parse_modifiers(r, meas.group(6) or "")
    if op == "DENSITY":
        tail = meas.group(6) or ""
        w = re.search(r'\bWINDOW\s+(-?[0-9]*\.?[0-9]+)', tail, re.I)
        s = re.search(r'\bSTEP\s+(-?[0-9]*\.?[0-9]+)', tail, re.I)
        r.window = float(w.group(1)) if w else None
        r.step = float(s.group(1)) if s else r.window
    elif op == "ANTENNA":
        # antenna ratio is charge-accumulation, routed to the antenna checker
        # (OpenROAD/magic fork), not the geometric core
        r.supported = False
        r.reason = "ANTENNA (charge ratio) routed to antenna checker, not geometric core"
    return r


_UNARY_LAYER_OPS = {"HOLES": "holes", "RECTANGLE": "rectangles", "RECTANGLES": "rectangles",
                    "EXTENT": "extents", "EXTENTS": "extents", "MERGE": "merge",
                    "MERGED": "merge", "DRAWN": "passthrough", "COPY": "passthrough"}
# ops whose result is EDGES (a polygon-DRC rule cannot consume them -> dependents SKIP)
_EDGE_OPS = {"COIN", "COINCIDENT", "STRAIGHT", "CONVEX", "ACUTE", "SNAP", "SHADOW"}
_METRIC_OPS = {"AREA", "PERIMETER", "LENGTH", "ANGLE"}   # AREA/PERIMETER=Region, LENGTH/ANGLE=Edges
_KEYWORDS = (set(_BOOL_OPS) | _SIZE_OPS | _SELECT_OPS | set(_UNARY_LAYER_OPS) | _EDGE_OPS
             | _METRIC_OPS | {"BY", "WITH", "EMPTY", "ONLY", "NET", "AS", "TO", "OF", "IN",
                              "OPPOSITE", "PROJECTING", "REGION", "SINGULAR", "ABUT", "EXTENDED",
                              "EDGE", "EDGES", "EXPAND", "VERTEX", "ASPECT", "RATIO", "CENTERS",
                              "PATH", "INNER", "OUTER", "TOUCH", "BOTH", "STEP", "OVERUNDER",
                              "UNDEROVER"})


def _layer_toks(toks):
    return [t for t in toks if re.fullmatch(r'[A-Za-z_][\w.$]*', t) and t.upper() not in _KEYWORDS]


def _pure_boolean(rawtoks):
    """True iff the expr is ONLY layer names + AND/OR/NOT/XOR + parens (a pure
    boolean layer expression, any nesting) — evaluated by the Engine left-to-right."""
    has_op = False
    for t in rawtoks:
        u = t.upper()
        if t in ("(", ")"):
            continue
        if u in _BOOL_OPS:
            has_op = True; continue
        if re.fullmatch(r'[A-Za-z_][\w.$]*', t) and u not in _KEYWORDS:
            continue
        return False
    return has_op


def _rel_bounds(expr):
    mn = mx = None
    for rel, v in re.findall(r'(<=|<|>=|>|==)\s*(-?[0-9]*\.?[0-9]+)', expr):
        v = float(v)
        if rel in ("<", "<="):
            mx = v
        elif rel in (">", ">="):
            mn = v
        else:
            mn = mx = v
    return (mn, mx)


def _neq(expr):
    m = re.search(r'!=\s*(-?[0-9]*\.?[0-9]+)', expr)
    return float(m.group(1)) if m else None


def _nums(toks):
    return [float(x) for x in toks if re.fullmatch(r'-?[0-9]*\.?[0-9]+', x)]


def _parse_derivation(name: str, expr: str) -> Optional[Derivation]:
    """A layer-derivation assignment (produces a derived layer, not an error output)."""
    toks = expr.replace("(", " ").replace(")", " ").split()
    up = [t.upper() for t in toks]
    U = set(up)
    d = Derivation(name=name, kind="unknown", expr=expr, raw=expr.strip())
    if not toks:
        d.supported = False; d.reason = "empty rhs"; return d
    if len(toks) == 1 and up[0] == "EMPTY":
        d.kind = "empty"; return d
    # pure boolean expression (any mix of AND/OR/NOT/XOR + parens over plain layers)
    rawtoks = re.findall(r'\(|\)|[^\s()]+', expr)
    if _pure_boolean(rawtoks):
        d.kind = "bool_expr"
        d.operands = [t for t in rawtoks if t not in "()" and t.upper() not in _BOOL_OPS]
        return d
    lt = _layer_toks(toks)
    # COPY / DRAWN <layer> -> alias  (COPY empty -> empty)
    if up[0] in ("COPY", "DRAWN"):
        if "EMPTY" in U or not lt:
            d.kind = "empty"
        else:
            d.kind = "passthrough"; d.operands = [lt[0]]
        return d
    # A NET AREA RATIO B <cmp> N -> per-net area ratio (area(A on net)/area(B on net)).
    # Executed natively via KLayout LayoutToNetlist; the comparison expresses the
    # violation condition directly (e.g. `== 0` flags nets where A is absent while B
    # is present -> a "not connected" error).
    if "NET" in U and "RATIO" in U:
        d.kind = "net_ratio"; d.operands = lt[:2]
        m = re.search(r'(<=|>=|==|!=|<|>)\s*(-?[0-9]*\.?[0-9]+)', expr)
        d.params = {"cmp": m.group(1) if m else "==", "thr": float(m.group(2)) if m else 0.0}
        if len(d.operands) < 2:
            d.supported = False; d.reason = "NET AREA RATIO needs two layers"
        return d
    # SIZE / GROW / SHRINK (isotropic polygon bias)
    for t in up:
        if t in _SIZE_OPS:
            nn = _nums(toks)
            d.kind = "size"; d.size_op = t
            d.operands = [lt[0]] if lt else []
            d.value = nn[0] if nn else None
            if t in ("SHRINK", "UNDERSIZE") and d.value is not None:
                d.value = -abs(d.value)
            if not d.operands:
                d.supported = False; d.reason = "size without a layer"
            return d
    # VERTEX <layer> op N -> Region (select polygons by vertex count)
    if "VERTEX" in U:
        d.kind = "vertex"; d.operands = [lt[0]] if lt else []; d.bounds = _rel_bounds(expr)
        if not d.operands:
            d.supported = False; d.reason = "vertex without a layer"
        return d
    # RECTANGLE <layer> [ <wrel> W BY <hrel> H ] [ ASPECT <rel> N ] -> Region
    if "RECTANGLE" in U or "RECTANGLES" in U:
        d.kind = "rectangles"; d.operands = [lt[0]] if lt else []; d.params = {}
        if re.search(r'\bBY\b', expr, re.I):
            a, b = re.split(r'\bBY\b', expr, maxsplit=1, flags=re.I)
            d.params["w"] = _rel_bounds(a); d.params["h"] = _rel_bounds(b)
        if "ASPECT" in U:
            d.params["aspect"] = _rel_bounds(re.split(r'\bASPECT\b', expr, 1, re.I)[1])
        if not d.operands:
            d.supported = False; d.reason = "rectangle without a layer"
        return d
    # HOLES / EXTENTS / MERGE (unary region ops)
    for t in up:
        if t in _UNARY_LAYER_OPS and _UNARY_LAYER_OPS[t] not in ("passthrough",):
            if lt:
                d.kind = _UNARY_LAYER_OPS[t]; d.operands = [lt[0]]; return d
    # WITH EDGE: X WITH EDGE Y -> Region (polygons of X sharing an edge with Y)
    if "WITH" in U and ("EDGE" in U or "EDGES" in U):
        d.kind = "with_edge"; d.operands = lt[:2]
        if len(lt) < 2:
            d.supported = False; d.reason = "WITH EDGE needs two layers"
        return d
    # EXPAND [EDGE] X [dir] BY N -> thin polygon strip from expanded edges (Region).
    # MUST precede the EDGE catch: "EXPAND EDGE" contains 'EDGE' yet yields polygons.
    if "EXPAND" in U:
        nn = _nums(toks)
        d.kind = "expand"; d.operands = lt[:1]; d.value = nn[0] if nn else None
        d.params = {"inside": "INSIDE" in U, "outside": "OUTSIDE" in U, "both": "BOTH" in U}
        if not d.operands:
            d.supported = False; d.reason = "expand without a layer"
        return d
    # Edge-typed derivations (result is a pya.Edges layer). Capture the modifier so the
    # Engine builds the geometrically-correct edge set (select_op + params):
    #   EDGE / INNER EDGE / OUTER EDGE      -> region.edges / holes.edges / hulls.edges
    #   A INSIDE EDGE B / A OUTSIDE EDGE B  -> A.edges.inside_part / outside_part (B)
    #   A COINCIDENT [INSIDE|OUTSIDE] EDGE B-> A.edges & B.edges (+ direction split)
    #   A TOUCH EDGE B                      -> A.edges.interacting(B)
    #   STRAIGHT/CONVEX/ACUTE/SNAP/SHADOW   -> functional edges() (rare DFM)
    if ("EDGE" in U or "EDGES" in U or "COINCIDENT" in U or "COIN" in U or (U & _EDGE_OPS)):
        coin = ("COINCIDENT" in U) or ("COIN" in U)
        inside, outside, touch = "INSIDE" in U, "OUTSIDE" in U, "TOUCH" in U
        binary = coin or inside or outside or touch
        dfm = next((t for t in up if t in _EDGE_OPS and t not in ("COIN", "COINCIDENT")), None)
        d.kind = "edge"; d.edge_typed = True
        d.select_op = ("COINCIDENT" if coin else "INSIDE" if inside else "OUTSIDE" if outside
                       else "TOUCH" if touch else dfm if dfm else "EDGE")
        d.params = {"coin": coin, "inside": inside, "outside": outside, "touch": touch,
                    "inner": "INNER" in U, "outer": "OUTER" in U}
        d.operands = lt[:2] if (binary and len(lt) >= 2) else lt[:1]
        if not d.operands:
            d.supported = False; d.edge_typed = False; d.reason = "edge op without a layer"
        return d
    # region-returning selects (INTERACT / INSIDE / OUTSIDE / CUT / TOUCH / ENCLOSE)
    for t in up:
        if t in _SELECT_OPS:
            d.kind = "select"; d.select_op = t; d.operands = lt[:2]
            if not lt:
                d.supported = False; d.reason = "select without a layer"
            return d
    # metric selection: AREA/PERIMETER -> Region ; LENGTH/ANGLE -> Edges
    for t in up:
        if t in _METRIC_OPS:
            d.kind = "metric_select"; d.metric = t
            d.operands = [lt[0]] if lt else []
            d.bounds = _rel_bounds(expr); d.neq = _neq(expr)
            d.edge_typed = t in ("LENGTH", "ANGLE")
            if not d.operands or (d.bounds == (None, None) and d.neq is None):
                d.supported = False; d.reason = f"{t} select without layer/bounds"
            return d
    # bare alias / passthrough
    if len(toks) == 1 and re.fullmatch(r'[A-Za-z_][\w.$]*', toks[0]):
        d.kind = "passthrough"; d.operands = [toks[0]]; return d
    d.supported = False; d.reason = "unrecognised derivation expression"
    return d


def parse_deck(text: str) -> Deck:
    """Parse into (layers, derivations, rules). Rules come from named blocks
    `NAME { OP ... }` / `NAME { COPY errlayer }` and from `name = OP ...`
    assignments; derivations from `name = <layer expression>` assignments."""
    text = strip_comments(text)
    layers = parse_layers(text)
    connects = [(m.group(1), m.group(2), m.group(3)) for m in _CONNECT_RE.finditer(text)]
    derivations: list[Derivation] = []
    rules: list[Rule] = []
    statements: list = []
    lines = text.splitlines()
    i, n = 0, len(lines)
    while i < n:
        line = lines[i]
        bo = _BLOCK_OPEN.match(line)
        if bo:
            name = bo.group(1)
            depth, j = 1, i + 1
            body = []
            while j < n and depth > 0:
                depth += lines[j].count("{") - lines[j].count("}")
                if depth > 0:
                    body.append(lines[j])
                j += 1
            joined = "\n".join(body)
            mm = _MEAS_RE.search(joined)
            if mm:
                r = _make_rule(name, mm)
                rules.append(r); statements.append(r)
            else:
                cp = _COPY_RE.search(joined)
                if cp:                       # NAME { COPY errlayer } -> output an error layer
                    r = Rule(name=name, op="COPY", layer1=cp.group(1), layer2=None,
                             cmp="", value=0.0, raw=joined.strip())
                    rules.append(r); statements.append(r)
            i = j
            continue
        ah = _ASSIGN_HEAD.match(line)
        if ah and not _LAYER_RE.match(line):
            rhs = ah.group(2)
            # An assignment-form RULE leads with its measurement op (`x = EXTERNAL ...`);
            # a derivation leads with a layer (`x = a NET AREA RATIO b ...`). Anchor the
            # match so an op embedded mid-RHS (e.g. the AREA in NET AREA RATIO) is not
            # mistaken for a rule.
            mm = _MEAS_RE.match(rhs.strip())
            if mm:
                r = _make_rule(ah.group(1), mm)
                rules.append(r); statements.append(r)
            else:
                d = _parse_derivation(ah.group(1), rhs)
                if d:
                    derivations.append(d); statements.append(d)
        i += 1
    return Deck(layers, derivations, rules, connects, statements)
