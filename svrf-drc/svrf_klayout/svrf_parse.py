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
    supported: bool = True
    reason: str = ""
    raw: str = ""
    notes: list = field(default_factory=list)


@dataclass
class Derivation:
    name: str
    kind: str                       # bool | size | select | copy | passthrough | unknown
    expr: str
    operands: list = field(default_factory=list)
    bool_sym: Optional[str] = None  # & | - ^
    size_op: Optional[str] = None   # SIZE | GROW | SHRINK
    value: Optional[float] = None
    select_op: Optional[str] = None
    supported: bool = True
    reason: str = ""
    raw: str = ""


@dataclass
class Deck:
    layers: dict
    derivations: list
    rules: list
    connects: list = field(default_factory=list)   # (layer_a, layer_b, via_or_None)


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
    if op in ("DENSITY", "ANTENNA"):
        r.supported = False
        r.reason = f"{op} (windowed/antenna) not in geometric core"
    return r


def _parse_derivation(name: str, expr: str) -> Optional[Derivation]:
    """A layer-derivation assignment (produces a derived layer, not an error)."""
    toks = expr.replace("(", " ").replace(")", " ").split()
    up = [t.upper() for t in toks]
    d = Derivation(name=name, kind="unknown", expr=expr, raw=expr.strip())
    if any(t in _BOOL_OPS for t in up):
        ops = {t for t in up if t in _BOOL_OPS}
        operands = [t for t in toks if t.upper() not in _BOOL_OPS]
        if len(ops) == 1 and operands:
            d.kind = "bool"
            d.bool_sym = _BOOL_OPS[next(iter(ops))]
            d.operands = operands
        else:
            d.kind = "bool"
            d.supported = False
            d.reason = "mixed-precedence boolean expression"
        return d
    for i, t in enumerate(up):
        if t in _SIZE_OPS:
            d.kind = "size"
            d.size_op = t
            d.operands = [toks[0]]
            mnum = re.search(r'([0-9]*\.?[0-9]+)', " ".join(toks[i + 1:]))
            d.value = float(mnum.group(1)) if mnum else None
            if t in ("SHRINK", "UNDERSIZE") and d.value is not None:
                d.value = -abs(d.value)
            return d
        if t in _SELECT_OPS:
            d.kind = "select"
            d.select_op = t
            d.operands = [toks[0], toks[i + 1]] if i + 1 < len(toks) else [toks[0]]
            return d
    # a bare `name = otherlayer` passthrough / alias
    if len(toks) == 1 and re.fullmatch(r'[A-Za-z_][\w.$]*', toks[0]):
        d.kind = "passthrough"
        d.operands = [toks[0]]
        return d
    d.supported = False
    d.reason = "unrecognised derivation expression"
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
                rules.append(_make_rule(name, mm))
            else:
                cp = _COPY_RE.search(joined)
                if cp:                       # NAME { COPY errlayer } -> re-output an error layer
                    rules.append(Rule(name=name, op="COPY", layer1=cp.group(1),
                                      layer2=None, cmp="", value=0.0,
                                      supported=False,
                                      reason="COPY of a derived error layer (passthrough)",
                                      raw=joined.strip()))
            i = j
            continue
        ah = _ASSIGN_HEAD.match(line)
        if ah and not _LAYER_RE.match(line):
            rhs = ah.group(2)
            mm = _MEAS_RE.search(rhs)
            if mm:
                rules.append(_make_rule(ah.group(1), mm))
            else:
                d = _parse_derivation(ah.group(1), rhs)
                if d:
                    derivations.append(d)
        i += 1
    return Deck(layers, derivations, rules, connects)
