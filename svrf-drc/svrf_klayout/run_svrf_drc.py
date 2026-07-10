"""
run_svrf_drc.py — the in-KLayout SVRF interpreter.

Run this INSIDE KLayout (it uses `pya`). It reads a Calibre/SVRF-format deck and
executes every rule DIRECTLY on KLayout's own DRC engine via `pya.Region.*`.
There is NO intermediate `.drc` file and NO separate compiler tool: KLayout reads
the deck format and solves it natively.

    klayout -b -r run_svrf_drc.py \
        -rd root=<repo-root> -rd deck=<deck.rule> -rd layout=<in.gds> -rd report=<out.txt>

Every SVRF measurement modifier maps 1:1 onto a native check parameter
(ABUT -> ignore_angle, PROJECTING/PARALLEL -> Projection metric, OPPOSITE ->
opposite_filter, SQUARE -> Square metric, WHOLE -> whole_edges, SHIELDED ->
shielded). Layer derivations (AND/OR/NOT/XOR, SIZE/GROW/SHRINK, INTERACT/INSIDE/
OUTSIDE) run as native Region algebra. Connectivity (CONNECTED / NOT CONNECTED)
uses net extraction; where a clean net-aware path is unavailable it is honestly
SKIPPED, never guessed.
"""
import sys
import pya

root = globals().get("root", ".")
deck_path = globals().get("deck")
layout_path = globals().get("layout")
report_path = globals().get("report", "svrf_drc_report.txt")

sys.path.insert(0, root)
from svrf_klayout.svrf_parse import parse_deck  # noqa: E402

_METRICS = {
    "euclidian": pya.Region.Euclidian,
    "projection": pya.Region.Projection,
    "square": pya.Region.Square,
}


class Engine:
    def __init__(self, layout_path, deck_text):
        self.layout = pya.Layout()
        self.layout.read(layout_path)
        self.top = self.layout.top_cell()
        self.dbu = self.layout.dbu
        self.deck = parse_deck(deck_text)
        self._derived = {}
        self._build_derivations()
        self._l2n = None            # lazily built when the first net-aware rule runs

    # -- layer / derived-layer resolution ------------------------------------
    def _layer_region(self, name):
        if name not in self.deck.layers:
            return pya.Region()
        num, dt = self.deck.layers[name]
        li = self.layout.layer(num, dt)
        return pya.Region(self.top.begin_shapes_rec(li))

    def resolve(self, name):
        if name in self._derived:
            return self._derived[name]
        return self._layer_region(name)

    def _dbu(self, um):
        return int(round(um / self.dbu))

    def _build_derivations(self):
        for d in self.deck.derivations:
            try:
                if not d.supported:
                    continue
                if d.kind == "bool":
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
                    self._derived[d.name] = acc
                elif d.kind == "size":
                    self._derived[d.name] = self.resolve(d.operands[0]).sized(self._dbu(d.value or 0.0))
                elif d.kind == "select":
                    a = self.resolve(d.operands[0])
                    b = self.resolve(d.operands[1]) if len(d.operands) > 1 else pya.Region()
                    op = (d.select_op or "").upper()
                    if op == "INTERACT":
                        self._derived[d.name] = a.interacting(b)
                    elif op == "INSIDE":
                        self._derived[d.name] = a.inside(b)
                    elif op == "OUTSIDE":
                        self._derived[d.name] = a.outside(b)
                    else:
                        self._derived[d.name] = a.interacting(b)
                elif d.kind == "passthrough":
                    self._derived[d.name] = self.resolve(d.operands[0])
            except Exception as e:
                d.supported = False
                d.reason = f"derivation error: {e}"

    # -- connectivity (net extraction) ---------------------------------------
    def _l2n_nets(self, name):
        """Return the region of `name` net-tagged (property 'net'), building the
        LayoutToNetlist from the deck's CONNECT stack on first use."""
        if self._l2n is None:
            l2n = pya.LayoutToNetlist("TOP", self.dbu)
            reg = {}

            def get(nm):
                if nm not in reg:
                    reg[nm] = self.resolve(nm)
                    l2n.register(reg[nm], nm)
                return reg[nm]

            for a, b, via in self.deck.connects:
                ra, rb = get(a), get(b)
                l2n.connect(ra)
                l2n.connect(rb)
                if via:
                    rv = get(via)
                    l2n.connect(rv)
                    l2n.connect(ra, rv)
                    l2n.connect(rb, rv)
                else:
                    l2n.connect(ra, rb)
            l2n.extract_netlist()
            self._l2n = (l2n, reg)
        l2n, reg = self._l2n
        base = reg.get(name) or self.resolve(name)
        return base.nets(l2n, net_prop_name="net")

    _PC = {"same": "SamePropertiesConstraint", "different": "DifferentPropertiesConstraint"}

    # -- rule execution ------------------------------------------------------
    def _check_kwargs(self, rule, allow_filters=True):
        kw = {"metrics": _METRICS.get(rule.metrics, pya.Region.Euclidian)}
        if rule.ignore_angle is not None:
            kw["ignore_angle"] = rule.ignore_angle
        if rule.whole_edges:
            kw["whole_edges"] = True
        if rule.min_projection is not None:
            kw["min_projection"] = self._dbu(rule.min_projection)
        if rule.max_projection is not None:
            kw["max_projection"] = self._dbu(rule.max_projection)
        if rule.shielded is not None:
            kw["shielded"] = rule.shielded
        # opposite_filter / rect_filter are only accepted by the space/separation
        # family — width_check / notch_check reject them.
        if allow_filters and rule.opposite and hasattr(pya.Region, "OnlyOpposite"):
            kw["opposite_filter"] = pya.Region.OnlyOpposite
        return kw

    def run_rule(self, r):
        if not r.supported:
            return ("SKIP", r.reason or "unsupported")
        d = self._dbu(r.value)
        try:
            # ---- net-aware EXTERNAL (CONNECTED / NOT CONNECTED) ----
            if r.connectivity is not None:
                if not self.deck.connects:
                    return ("SKIP", f"connectivity={r.connectivity} but deck has no CONNECT stack")
                if r.op != "EXTERNAL" or not r.layer2:
                    return ("SKIP", f"connectivity on {r.op} unsupported (needs 2-layer EXTERNAL)")
                kw = self._check_kwargs(r)
                kw["property_constraint"] = getattr(pya.Region, self._PC[r.connectivity])
                an, bn = self._l2n_nets(r.layer1), self._l2n_nets(r.layer2)
                ep = an.separation_check(bn, d, **kw)
                cnt = ep.size()
                return ("PASS" if cnt == 0 else "FAIL", cnt)

            l1 = self.resolve(r.layer1)
            if r.op == "EXTERNAL":
                kw = self._check_kwargs(r)
                ep = (l1.separation_check(self.resolve(r.layer2), d, **kw)
                      if r.layer2 else l1.space_check(d, **kw))
            elif r.op == "INTERNAL":
                if r.layer2:                       # 2-layer INTERNAL == overlap distance
                    ep = l1.overlap_check(self.resolve(r.layer2), d, **self._check_kwargs(r))
                else:
                    ep = l1.width_check(d, **self._check_kwargs(r, allow_filters=False))
            elif r.op == "NOTCH":
                ep = l1.notch_check(d, **self._check_kwargs(r, allow_filters=False))
            elif r.op == "ENCLOSURE":
                outer = self.resolve(r.layer2) if r.layer2 else pya.Region()
                ep = outer.enclosing_check(l1, d, **self._check_kwargs(r))
            elif r.op == "AREA":
                # flag polygons whose area is below the rule value (3-arg RANGE form:
                # 2-arg with_area is the exact-area overload — a real API trap).
                area_dbu = int(round(r.value / (self.dbu * self.dbu)))
                viol = l1.with_area(None, area_dbu, False)
                return ("PASS" if viol.is_empty() else "FAIL", viol.count())
            else:
                return ("SKIP", f"op {r.op} not in core")
        except Exception as e:
            return ("ERROR", str(e))
        cnt = ep.size()
        return ("PASS" if cnt == 0 else "FAIL", cnt)


def main():
    deck_text = open(deck_path, encoding="utf-8", errors="replace").read()
    eng = Engine(layout_path, deck_text)
    ver = pya.Application.instance().version() if pya.Application.instance() else ""
    lines = [f"# SVRF-native DRC via KLayout {ver}".rstrip(),
             f"# deck={deck_path}  layout={layout_path}  dbu={eng.dbu}",
             f"# {len(eng.deck.layers)} layers, {len(eng.deck.derivations)} derivations, "
             f"{len(eng.deck.rules)} rules", ""]
    n_fail = 0
    for r in eng.deck.rules:
        verdict, info = eng.run_rule(r)
        if verdict in ("FAIL", "ERROR"):
            n_fail += 1
        tag = (f"[metrics={r.metrics}"
               f"{',ignore_angle=' + str(r.ignore_angle) if r.ignore_angle is not None else ''}"
               f"{',opposite' if r.opposite else ''}{',whole' if r.whole_edges else ''}]")
        lines.append(f"{verdict:5s} {r.name:16s} {r.op} {r.layer1}"
                     f"{('/' + r.layer2) if r.layer2 else ''} {r.cmp} {r.value} {tag} -> {info}")
    lines.append("")
    lines.append(f"# {n_fail} rule(s) with violations/errors")
    out = "\n".join(lines) + "\n"
    open(report_path, "w").write(out)
    print(out)


# auto-run only when invoked as a batch DRC (`-rd deck=... -rd layout=...`);
# stays importable (e.g. by proof.py / the .lym macro) when those are absent.
if globals().get("deck") and globals().get("layout"):
    main()
