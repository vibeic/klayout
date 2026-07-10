"""
run_svrf_drc.py — the in-KLayout SVRF interpreter.

Run this INSIDE KLayout (it uses `pya`). It reads a Calibre/SVRF-format deck and
executes every statement DIRECTLY on KLayout's own DRC engine via `pya.Region.*`
and `pya.LayoutToNetlist`. NO intermediate `.drc` file, NO separate compiler tool.

Execution model (matches real decks): ONE unified layer namespace, statements run
top-to-bottom. A name can be a DRAWN layer, a DERIVED layer (boolean/size/select/
holes/rectangles/extents/metric-select), or an ERROR layer (a measurement's
violations). `NAME { COPY errlayer }` reports a previously-computed error layer —
the way a foundry deck names its rules. Modifiers map 1:1 to native check params
(ABUT->ignore_angle, PROJECTING->Projection, OPPOSITE->opposite_filter, CONNECTED/
NOT CONNECTED->net extraction). Edge-typed derivations (LENGTH/ANGLE/EDGE) and
DENSITY/ANTENNA have no polygon-DRC equivalent -> the dependent rule is honestly
SKIPPED, never falsely PASSed.

    klayout -b -r run_svrf_drc.py \
        -rd root=<repo> -rd deck=<deck.rule> -rd layout=<in.gds> -rd report=<out.txt>
"""
import re
import sys
import pya

root = globals().get("root", ".")
deck_path = globals().get("deck")
layout_path = globals().get("layout")
report_path = globals().get("report", "svrf_drc_report.txt")

sys.path.insert(0, root)
from svrf_klayout.svrf_parse import parse_deck, Derivation, Rule  # noqa: E402

_METRICS = {"euclidian": pya.Region.Euclidian, "projection": pya.Region.Projection,
            "square": pya.Region.Square}
_PC = {"same": "SamePropertiesConstraint", "different": "DifferentPropertiesConstraint"}


class _NetLayerUnavailable(Exception):
    """A connectivity rule referenced a layer that has no shapes in net extraction —
    the net-aware check cannot run, so the rule is honestly SKIPPED (not ERRORed)."""


class Engine:
    def __init__(self, layout_path, deck_text):
        self.layout = pya.Layout()
        self.layout.read(layout_path)
        self.top = self.layout.top_cell()
        self.dbu = self.layout.dbu
        self.deck = parse_deck(deck_text)
        self.regions = {}          # name -> pya.Region (drawn / derived / error)
        self.edges_ns = {}         # name -> pya.Edges (edge-derived layers)
        self.edge_layers = set()   # names that are edge (Edges) layers
        self.unmodeled = set()     # names whose layer is genuinely unsupported
        self.results = []          # (verdict, rule, info) for measurement + COPY statements
        self._l2n = None

    # -- namespace resolution ------------------------------------------------
    def _drawn(self, name):
        binding = self.deck.layers.get(name)
        if not binding:
            return None
        reg = pya.Region()
        for num, dt in binding:          # union every GDS (layer, datatype) purpose
            reg.insert(self.top.begin_shapes_rec(self.layout.layer(num, dt)))
        return reg

    def resolve(self, name):
        if name in self.regions:
            return self.regions[name]
        r = self._drawn(name)
        if r is None:
            r = pya.Region()
        self.regions[name] = r
        return r

    def _dbu(self, um):
        return int(round(um / self.dbu))

    # -- run everything in source order --------------------------------------
    def execute(self):
        # pass 1: derivations + measurement rules (build every layer, incl. error layers).
        # COPY (report of an error layer) is deferred to pass 2 so forward-references
        # to a later-defined error layer still resolve.
        copies = []
        for st in self.deck.statements:
            if isinstance(st, Derivation):
                self._exec_derivation(st)
            elif st.op == "COPY":
                copies.append(st)
            else:
                self._exec_rule(st)
        for st in copies:
            self._exec_rule(st)
        return self.results

    def _as_edges(self, name):
        if name in self.edges_ns:
            return self.edges_ns[name]
        return self.resolve(name).edges()

    def _edge_or_region(self, name):
        return self.edges_ns[name] if name in self.edge_layers else self.resolve(name)

    def _coincident_edges(self, ea, eb, want):
        """A.edges & B.edges = colinear-overlap segments (A-oriented). For
        COINCIDENT INSIDE/OUTSIDE, keep segments whose matching B-edge runs the
        SAME direction (inside: interiors on the same side) or OPPOSITE
        (outside: abutting). Falls back to the full coincident set when too
        large to split per-edge."""
        coin = ea & eb
        if want not in ("inside", "outside") or coin.count() == 0:
            return coin
        if ea.count() > 40000 or eb.count() > 40000:
            return coin                       # conservative superset (too large to split)
        bedges = list(eb.each())
        out = pya.Edges()
        for s in coin.each():
            se = pya.Edge(s.p1, s.p2)
            for be in bedges:
                if be.contains(se.p1) and be.contains(se.p2):   # s is a sub-segment of be
                    same = (be.dx() * se.dx() + be.dy() * se.dy()) > 0
                    if same == (want == "inside"):
                        out.insert(s)
                    break
        return out

    def _build_edges(self, d):
        if d.kind == "metric_select":
            e = self._as_edges(d.operands[0])
            lo, hi = d.bounds or (None, None)
            if d.metric == "LENGTH":
                if d.neq is not None:
                    return e.with_length(self._dbu(d.neq), True)
                if lo is not None and lo == hi:                 # exact length
                    return e.with_length(self._dbu(lo), False)
                return e.with_length(self._dbu(lo) if lo is not None else None,
                                     self._dbu(hi) if hi is not None else None, False)
            # ANGLE (degrees, undirected 0..180)
            if d.neq is not None:
                return e.with_angle(d.neq, True)
            if lo is not None and lo == hi:                     # exact angle
                return e.with_angle(lo, False)
            return e.with_angle(lo if lo is not None else 0.0,
                                hi if hi is not None else 90.0, False)
        # kind == "edge"
        p = d.params or {}
        op = (d.select_op or "EDGE").upper()
        a = self._as_edges(d.operands[0])
        b = self._edge_or_region(d.operands[1]) if len(d.operands) > 1 else None
        if op == "INSIDE" and b is not None:
            return a.inside_part(b)
        if op == "OUTSIDE" and b is not None:
            return a.outside_part(b)
        if op == "TOUCH" and b is not None:
            return a.interacting(b)
        if op in ("COINCIDENT", "COIN"):
            if b is None:
                return a
            be = b if isinstance(b, pya.Edges) else b.edges()
            want = "inside" if p.get("inside") else "outside" if p.get("outside") else None
            return self._coincident_edges(a, be, want)
        # plain EDGE / INNER EDGE / OUTER EDGE / DFM edge ops (STRAIGHT/CONVEX/ACUTE/...)
        base = self.resolve(d.operands[0])
        if p.get("inner"):
            return base.holes().edges()
        if p.get("outer"):
            return base.hulls().edges() if hasattr(base, "hulls") else base.edges()
        return base.edges()

    def _exec_derivation(self, d):
        if not d.supported:
            self.unmodeled.add(d.name)
            self.regions[d.name] = pya.Region()
            return
        if d.edge_typed:
            try:
                self.edges_ns[d.name] = self._build_edges(d)
                self.edge_layers.add(d.name)
                self.regions[d.name] = pya.Region()   # placeholder for region-typed consumers
            except Exception as e:
                self.unmodeled.add(d.name)
                self.regions[d.name] = pya.Region()
                d.supported = False
                d.reason = f"edge build error: {e}"
            return
        try:
            k = d.kind
            if k == "bool_expr":
                reg, used = self._eval_bool(d.expr)
                self.regions[d.name] = reg
                if any(u in self.unmodeled for u in used):
                    self.unmodeled.add(d.name)
            elif k == "bool":
                acc = self.resolve(d.operands[0]).dup()
                for o in d.operands[1:]:
                    r = self.resolve(o)
                    if d.bool_sym == "&":
                        acc &= r
                    elif d.bool_sym == "|":
                        acc |= r
                    elif d.bool_sym == "-":
                        acc -= r
                    elif d.bool_sym == "^":
                        acc ^= r
                self.regions[d.name] = acc
            elif k == "size":
                self.regions[d.name] = self.resolve(d.operands[0]).sized(self._dbu(d.value or 0.0))
            elif k == "select":
                a = self.resolve(d.operands[0])
                b = self.resolve(d.operands[1]) if len(d.operands) > 1 else pya.Region()
                op = (d.select_op or "").upper()
                negate = (d.params or {}).get("negate", False)
                pos = {"INTERACT": a.interacting, "INSIDE": a.inside, "OUTSIDE": a.outside,
                       "CUT": a.interacting, "TOUCH": a.interacting, "ENCLOSE": a.interacting}
                neg = {"INTERACT": a.not_interacting, "INSIDE": a.not_inside,
                       "OUTSIDE": a.not_outside, "CUT": a.not_interacting,
                       "TOUCH": a.not_interacting, "ENCLOSE": a.not_interacting}
                fn = (neg if negate else pos).get(op, a.not_interacting if negate else a.interacting)
                self.regions[d.name] = fn(b)
            elif k == "passthrough":
                self.regions[d.name] = self.resolve(d.operands[0])
            elif k == "empty":
                self.regions[d.name] = pya.Region()
            elif k == "holes":
                self.regions[d.name] = self.resolve(d.operands[0]).holes()
            elif k == "rectangles":
                self.regions[d.name] = self._rectangles(d)
            elif k == "extents":
                self.regions[d.name] = self.resolve(d.operands[0]).extents()
            elif k == "merge":
                self.regions[d.name] = self.resolve(d.operands[0]).merged()
            elif k == "vertex":
                self.regions[d.name] = self._vertex(d)
            elif k == "with_edge":
                a = self.resolve(d.operands[0])
                b = self.resolve(d.operands[1]) if len(d.operands) > 1 else pya.Region()
                self.regions[d.name] = a.interacting(b.edges())
            elif k == "expand":
                edges = self.resolve(d.operands[0]).edges()
                w = self._dbu(abs(d.value or 0.0))
                p = d.params or {}
                if p.get("inside"):
                    self.regions[d.name] = edges.extended_in(w)
                elif p.get("outside"):
                    self.regions[d.name] = edges.extended_out(w)
                else:
                    self.regions[d.name] = edges.extended(0, 0, w, w)
            elif k == "metric_select":
                self.regions[d.name] = self._metric_select(d)
            elif k == "net_ratio":
                self.regions[d.name] = self._net_area_ratio(d)
            else:
                self.unmodeled.add(d.name)
                self.regions[d.name] = pya.Region()
        except Exception as e:
            self.unmodeled.add(d.name)
            self.regions[d.name] = pya.Region()
            d.supported = False
            d.reason = f"build error: {e}"

    def _eval_bool(self, expr):
        """Evaluate a pure boolean layer expression (parens + AND/OR/NOT/XOR,
        left-to-right) into a Region. Returns (region, operand_names_used)."""
        toks = re.findall(r'\(|\)|[^\s()]+', expr)
        used = []
        pos = [0]

        def atom():
            t = toks[pos[0]]
            if t == "(":
                pos[0] += 1
                v = expression()
                if pos[0] < len(toks) and toks[pos[0]] == ")":
                    pos[0] += 1
                return v
            pos[0] += 1
            used.append(t)
            return self.resolve(t).dup()

        def expression():
            val = atom()
            while pos[0] < len(toks) and toks[pos[0]].upper() in ("AND", "OR", "NOT", "XOR"):
                op = toks[pos[0]].upper(); pos[0] += 1
                rhs = atom()
                if op == "AND":
                    val &= rhs
                elif op == "OR":
                    val |= rhs
                elif op == "NOT":
                    val -= rhs
                else:
                    val ^= rhs
            return val

        return expression(), used

    def _metric_select(self, d):
        r = self.resolve(d.operands[0])
        lo, hi = d.bounds or (None, None)
        if d.neq is not None:                        # `!= N` -> inverse exact select
            n = int(round(d.neq / (self.dbu * self.dbu))) if d.metric == "AREA" else self._dbu(d.neq)
            return (r.with_area(n, True) if d.metric == "AREA" else r.with_perimeter(n, True))
        if d.metric == "AREA":
            lo_dbu = int(round(lo / (self.dbu * self.dbu))) if lo is not None else None
            hi_dbu = int(round(hi / (self.dbu * self.dbu))) if hi is not None else None
            return r.with_area(lo_dbu, hi_dbu, False)
        if d.metric == "PERIMETER":
            return r.with_perimeter(self._dbu(lo) if lo is not None else None,
                                    self._dbu(hi) if hi is not None else None, False)
        raise NotImplementedError(f"metric {d.metric}")   # LENGTH/ANGLE are edge-typed

    def _rectangles(self, d):
        r = self.resolve(d.operands[0]).rectangles()
        p = d.params or {}
        if "w" in p:
            lo, hi = p["w"]
            r = r.with_bbox_width(self._dbu(lo) if lo is not None else None,
                                  self._dbu(hi) if hi is not None else None, False)
        if "h" in p:
            lo, hi = p["h"]
            r = r.with_bbox_height(self._dbu(lo) if lo is not None else None,
                                   self._dbu(hi) if hi is not None else None, False)
        if "aspect" in p and hasattr(r, "with_bbox_aspect_ratio"):
            lo, hi = p["aspect"]
            r = r.with_bbox_aspect_ratio(lo, hi, False)
        return r

    def _vertex(self, d):
        lo, hi = d.bounds or (None, None)
        out = pya.Region()
        for poly in self.resolve(d.operands[0]).each():
            npts = poly.num_points()
            if (lo is None or npts >= lo) and (hi is None or npts <= hi):
                out.insert(poly)
        return out

    # -- connectivity (net extraction) ---------------------------------------
    def _build_l2n(self):
        """Build the LayoutToNetlist once from the full CONNECT stack. Also register
        every layer used by a NET AREA RATIO derivation so its per-net area can be
        queried after extraction."""
        if self._l2n is not None:
            return self._l2n
        l2n = pya.LayoutToNetlist("TOP", self.dbu)
        reg = {}

        def get(nm):
            if nm not in reg:
                reg[nm] = self.resolve(nm)
                l2n.register(reg[nm], nm)
                l2n.connect(reg[nm])          # make it a net-bearing layer
            return reg[nm]

        for a, b, via in self.deck.connects:
            ra, rb = get(a), get(b)
            if via:
                rv = get(via)
                l2n.connect(ra, rv); l2n.connect(rb, rv)
            else:
                l2n.connect(ra, rb)
        # register NET AREA RATIO operands so shapes_of_net can see them
        for dv in self.deck.derivations:
            if dv.kind == "net_ratio" and dv.supported:
                for nm in dv.operands[:2]:
                    get(nm)
        # register every layer a CONNECTED / NOT CONNECTED rule queries, so .nets()
        # never fails on an unregistered layer (an unconnected layer -> per-shape nets)
        for rl in self.deck.rules:
            if getattr(rl, "connectivity", None) is not None:
                for nm in (rl.layer1, rl.layer2):
                    if nm:
                        get(nm)
        l2n.extract_netlist()
        self._l2n = (l2n, reg)
        return self._l2n

    def _l2n_nets(self, name):
        l2n, reg = self._build_l2n()
        base = reg.get(name)
        if base is None:                    # layer never entered net extraction
            raise _NetLayerUnavailable(name)
        try:
            return base.nets(l2n, net_prop_name="net")
        except Exception:                   # empty/unextracted layer -> not net-queryable
            raise _NetLayerUnavailable(name)

    def _net_area_ratio(self, d):
        """area(A on net) / area(B on net) per net; flag nets meeting the comparison.
        Emits the offending B geometry as the error layer."""
        A, B = d.operands[0], d.operands[1]
        l2n, reg = self._build_l2n()
        ra = reg.get(A) or self.resolve(A)
        rb = reg.get(B) or self.resolve(B)
        p = d.params or {}
        cmp, thr = p.get("cmp", "=="), float(p.get("thr", 0.0))

        def viol(x):
            return {"<": x < thr, "<=": x <= thr, ">": x > thr, ">=": x >= thr,
                    "==": x == thr, "!=": x != thr}.get(cmp, False)

        out = pya.Region()
        for c in l2n.netlist().each_circuit():
            for net in c.each_net():
                sb = l2n.shapes_of_net(net, rb)
                ab = sb.area() if sb else 0
                if ab == 0:
                    continue                    # B absent on this net: ratio undefined, skip
                sa = l2n.shapes_of_net(net, ra)
                aa = sa.area() if sa else 0
                if viol(aa / ab):
                    out += sb
        return out

    # -- checks --------------------------------------------------------------
    def _check_kwargs(self, r, allow_filters=True):
        kw = {"metrics": _METRICS.get(r.metrics, pya.Region.Euclidian)}
        if r.ignore_angle is not None:
            kw["ignore_angle"] = r.ignore_angle
        if r.whole_edges:
            kw["whole_edges"] = True
        if r.min_projection is not None:
            kw["min_projection"] = self._dbu(r.min_projection)
        if r.max_projection is not None:
            kw["max_projection"] = self._dbu(r.max_projection)
        if r.shielded is not None:
            kw["shielded"] = r.shielded
        if allow_filters and r.opposite and hasattr(pya.Region, "OnlyOpposite"):
            kw["opposite_filter"] = pya.Region.OnlyOpposite
        return kw

    def _inputs_unmodeled(self, r):
        return any(L and L in self.unmodeled for L in (r.layer1, r.layer2))

    def _is_edge_rule(self, r):
        return (r.layer1 in self.edge_layers) or (r.layer2 and r.layer2 in self.edge_layers)

    def _edge_check_kwargs(self, r):
        kw = {"metrics": _METRICS.get(r.metrics, pya.Region.Euclidian)}
        if r.ignore_angle is not None:
            kw["ignore_angle"] = r.ignore_angle
        if r.whole_edges:
            kw["whole_edges"] = True
        if r.min_projection is not None:
            kw["min_projection"] = self._dbu(r.min_projection)
        if r.max_projection is not None:
            kw["max_projection"] = self._dbu(r.max_projection)
        return kw

    def _exec_edge_rule(self, r):
        """Dispatch a spacing/width/enclosure rule whose input is an Edges layer
        (EDGE/COINCIDENT/LENGTH/ANGLE derived) to the native pya.Edges check
        family. Native Edges checks return EdgePairs, same as Region checks."""
        d = self._dbu(r.value)
        e1 = self._as_edges(r.layer1)

        def run(kw):
            if r.op == "EXTERNAL":
                return (e1.separation_check(self._as_edges(r.layer2), d, **kw)
                        if r.layer2 else e1.space_check(d, **kw))
            if r.op == "INTERNAL":
                return (e1.overlap_check(self._as_edges(r.layer2), d, **kw)
                        if r.layer2 else e1.width_check(d, **kw))
            if r.op == "ENCLOSURE":
                outer = self._as_edges(r.layer2) if r.layer2 else pya.Edges()
                return outer.enclosing_check(e1, d, **kw)
            return None

        if r.op not in ("EXTERNAL", "INTERNAL", "ENCLOSURE"):
            self.results.append(("SKIP", r, f"edge op {r.op} has no Edges check"))
            return
        try:
            ep = run(self._edge_check_kwargs(r))
        except Exception:
            try:
                ep = run({})               # some Edges checks reject a filter kwarg
            except Exception as ex:
                self.results.append(("SKIP", r, f"edge-check unsupported: {str(ex)[:60]}"))
                return
        cnt = ep.size()
        self.regions[r.name] = ep.polygons()   # error layer (edge pairs -> polygons)
        self.results.append(("PASS" if cnt == 0 else "FAIL", r, cnt))

    def _exec_density(self, r):
        """Windowed area-density check. density(window) = covered-area / window-area;
        a W x W window slides by STEP s. The rule's comparison expresses the VIOLATION
        condition directly (min-density `< d` and max-density `> d` are both real CMP
        rules), so a window is flagged when `density <cmp> value` holds. Two-layer
        DENSITY uses the merged coverage of both layers."""
        reg = self.resolve(r.layer1)
        if r.layer2:
            reg = reg | self.resolve(r.layer2)
        bb = reg.bbox()
        if bb.empty():
            self.results.append(("PASS", r, 0))
            return
        thr, cmp = r.value, r.cmp

        def viol(dens):
            return {"<": dens < thr, "<=": dens <= thr, ">": dens > thr,
                    ">=": dens >= thr, "==": dens == thr}.get(cmp, False)

        W = self._dbu(r.window) if r.window else None
        if not W or W <= 0:
            windows = [bb]                      # whole-extent single window
        else:
            s = self._dbu(r.step) if r.step else W
            if s <= 0:
                s = W
            while ((bb.width() // s) + 1) * ((bb.height() // s) + 1) > 20000:
                s *= 2                          # coarsen so the sweep stays bounded
            windows = []
            y = bb.bottom
            while y < bb.top:
                x = bb.left
                while x < bb.right:
                    windows.append(pya.Box(x, y, x + W, y + W))
                    x += s
                y += s
        bad = pya.Region()
        for win in windows:
            area = float(win.area())
            if area <= 0:
                continue
            dens = (reg & pya.Region(win)).area() / area
            if viol(dens):
                bad.insert(win)
        cnt = bad.count()
        self.regions[r.name] = bad
        self.results.append(("PASS" if cnt == 0 else "FAIL", r, cnt))

    def _exec_rule(self, r):
        # COPY: report a previously-computed error layer (foundry rule naming)
        if r.op == "COPY":
            src = r.layer1
            if src in self.unmodeled:            # antenna / net-ratio etc: honest SKIP, never PASS
                self.results.append(("SKIP", r, f"errlayer '{src}' routed to dedicated checker"))
                return
            reg = self.regions.get(src)
            if reg is None:                      # COPY of a plain drawn layer -> report its shapes
                reg = self._drawn(src)
                if reg is None:
                    self.results.append(("SKIP", r, f"errlayer '{src}' unresolved"))
                    return
                self.regions[src] = reg
            c = reg.count()
            self.regions[r.name] = reg
            self.results.append(("PASS" if c == 0 else "FAIL", r, c))
            return
        if not r.supported:
            self.results.append(("SKIP", r, r.reason or "unsupported"))
            return
        if self._inputs_unmodeled(r):
            self.results.append(("SKIP", r, "input layer is edge-typed/unmodeled"))
            return
        if self._is_edge_rule(r):
            self._exec_edge_rule(r)
            return
        if r.op == "DENSITY":
            self._exec_density(r)
            return
        d = self._dbu(r.value)
        try:
            if r.connectivity is not None:
                if not self.deck.connects:
                    self.results.append(("SKIP", r, "connectivity but no CONNECT stack"))
                    return
                if r.op != "EXTERNAL" or not r.layer2:
                    self.results.append(("SKIP", r, "connectivity needs 2-layer EXTERNAL"))
                    return
                kw = self._check_kwargs(r)
                kw["property_constraint"] = getattr(pya.Region, _PC[r.connectivity])
                ep = self._l2n_nets(r.layer1).separation_check(self._l2n_nets(r.layer2), d, **kw)
                viol = ep.polygons()
            elif r.op == "AREA":
                area_dbu = int(round(r.value / (self.dbu * self.dbu)))
                viol = self.resolve(r.layer1).with_area(None, area_dbu, False)
                ep = None
            else:
                l1 = self.resolve(r.layer1)
                if r.op == "EXTERNAL":
                    kw = self._check_kwargs(r)
                    ep = (l1.separation_check(self.resolve(r.layer2), d, **kw)
                          if r.layer2 else l1.space_check(d, **kw))
                elif r.op == "INTERNAL":
                    ep = (l1.overlap_check(self.resolve(r.layer2), d, **self._check_kwargs(r))
                          if r.layer2 else l1.width_check(d, **self._check_kwargs(r, False)))
                elif r.op == "NOTCH":
                    ep = l1.notch_check(d, **self._check_kwargs(r, False))
                elif r.op == "ENCLOSURE":
                    outer = self.resolve(r.layer2) if r.layer2 else pya.Region()
                    ep = outer.enclosing_check(l1, d, **self._check_kwargs(r))
                else:
                    self.results.append(("SKIP", r, f"op {r.op} not in core"))
                    return
                viol = ep.polygons()
        except _NetLayerUnavailable as e:
            self.results.append(("SKIP", r, f"net layer '{e}' has no extracted shapes"))
            return
        except Exception as e:
            self.results.append(("ERROR", r, str(e)[:80]))
            return
        cnt = ep.size() if ep is not None else viol.count()
        self.regions[r.name] = viol            # this measurement IS an error layer
        self.results.append(("PASS" if cnt == 0 else "FAIL", r, cnt))


def main():
    deck_text = open(deck_path, encoding="utf-8", errors="replace").read()
    eng = Engine(layout_path, deck_text)
    eng.execute()
    ver = pya.Application.instance().version() if pya.Application.instance() else ""
    from collections import Counter
    tally = Counter(v for v, _, _ in eng.results)
    lines = [f"# SVRF-native DRC via KLayout {ver}".rstrip(),
             f"# deck={deck_path}  layout={layout_path}  dbu={eng.dbu}",
             f"# {len(eng.deck.layers)} layers, {len(eng.deck.derivations)} derivations, "
             f"{len(eng.deck.rules)} rules  |  {dict(tally)}", ""]
    for verdict, r, info in eng.results:
        tag = (f"[metrics={r.metrics}"
               f"{',ignore_angle=' + str(r.ignore_angle) if r.ignore_angle is not None else ''}"
               f"{',opposite' if r.opposite else ''}{',whole' if r.whole_edges else ''}]"
               if r.op != "COPY" else "[COPY]")
        lines.append(f"{verdict:5s} {r.name:18s} {r.op} {r.layer1}"
                     f"{('/' + r.layer2) if r.layer2 else ''} {r.cmp} {r.value} {tag} -> {info}")
    lines.append("")
    lines.append(f"# tally: {dict(tally)}")
    open(report_path, "w").write("\n".join(lines) + "\n")
    print("\n".join(lines[:3]) + f"\n# tally: {dict(tally)}")


if globals().get("deck") and globals().get("layout"):
    main()
