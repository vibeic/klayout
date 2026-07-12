
/*

  dbSVRFEngine.cc -- native C++ port of svrf_klayout/run_svrf_drc.py

  See dbSVRFEngine.h. Executes a parsed db::SVRFDeck directly on db:: geometry.
  Report format is byte-identical to run_svrf_drc.py::main(). NO vendor data.

*/

#include "dbSVRFEngine.h"

#include "dbReader.h"
#include "dbWriter.h"
#include "dbSaveLayoutOptions.h"
#include "dbRecursiveShapeIterator.h"
#include "dbRegionUtils.h"
#include "dbEdgesUtils.h"
#include "dbLayoutToNetlistEnums.h"
#include "dbNetlist.h"
#include "dbCircuit.h"
#include "dbNet.h"
#include "dbLayerProperties.h"
#include "dbPropertyConstraint.h"

#include "tlStream.h"
#include "tlVariant.h"
#include "tlException.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <limits>

namespace db
{

// ---------------------------------------------------------------------------
//  formatting helpers -- reproduce Python str(float) / dict(Counter) EXACTLY
//  (the report is a frozen contract parsed by the vibe-ic plugin)
// ---------------------------------------------------------------------------

namespace
{

//  Python str(float): shortest decimal that round-trips, with a ".0" kept on
//  integral values (str(90.0) == "90.0", str(0.14) == "0.14", str(0.001) ==
//  "0.001"). Implemented C++11-clean (no <charconv>) so this file compiles under
//  the KLayout build's -std=c++11: try %.*g at increasing precision until the
//  formatted value parses back to the exact same double -- that IS the shortest
//  round-trip -- then re-add the ".0" that %g drops for integers. For the decimal
//  dimension values decks carry (0.005 .. ~100) this matches Python str() exactly.
std::string py_float_str (double v)
{
  if (std::isnan (v)) {
    return "nan";
  }
  if (std::isinf (v)) {
    return v < 0 ? "-inf" : "inf";
  }
  if (v == 0.0) {
    return std::signbit (v) ? "-0.0" : "0.0";
  }
  char buf[64];
  //  Python str() picks FIXED notation unless the decimal exponent is < -4 or
  //  >= 16 (then scientific). Decide first, then emit the shortest round-trip in
  //  that style. DRC dimension values (1e-4 .. 1e5) are always fixed.
  int e = (int) std::floor (std::log10 (std::fabs (v)));
  if (e >= -4 && e < 16) {
    //  shortest fixed-point decimal that round-trips
    for (int d = 0; d <= 17; ++d) {
      std::snprintf (buf, sizeof (buf), "%.*f", d, v);
      if (std::strtod (buf, 0) == v) {
        break;
      }
    }
    std::string s (buf);
    if (s.find ('.') == std::string::npos) {
      s += ".0";                       // integral value keeps a trailing ".0"
    }
    return s;
  }
  //  scientific range: shortest %g (exponent form). Not reached by real decks.
  for (int prec = 1; prec <= 17; ++prec) {
    std::snprintf (buf, sizeof (buf), "%.*g", prec, v);
    if (std::strtod (buf, 0) == v) {
      break;
    }
  }
  std::string s (buf);
  if (s.find ('.') == std::string::npos && s.find ('e') == std::string::npos &&
      s.find ('E') == std::string::npos) {
    s += ".0";
  }
  return s;
}

db::metrics_type map_metrics (const std::string &m)
{
  if (m == "projection") {
    return db::Projection;
  }
  if (m == "square") {
    return db::Square;
  }
  return db::Euclidian;
}

//  A boolean derivation param (negate/inside/outside/inner/outer) is stored by the
//  parser as the string "1"/"0" and the KEY IS ALWAYS PRESENT. So test the VALUE,
//  not key existence (matches the reference `params.get(flag)` truthiness). Value-
//  bearing params (w/h/aspect/cmp/thr) are set only when applicable -> those use
//  find() directly.
bool pflag (const std::map<std::string, std::string> &params, const char *key)
{
  std::map<std::string, std::string>::const_iterator it = params.find (key);
  return it != params.end () && it->second == "1";
}

//  A verdict tally in first-appearance order, rendered as a Python dict repr:
//    {'PASS': 4523, 'FAIL': 10}
struct Tally
{
  std::vector<std::pair<std::string, long> > order;
  void bump (const std::string &v)
  {
    for (auto &e : order) {
      if (e.first == v) { e.second += 1; return; }
    }
    order.push_back (std::make_pair (v, 1L));
  }
  std::string repr () const
  {
    std::string s = "{";
    for (size_t i = 0; i < order.size (); ++i) {
      if (i) {
        s += ", ";
      }
      s += "'";
      s += order[i].first;
      s += "': ";
      s += std::to_string (order[i].second);
    }
    s += "}";
    return s;
  }
};

}  // namespace

// ---------------------------------------------------------------------------
//  construction
// ---------------------------------------------------------------------------

SVRFEngine::SVRFEngine (const std::string &layout_path, const SVRFDeck &deck,
                        const std::string &top_cell_name)
  : m_top (0), m_deck (deck)
{
  tl::InputStream stream (layout_path);
  db::Reader reader (stream);
  reader.read (m_layout);
  m_dbu = m_layout.dbu ();

  if (! top_cell_name.empty ()) {
    std::pair<bool, db::cell_index_type> p = m_layout.cell_by_name (top_cell_name.c_str ());
    if (! p.first) {
      throw tl::Exception ("top cell '" + top_cell_name + "' not found in layout " + layout_path);
    }
    m_top = p.second;
  } else {
    //  single-top layouts: unchanged behaviour (raise on multi-top, the honest
    //  signal to pass an explicit cell=).
    std::vector<db::cell_index_type> tops;
    for (db::Layout::top_down_const_iterator t = m_layout.begin_top_down ();
         t != m_layout.end_top_cells (); ++t) {
      tops.push_back (*t);
    }
    if (tops.empty ()) {
      throw tl::Exception ("layout " + layout_path + " has no top cell");
    }
    if (tops.size () > 1) {
      std::string names;
      for (size_t i = 0; i < tops.size (); ++i) {
        if (i) {
          names += ", ";
        }
        names += m_layout.cell_name (tops[i]);
      }
      throw tl::Exception ("layout " + layout_path + " has multiple top cells (" + names +
                           ") -- pass an explicit cell=");
    }
    m_top = tops.front ();
  }
}

db::Coord SVRFEngine::to_dbu (double um) const
{
  return (db::Coord) std::llround (um / m_dbu);
}

// ---------------------------------------------------------------------------
//  namespace resolution
// ---------------------------------------------------------------------------

bool SVRFEngine::drawn (const std::string &name, db::Region &out) const
{
  std::map<std::string, std::vector<std::pair<int, int> > >::const_iterator b = m_deck.layers.find (name);
  if (b == m_deck.layers.end ()) {
    return false;
  }
  db::Region reg;
  const db::Cell &cell = m_layout.cell (m_top);
  for (std::vector<std::pair<int, int> >::const_iterator pd = b->second.begin (); pd != b->second.end (); ++pd) {
    //  get_layer creates the (layer, datatype) index if absent -> empty shapes,
    //  matching pya layout.layer(num, dt).begin_shapes_rec behaviour.
    unsigned int li = const_cast<db::Layout &> (m_layout).get_layer (db::LayerProperties (pd->first, pd->second));
    db::RecursiveShapeIterator si (m_layout, cell, li);
    reg += db::Region (si);
  }
  out = reg;
  return true;
}

db::Region &SVRFEngine::resolve (const std::string &name)
{
  std::map<std::string, db::Region>::iterator it = m_regions.find (name);
  if (it != m_regions.end ()) {
    return it->second;
  }
  db::Region r;
  drawn (name, r);                    // leaves r empty when not a drawn layer
  m_regions[name] = r;
  return m_regions[name];
}

db::Edges SVRFEngine::as_edges (const std::string &name)
{
  std::map<std::string, db::Edges>::iterator it = m_edges_ns.find (name);
  if (it != m_edges_ns.end ()) {
    return it->second;
  }
  return resolve (name).edges ();
}

// ---------------------------------------------------------------------------
//  run everything in source order
// ---------------------------------------------------------------------------

const std::vector<SVRFResult> &SVRFEngine::execute ()
{
  //  pass 1: derivations + measurement rules (build every layer incl. error
  //  layers). COPY is deferred to pass 2 so forward-references still resolve.
  std::vector<const SVRFRule *> copies;
  for (std::vector<SVRFStatement>::const_iterator s = m_deck.statements.begin (); s != m_deck.statements.end (); ++s) {
    if (s->kind == SVRFStatement::Derivation) {
      exec_derivation (m_deck.derivations[s->index]);
    } else {
      const SVRFRule &r = m_deck.rules[s->index];
      if (r.op == "COPY") {
        copies.push_back (&r);
      } else {
        exec_rule (r);
      }
    }
  }
  for (std::vector<const SVRFRule *>::iterator c = copies.begin (); c != copies.end (); ++c) {
    exec_rule (**c);
  }
  //  SVRFDRC_DUMP_GDS=name1,name2,... : write each named region/edge layer to
  //  dump_layers.gds under the cwd (regions -> layer i/0, edges -> layer i/1
  //  as zero-width paths). Parity-diagnosis aid: lets you SEE what a derived
  //  intermediate actually contains vs what Calibre intends (no oracle).
  if (const char *dl = getenv ("SVRFDRC_DUMP_GDS")) {
    try {
      db::Layout out;
      out.dbu (m_dbu);
      db::Cell &top = out.cell (out.add_cell ("DUMP"));
      std::string spec (dl);
      size_t i0 = 0; int li = 0;
      while (i0 <= spec.size ()) {
        size_t comma = spec.find (',', i0);
        std::string nm = spec.substr (i0, comma == std::string::npos ? std::string::npos : comma - i0);
        i0 = (comma == std::string::npos) ? spec.size () + 1 : comma + 1;
        while (!nm.empty () && (nm.front () == ' ')) nm.erase (nm.begin ());
        while (!nm.empty () && (nm.back () == ' ')) nm.pop_back ();
        if (nm.empty ()) continue;
        if (m_edge_layers.count (nm)) {
          unsigned int lay = out.insert_layer (db::LayerProperties (li, 1));
          std::map<std::string, db::Edges>::const_iterator ei = m_edges_ns.find (nm);
          if (ei != m_edges_ns.end ()) {
            for (db::Edges::const_iterator e = ei->second.begin (); !e.at_end (); ++e) {
              top.shapes (lay).insert (*e);
            }
          }
          fprintf (stderr, "DUMP %s -> layer %d/1 (edges)\n", nm.c_str (), li);
        } else {
          unsigned int lay = out.insert_layer (db::LayerProperties (li, 0));
          std::map<std::string, db::Region>::const_iterator ri = m_regions.find (nm);
          if (ri != m_regions.end ()) {
            for (db::Region::const_iterator p = ri->second.begin (); !p.at_end (); ++p) {
              top.shapes (lay).insert (*p);
            }
          }
          fprintf (stderr, "DUMP %s -> layer %d/0 (region)\n", nm.c_str (), li);
        }
        ++li;
      }
      db::SaveLayoutOptions so; so.set_format ("GDS2");
      db::Writer w (so);
      tl::OutputStream os ("dump_layers.gds");
      w.write (out, os);
      fprintf (stderr, "DUMP wrote dump_layers.gds (%d layers)\n", li);
    } catch (std::exception &e) {
      fprintf (stderr, "DUMP failed: %s\n", e.what ());
    }
  }
  return m_results;
}

// ---------------------------------------------------------------------------
//  derivations
// ---------------------------------------------------------------------------

db::Region SVRFEngine::eval_bool_expr (const std::string &expr, std::vector<std::string> &used)
{
  //  tokenize: '(' | ')' | run-of-non-space-non-paren
  std::vector<std::string> toks;
  {
    std::string cur;
    for (size_t i = 0; i < expr.size (); ++i) {
      char ch = expr[i];
      if (ch == '(' || ch == ')') {
        if (! cur.empty ()) { toks.push_back (cur); cur.clear (); }
        toks.push_back (std::string (1, ch));
      } else if (std::isspace ((unsigned char) ch)) {
        if (! cur.empty ()) { toks.push_back (cur); cur.clear (); }
      } else {
        cur += ch;
      }
    }
    if (! cur.empty ()) {
      toks.push_back (cur);
    }
  }

  size_t pos = 0;
  //  recursive-descent, left-to-right, matching the Python nested closures
  std::function<db::Region ()> expression;
  std::function<db::Region ()> atom = [&] () -> db::Region {
    if (pos < toks.size () && toks[pos] == "(") {
      pos += 1;
      db::Region v = expression ();
      if (pos < toks.size () && toks[pos] == ")") {
        pos += 1;
      }
      return v;
    }
    std::string t = toks[pos];
    pos += 1;
    used.push_back (t);
    return resolve (t);                // a copy (dup)
  };
  auto upper = [] (std::string s) { for (char &c : s) c = (char) std::toupper ((unsigned char) c); return s; };
  expression = [&] () -> db::Region {
    db::Region val = atom ();
    while (pos < toks.size ()) {
      std::string u = upper (toks[pos]);
      if (u != "AND" && u != "OR" && u != "NOT" && u != "XOR") {
        break;
      }
      pos += 1;
      db::Region rhs = atom ();
      if (u == "AND") {
        val &= rhs;
      } else if (u == "OR") {
        val |= rhs;
      } else if (u == "NOT") {
        val -= rhs;
      } else {
        val ^= rhs;
      }
    }
    return val;
  };
  return expression ();
}

db::Region SVRFEngine::metric_select (const SVRFDerivation &d)
{
  db::Region r = resolve (d.operands[0]);
  bool has_lo = d.has_lo, has_hi = d.has_hi;
  double lo = d.lo, hi = d.hi;
  if (d.has_neq) {
    if (d.metric == "AREA") {
      db::Region::area_type n = (db::Region::area_type) std::llround (d.neq / (m_dbu * m_dbu));
      db::RegionAreaFilter f (n, n, true);
      return r.filtered (f);
    }
    db::Coord n = to_dbu (d.neq);
    db::RegionPerimeterFilter f (n, n, true);
    return r.filtered (f);
  }
  if (d.metric == "AREA") {
    db::Region::area_type amin = has_lo ? (db::Region::area_type) std::llround (lo / (m_dbu * m_dbu)) : 0;
    db::Region::area_type amax = has_hi ? (db::Region::area_type) std::llround (hi / (m_dbu * m_dbu)) : std::numeric_limits<db::Region::area_type>::max ();
    db::RegionAreaFilter f (amin, amax, false);
    return r.filtered (f);
  }
  if (d.metric == "PERIMETER") {
    db::RegionPerimeterFilter::perimeter_type pmin = has_lo ? (db::RegionPerimeterFilter::perimeter_type) to_dbu (lo) : 0;
    db::RegionPerimeterFilter::perimeter_type pmax = has_hi ? (db::RegionPerimeterFilter::perimeter_type) to_dbu (hi) : std::numeric_limits<db::RegionPerimeterFilter::perimeter_type>::max ();
    db::RegionPerimeterFilter f (pmin, pmax, false);
    return r.filtered (f);
  }
  //  LENGTH / ANGLE are edge-typed and never reach here
  return db::Region ();
}

db::Region SVRFEngine::rectangles_of (const SVRFDerivation &d)
{
  db::RectangleFilter rf (false /*is_square*/, false /*inverse*/);
  db::Region r = resolve (d.operands[0]).filtered (rf);
  std::map<std::string, std::string>::const_iterator w = d.params.find ("w");
  //  params carry bbox bounds as "lo:hi" (empty side == None); see parser
  auto parse_bounds = [] (const std::string &s, bool &has_lo, double &lo, bool &has_hi, double &hi) {
    has_lo = has_hi = false; lo = hi = 0.0;
    std::string::size_type c = s.find (':');
    std::string a = c == std::string::npos ? s : s.substr (0, c);
    std::string b = c == std::string::npos ? std::string () : s.substr (c + 1);
    if (! a.empty ()) { has_lo = true; lo = std::atof (a.c_str ()); }
    if (! b.empty ()) { has_hi = true; hi = std::atof (b.c_str ()); }
  };
  if (w != d.params.end ()) {
    bool hl, hh; double lo, hi; parse_bounds (w->second, hl, lo, hh, hi);
    db::RegionBBoxFilter::value_type vmin = hl ? (db::RegionBBoxFilter::value_type) to_dbu (lo) : 0;
    db::RegionBBoxFilter::value_type vmax = hh ? (db::RegionBBoxFilter::value_type) to_dbu (hi) : std::numeric_limits<db::RegionBBoxFilter::value_type>::max ();
    db::RegionBBoxFilter f (vmin, vmax, false, db::RegionBBoxFilter::BoxWidth);
    r = r.filtered (f);
  }
  std::map<std::string, std::string>::const_iterator h = d.params.find ("h");
  if (h != d.params.end ()) {
    bool hl, hh; double lo, hi; parse_bounds (h->second, hl, lo, hh, hi);
    db::RegionBBoxFilter::value_type vmin = hl ? (db::RegionBBoxFilter::value_type) to_dbu (lo) : 0;
    db::RegionBBoxFilter::value_type vmax = hh ? (db::RegionBBoxFilter::value_type) to_dbu (hi) : std::numeric_limits<db::RegionBBoxFilter::value_type>::max ();
    db::RegionBBoxFilter f (vmin, vmax, false, db::RegionBBoxFilter::BoxHeight);
    r = r.filtered (f);
  }
  std::map<std::string, std::string>::const_iterator a = d.params.find ("aspect");
  if (a != d.params.end ()) {
    bool hl, hh; double lo, hi; parse_bounds (a->second, hl, lo, hh, hi);
    db::RegionRatioFilter f (hl ? lo : 0.0, true, hh ? hi : std::numeric_limits<double>::max (), true, false, db::RegionRatioFilter::AspectRatio);
    r = r.filtered (f);
  }
  return r;
}

db::Region SVRFEngine::vertex_of (const SVRFDerivation &d)
{
  bool has_lo = d.has_lo, has_hi = d.has_hi;
  long lo = (long) d.lo, hi = (long) d.hi;
  db::Region src = resolve (d.operands[0]);
  db::Region out;
  for (db::Region::const_iterator p = src.begin (); ! p.at_end (); ++p) {
    //  total point count (hull + holes), matching pya Polygon.num_points()
    long npts = (long) p->vertices ();
    for (size_t hi_i = 0; hi_i < p->holes (); ++hi_i) {
      npts += (long) p->contour (int (hi_i + 1)).size ();
    }
    if ((! has_lo || npts >= lo) && (! has_hi || npts <= hi)) {
      out.insert (*p);
    }
  }
  return out;
}

db::Edges SVRFEngine::coincident_edges (const db::Edges &ea, const db::Edges &eb, int want)
{
  db::Edges coin = ea & eb;
  if (want < 0 || coin.count () == 0) {
    return coin;
  }
  if (ea.count () > 40000 || eb.count () > 40000) {
    return coin;                       // conservative superset (too large to split)
  }
  std::vector<db::Edge> bedges;
  for (db::Edges::const_iterator e = eb.begin (); ! e.at_end (); ++e) {
    bedges.push_back (*e);
  }
  db::Edges out;
  for (db::Edges::const_iterator s = coin.begin (); ! s.at_end (); ++s) {
    db::Edge se (s->p1 (), s->p2 ());
    for (std::vector<db::Edge>::iterator be = bedges.begin (); be != bedges.end (); ++be) {
      if (be->contains (se.p1 ()) && be->contains (se.p2 ())) {
        bool same = (be->dx () * se.dx () + be->dy () * se.dy ()) > 0;
        if (same == (want == 1)) {
          out.insert (*s);
        }
        break;
      }
    }
  }
  return out;
}

db::Edges SVRFEngine::build_edges (const SVRFDerivation &d)
{
  if (d.kind == "metric_select") {
    db::Edges e = as_edges (d.operands[0]);
    bool has_lo = d.has_lo, has_hi = d.has_hi;
    double lo = d.lo, hi = d.hi;
    //  SVRF metric negation + bound strictness (compiler folds `NOT ANGLE
    //  X >0 <90` into negate=1 + strict bounds). Ignoring either turned
    //  that construct into the strictly-inside-(0,90) selection — the EMPTY
    //  set on a rectilinear layout (commercial-PDK a contact-orientation rule collapse).
    bool negate = pflag (d.params, "negate");
    bool lo_strict = pflag (d.params, "lo_strict");
    bool hi_strict = pflag (d.params, "hi_strict");
    if (d.metric == "LENGTH") {
      if (d.has_neq) {
        db::EdgeLengthFilter f (to_dbu (d.neq), to_dbu (d.neq), !negate);
        return e.filtered (f);
      }
      if (has_lo && has_hi && lo == hi) {
        db::EdgeLengthFilter f (to_dbu (lo), to_dbu (lo), negate);
        return e.filtered (f);
      }
      db::EdgeLengthFilter::length_type lmin = has_lo ? (db::EdgeLengthFilter::length_type) to_dbu (lo) : 0;
      db::EdgeLengthFilter::length_type lmax = has_hi ? (db::EdgeLengthFilter::length_type) to_dbu (hi) : std::numeric_limits<db::EdgeLengthFilter::length_type>::max ();
      if (has_lo && lo_strict) { lmin += 1; }
      if (has_hi && hi_strict && lmax > 0) { /* lmax is exclusive already */ }
      db::EdgeLengthFilter f (lmin, lmax, negate);
      return e.filtered (f);
    }
    //  ANGLE (degrees, undirected 0..180) -> absolute orientation filter
    if (d.has_neq) {
      db::EdgeOrientationFilter f (d.neq, !negate /*inverse*/, true /*absolute*/);
      return e.filtered (f);
    }
    if (has_lo && has_hi && lo == hi) {
      db::EdgeOrientationFilter f (lo, negate, true);
      return e.filtered (f);
    }
    db::EdgeOrientationFilter f (has_lo ? lo : 0.0, !(has_lo && lo_strict),
                                 has_hi ? hi : 90.0, !(has_hi && hi_strict),
                                 negate, true);
    return e.filtered (f);
  }

  //  kind == "edge"
  std::string op = d.select_op.empty () ? std::string ("EDGE") : d.select_op;
  for (char &c : op) c = (char) std::toupper ((unsigned char) c);
  db::Edges a = as_edges (d.operands[0]);
  bool have_b = d.operands.size () > 1;
  bool b_is_edge = have_b && m_edge_layers.count (d.operands[1]) > 0;

  if (op == "INSIDE" && have_b) {
    return a.inside_part (resolve (d.operands[1]));
  }
  if (op == "OUTSIDE" && have_b) {
    //  Calibre OUTSIDE EDGE excludes edges COINCIDENT with b's boundary:
    //  an edge lying on b's border is neither inside nor outside. KLayout's
    //  outside_part keeps a boundary edge when its material faces away from
    //  b (e.g. an abutting NACT/PACT interface at the shared boundary),
    //  which flooded opposite-active spacing checks (commercial-PDK an active-spacing rule.*).
    //  Subtract the boundary-coincident parts explicitly.
    db::Region rb = resolve (d.operands[1]);
    return a.outside_part (rb) - rb.edges ();
  }
  if (op == "TOUCH" && have_b) {
    return a.selected_interacting (resolve (d.operands[1]));
  }
  if (op == "COINCIDENT" || op == "COIN") {
    if (! have_b) {
      return a;
    }
    db::Edges be = b_is_edge ? as_edges (d.operands[1]) : resolve (d.operands[1]).edges ();
    int want = pflag (d.params, "inside") ? 1 : (pflag (d.params, "outside") ? 0 : -1);
    return coincident_edges (a, be, want);
  }
  //  plain EDGE / INNER EDGE / OUTER EDGE
  db::Region base = resolve (d.operands[0]);
  if (pflag (d.params, "inner")) {
    return base.holes ().edges ();
  }
  if (pflag (d.params, "outer")) {
    return base.hulls ().edges ();
  }
  return base.edges ();
}

void SVRFEngine::exec_derivation (const SVRFDerivation &d)
{
  if (! d.supported) {
    m_unmodeled.insert (d.name);
    m_regions[d.name] = db::Region ();
    //  SVRFDRC_DEBUG=1: name every unsupported derivation + the parser's
    //  reason on stderr — the only way to see WHY a chain collapsed without
    //  bisecting probe decks (this diagnosis cost hours blind).
    if (getenv ("SVRFDRC_DEBUG")) {
      fprintf (stderr, "svrfdrc: UNMODELED %s (%s) expr=%s\n",
               d.name.c_str (), d.reason.c_str (), d.expr.c_str ());
    }
    return;
  }
  if (d.edge_typed) {
    try {
      m_edges_ns[d.name] = build_edges (d);
      m_edge_layers.insert (d.name);
      m_regions[d.name] = db::Region ();     // placeholder for region-typed consumers
    } catch (...) {
      m_unmodeled.insert (d.name);
      m_regions[d.name] = db::Region ();
    }
    return;
  }
  try {
    const std::string &k = d.kind;
    if (k == "bool_expr") {
      std::vector<std::string> used;
      db::Region reg = eval_bool_expr (d.expr, used);
      m_regions[d.name] = reg;
      for (std::vector<std::string>::iterator u = used.begin (); u != used.end (); ++u) {
        if (m_unmodeled.count (*u)) { m_unmodeled.insert (d.name); break; }
      }
    } else if (k == "bool") {
      db::Region acc = resolve (d.operands[0]);
      for (size_t i = 1; i < d.operands.size (); ++i) {
        db::Region &r = resolve (d.operands[i]);
        if (d.bool_sym == "&") acc &= r;
        else if (d.bool_sym == "|") acc |= r;
        else if (d.bool_sym == "-") acc -= r;
        else if (d.bool_sym == "^") acc ^= r;
      }
      m_regions[d.name] = acc;
    } else if (k == "size") {
      auto it_dir = d.params.find ("dir");
      if (it_dir != d.params.end ()) {
        //  One-sided size (SHRINK/GROW <layer> RIGHT|LEFT|TOP|BOTTOM BY w):
        //  exactly erosion/dilation by an off-center axis-aligned segment =
        //  half-size on that axis + a translation that pins the far side.
        //  With signed v (negative for SHRINK), h = v/2, t = v - h:
        //    RIGHT: sized(h,0) then move(+t,0)   LEFT:   ... move(-t,0)
        //    TOP:   sized(0,h) then move(0,+t)   BOTTOM: ... move(0,-t)
        //  (worked example, SHRINK RIGHT BY 5: [x0,x1] -> sized -2.5 ->
        //   [x0+2.5, x1-2.5] -> move -2.5 -> [x0, x1-5]; far side pinned.)
        //  Sequential R,L,T,B one-sided shrinks compose to the isotropic
        //  size — Calibre's wide-metal derivation idiom relies on this;
        //  treating the qualifier as isotropic corrupted every
        //  wide-metal chains chain (commercial-PDK a metal1-spacing rule/a wide-metal spacing rule phantoms).
        //  possibly a NESTED one-line chain (SHRINK(SHRINK(...R 5) L 5)...):
        //  apply every (dir, value) pair in token order (innermost-out).
        std::vector<std::string> dirs;
        std::vector<double> vals;
        {
          std::stringstream ds (it_dir->second);
          std::string tok;
          while (std::getline (ds, tok, ',')) { dirs.push_back (tok); }
          auto it_v = d.params.find ("dir_vals");
          if (it_v != d.params.end ()) {
            std::stringstream vs (it_v->second);
            while (std::getline (vs, tok, ',')) { vals.push_back (std::atof (tok.c_str ())); }
          }
        }
        db::Region r = resolve (d.operands[0]);
        for (size_t i = 0; i < dirs.size (); ++i) {
          double vv = (i < vals.size ()) ? vals[i] : (d.has_value ? d.value : 0.0);
          db::Coord v = to_dbu (vv);                           // signed
          db::Coord h = v / 2;
          db::Coord t = v - h;                                 // dbu-exact split
          const std::string &dir = dirs[i];
          if (dir == "RIGHT") {
            r = r.sized (h, 0); r.transform (db::Disp (db::Vector (t, 0)));
          } else if (dir == "LEFT") {
            r = r.sized (h, 0); r.transform (db::Disp (db::Vector (-t, 0)));
          } else if (dir == "TOP") {
            r = r.sized (0, h); r.transform (db::Disp (db::Vector (0, t)));
          } else {  // BOTTOM
            r = r.sized (0, h); r.transform (db::Disp (db::Vector (0, -t)));
          }
        }
        m_regions[d.name] = r;
      } else {
        auto it_morph = d.params.find ("morph");
        if (it_morph != d.params.end () && d.has_value) {
          //  OVERUNDER = close (grow then shrink); UNDEROVER = open
          //  (shrink then grow). d.value is +ve for SIZE ... BY d; the
          //  two-step derives dense-array cores / removes thin necks.
          db::Coord dd = to_dbu (std::abs (d.value));
          db::Region r = resolve (d.operands[0]);
          if (it_morph->second == "OVERUNDER") {
            r = r.sized (dd); r = r.sized (-dd);
          } else {  // UNDEROVER
            r = r.sized (-dd); r = r.sized (dd);
          }
          m_regions[d.name] = r;
        } else {
          m_regions[d.name] = resolve (d.operands[0]).sized (to_dbu (d.has_value ? d.value : 0.0));
        }
      }
    } else if (k == "select") {
      db::Region a = resolve (d.operands[0]);
      db::Region b = d.operands.size () > 1 ? resolve (d.operands[1]) : db::Region ();
      std::string op = d.select_op;
      for (char &c : op) c = (char) std::toupper ((unsigned char) c);
      bool negate = pflag (d.params, "negate");
      db::Region res;
      if (op == "INSIDE") {
        res = negate ? a.selected_not_inside (b) : a.selected_inside (b);
      } else if (op == "OUTSIDE") {
        res = negate ? a.selected_not_outside (b) : a.selected_outside (b);
      } else {
        //  INTERACT / CUT / TOUCH / ENCLOSE all map to interacting in the reference.
        //  SVRF interaction-count qualifier (INTERACT A B ==N / >N / <N): fold
        //  the compiler's count params into KLayout's counted overload; strict
        //  integral bounds shift by one (>N -> min N+1, <N -> max N-1).
        size_t cmin = 1;
        size_t cmax = std::numeric_limits<size_t>::max ();
        auto it_lo = d.params.find ("count_lo");
        auto it_hi = d.params.find ("count_hi");
        if (it_lo != d.params.end ()) {
          cmin = (size_t) std::strtoull (it_lo->second.c_str (), 0, 10);
          if (pflag (d.params, "count_lo_strict")) { cmin += 1; }
        }
        if (it_hi != d.params.end ()) {
          cmax = (size_t) std::strtoull (it_hi->second.c_str (), 0, 10);
          if (pflag (d.params, "count_hi_strict") && cmax > 0) { cmax -= 1; }
        }
        if (cmin != 1 || cmax != std::numeric_limits<size_t>::max ()) {
          //  Calibre counts the OTHER layer's ORIGINAL polygons; KLayout's
          //  merged semantics unions corner-touching polygons first (the 4
          //  EXPAND-EDGE strips of a square become ONE ring -> count 1 and
          //  ==4 never matches). Count against the raw polygons.
          db::Region braw (b);
          braw.set_merged_semantics (false);
          res = negate ? a.selected_not_interacting (braw, cmin, cmax)
                       : a.selected_interacting (braw, cmin, cmax);
        } else {
          res = negate ? a.selected_not_interacting (b, cmin, cmax)
                       : a.selected_interacting (b, cmin, cmax);
        }
      }
      m_regions[d.name] = res;
    } else if (k == "passthrough") {
      m_regions[d.name] = resolve (d.operands[0]);
    } else if (k == "empty") {
      m_regions[d.name] = db::Region ();
    } else if (k == "holes") {
      m_regions[d.name] = resolve (d.operands[0]).holes ();
    } else if (k == "rectangles") {
      m_regions[d.name] = rectangles_of (d);
    } else if (k == "layout_extent") {
      //  nullary EXTENT = the LAYOUT extent (bbox of the top cell over all
      //  layers). Distinct from the per-shape EXTENTS op below. commercial-PDK's
      //  SUB=EXTENT seeds the whole BULK/LV context tree from this.
      m_regions[d.name] = db::Region (m_layout.cell (m_top).bbox ());
    } else if (k == "extents") {
      m_regions[d.name] = resolve (d.operands[0]).processed (db::extents_processor<db::Polygon> (0, 0));
    } else if (k == "merge") {
      m_regions[d.name] = resolve (d.operands[0]).merged ();
    } else if (k == "vertex") {
      m_regions[d.name] = vertex_of (d);
    } else if (k == "with_edge") {
      db::Region a = resolve (d.operands[0]);
      db::Region b = d.operands.size () > 1 ? resolve (d.operands[1]) : db::Region ();
      m_regions[d.name] = a.selected_interacting (b.edges ());
    } else if (k == "expand") {
      //  the operand may be an EDGE-typed derivation (e.g. an ANGLE
      //  selection): resolve() only consults the REGION table, so an edge
      //  operand silently became empty and every EXPAND EDGE chain
      //  collapsed (commercial-PDK a contact-orientation rule). as_edges() consults the edge table
      //  first and falls back to region.edges().
      db::Edges edges = as_edges (d.operands[0]);
      db::Coord w = to_dbu (std::fabs (d.has_value ? d.value : 0.0));
      db::Region ex;
      if (pflag (d.params, "inside")) {
        edges.extended (ex, 0, 0, 0, w, false);        // extended_in
      } else if (pflag (d.params, "outside")) {
        edges.extended (ex, 0, 0, w, 0, false);        // extended_out
      } else {
        edges.extended (ex, 0, 0, w, w, false);
      }
      m_regions[d.name] = ex;
    } else if (k == "metric_select") {
      m_regions[d.name] = metric_select (d);
    } else if (k == "net_ratio") {
      m_regions[d.name] = net_area_ratio (d);
    } else {
      m_unmodeled.insert (d.name);
      m_regions[d.name] = db::Region ();
    }
  } catch (...) {
    //  an unsupported/failed derivation -> empty region + unmodeled (dependent
    //  rules honestly SKIP rather than false-PASS)
    m_unmodeled.insert (d.name);
    m_regions[d.name] = db::Region ();
  }
}

// ---------------------------------------------------------------------------
//  check option builders
// ---------------------------------------------------------------------------

db::RegionCheckOptions SVRFEngine::check_options (const SVRFRule &r, bool allow_filters) const
{
  db::RegionCheckOptions o;                 // defaults: whole_edges=false, metrics=Euclidian,
                                            // ignore_angle=90, min_proj=0, max_proj=max, shielded=true
  o.metrics = map_metrics (r.metrics);
  if (r.has_ignore_angle) {
    o.ignore_angle = r.ignore_angle;
  }
  if (r.whole_edges) {
    o.whole_edges = true;
  }
  if (r.has_min_projection) {
    o.min_projection = to_dbu (r.min_projection);
  }
  if (r.has_max_projection) {
    o.max_projection = to_dbu (r.max_projection);
  }
  if (r.has_shielded) {
    o.shielded = r.shielded;
  }
  if (allow_filters && r.opposite) {
    o.opposite_filter = db::OnlyOpposite;
  }
  return o;
}

db::EdgesCheckOptions SVRFEngine::edge_check_options (const SVRFRule &r) const
{
  db::EdgesCheckOptions o;
  o.metrics = map_metrics (r.metrics);
  if (r.has_ignore_angle) {
    o.ignore_angle = r.ignore_angle;
  }
  if (r.whole_edges) {
    o.whole_edges = true;
  }
  if (r.has_min_projection) {
    o.min_projection = to_dbu (r.min_projection);
  }
  if (r.has_max_projection) {
    o.max_projection = to_dbu (r.max_projection);
  }
  return o;
}

//  Calibre EXT/INT/ENC default: COINCIDENT (collinear-overlapping, "flush")
//  edge pairs are TOUCHING, not spacing violations — abutting-cell implant /
//  act / met flush boundaries are legal. KLayout's checks report them at
//  distance 0. Drop pairs whose edges are parallel AND intersecting (parallel
//  edges can only intersect when collinear-overlapping); endpoint abutments
//  at an angle stay (that is what ABUT<n / ignore_angle governs).
//  On the commercial-PDK full-FEOL spm GDS this phantom class alone accounted for
//  the per-cell-count families (imp enclosure x1069, NPSD/PPSD waves, ...).
static db::EdgePairs drop_coincident_pairs (const db::EdgePairs &ep)
{
  db::EdgePairs out;
  for (auto p = ep.begin (); ! p.at_end (); ++p) {
    const db::Edge &a = (*p).first ();
    const db::Edge &b = (*p).second ();
    bool parallel = ((long long) a.dx () * (long long) b.dy ()
                     - (long long) a.dy () * (long long) b.dx ()) == 0;
    if (parallel && a.intersect (b)) {
      continue;                       //  flush/coincident touching — legal
    }
    out.insert (*p);
  }
  return out;
}

bool SVRFEngine::inputs_unmodeled (const SVRFRule &r) const
{
  if (! r.layer1.empty () && m_unmodeled.count (r.layer1)) {
    return true;
  }
  if (! r.layer2.empty () && m_unmodeled.count (r.layer2)) {
    return true;
  }
  return false;
}

bool SVRFEngine::is_edge_rule (const SVRFRule &r) const
{
  if (m_edge_layers.count (r.layer1)) {
    return true;
  }
  if (! r.layer2.empty () && m_edge_layers.count (r.layer2)) {
    return true;
  }
  return false;
}

// ---------------------------------------------------------------------------
//  edge-typed rule dispatch
// ---------------------------------------------------------------------------

void SVRFEngine::exec_edge_rule (const SVRFRule &r)
{
  if (r.op != "EXTERNAL" && r.op != "INTERNAL" && r.op != "ENCLOSURE") {
    SVRFResult res; res.verdict = "SKIP"; res.rule = &r;
    res.info = "edge op " + r.op + " has no Edges check";
    m_results.push_back (res);
    return;
  }
  db::Coord d = to_dbu (r.value);
  db::Edges e1 = as_edges (r.layer1);
  bool ok = false;
  db::EdgePairs ep;
  try {
    db::EdgesCheckOptions o = edge_check_options (r);
    if (r.op == "EXTERNAL") {
      ep = r.layer2.empty () ? e1.space_check (d, o) : e1.separation_check (as_edges (r.layer2), d, o);
    } else if (r.op == "INTERNAL") {
      ep = r.layer2.empty () ? e1.width_check (d, o) : e1.overlap_check (as_edges (r.layer2), d, o);
    } else {  // ENCLOSURE
      db::Edges outer = r.layer2.empty () ? db::Edges () : as_edges (r.layer2);
      ep = outer.enclosing_check (e1, d, o);
    }
    ok = true;
  } catch (...) {
    ok = false;
  }
  if (! ok) {
    SVRFResult res; res.verdict = "SKIP"; res.rule = &r;
    res.info = "edge-check unsupported";
    m_results.push_back (res);
    return;
  }
  ep = drop_coincident_pairs (ep);
  size_t cnt = ep.count ();
  db::Region errpoly;
  ep.polygons (errpoly);
  //  SVRFDRC_VIOBBOX=1: dump the first violation bboxes (um) per failing
  //  rule to stderr — parity triage needs a LOCATION to inspect, not a tally.
  if (cnt > 0 && getenv ("SVRFDRC_VIOBBOX")) {
    int nb = 0;
    for (auto p = ep.begin (); ! p.at_end () && nb < 5; ++p, ++nb) {
      const db::Edge &ea = (*p).first ();
      const db::Edge &eb = (*p).second ();
      fprintf (stderr, "VIOPAIR %s A(%.3f,%.3f)-(%.3f,%.3f) B(%.3f,%.3f)-(%.3f,%.3f)\n",
               r.name.c_str (),
               ea.p1 ().x () * m_dbu, ea.p1 ().y () * m_dbu, ea.p2 ().x () * m_dbu, ea.p2 ().y () * m_dbu,
               eb.p1 ().x () * m_dbu, eb.p1 ().y () * m_dbu, eb.p2 ().x () * m_dbu, eb.p2 ().y () * m_dbu);
    }
    nb = 0;
    for (db::Region::const_iterator vp = errpoly.begin (); ! vp.at_end () && nb < 5; ++vp, ++nb) {
      db::Box bx = (*vp).box ();
      fprintf (stderr, "VIOBBOX %s %.3f %.3f %.3f %.3f\n", r.name.c_str (),
               bx.left () * m_dbu, bx.bottom () * m_dbu,
               bx.right () * m_dbu, bx.top () * m_dbu);
    }
  }
  m_regions[r.name] = errpoly;
  SVRFResult res; res.rule = &r;
  res.verdict = cnt == 0 ? "PASS" : "FAIL";
  res.info = std::to_string (cnt);
  m_results.push_back (res);
}

// ---------------------------------------------------------------------------
//  density windowing
// ---------------------------------------------------------------------------

void SVRFEngine::exec_density (const SVRFRule &r)
{
  db::Region reg = resolve (r.layer1);
  if (! r.layer2.empty ()) {
    reg = reg | resolve (r.layer2);
  }
  db::Box bb = reg.bbox ();
  if (bb.empty ()) {
    SVRFResult res; res.rule = &r; res.verdict = "PASS"; res.info = "0";
    m_results.push_back (res);
    return;
  }
  double thr = r.value;
  const std::string &cmp = r.cmp;
  auto viol = [&] (double dens) -> bool {
    if (cmp == "<")  return dens < thr;
    if (cmp == "<=") return dens <= thr;
    if (cmp == ">")  return dens > thr;
    if (cmp == ">=") return dens >= thr;
    if (cmp == "==") return dens == thr;
    return false;
  };

  std::vector<db::Box> windows;
  db::Coord W = r.has_window ? to_dbu (r.window) : 0;
  if (W <= 0) {
    windows.push_back (bb);
  } else {
    db::Coord s = r.has_step ? to_dbu (r.step) : W;
    if (s <= 0) {
      s = W;
    }
    while (((long long) (bb.width () / s) + 1) * ((long long) (bb.height () / s) + 1) > 20000) {
      s *= 2;
    }
    for (db::Coord y = bb.bottom (); y < bb.top (); y += s) {
      for (db::Coord x = bb.left (); x < bb.right (); x += s) {
        windows.push_back (db::Box (x, y, x + W, y + W));
      }
    }
  }
  db::Region bad;
  for (std::vector<db::Box>::iterator w = windows.begin (); w != windows.end (); ++w) {
    double area = (double) w->area ();
    if (area <= 0) {
      continue;
    }
    double covered = (double) (reg & db::Region (*w)).area ();
    if (viol (covered / area)) {
      bad.insert (*w);
    }
  }
  size_t cnt = bad.count ();
  m_regions[r.name] = bad;
  SVRFResult res; res.rule = &r;
  res.verdict = cnt == 0 ? "PASS" : "FAIL";
  res.info = std::to_string (cnt);
  m_results.push_back (res);
}

// ---------------------------------------------------------------------------
//  measurement rule dispatch
// ---------------------------------------------------------------------------

void SVRFEngine::exec_rule (const SVRFRule &r)
{
  //  COPY: report a previously-computed error layer (foundry rule naming)
  if (r.op == "COPY") {
    const std::string &src = r.layer1;
    if (m_unmodeled.count (src)) {           // antenna / net-ratio etc: honest SKIP, never PASS
      SVRFResult res; res.rule = &r; res.verdict = "SKIP";
      res.info = "errlayer '" + src + "' routed to dedicated checker";
      m_results.push_back (res);
      return;
    }
    std::map<std::string, db::Region>::iterator it = m_regions.find (src);
    db::Region reg;
    if (it == m_regions.end ()) {            // COPY of a plain drawn layer -> report its shapes
      if (! drawn (src, reg)) {
        SVRFResult res; res.rule = &r; res.verdict = "SKIP";
        res.info = "errlayer '" + src + "' unresolved";
        m_results.push_back (res);
        return;
      }
      m_regions[src] = reg;
    } else {
      reg = it->second;
    }
    size_t c = reg.count ();
    m_regions[r.name] = reg;
    SVRFResult res; res.rule = &r;
    res.verdict = c == 0 ? "PASS" : "FAIL";
    res.info = std::to_string (c);
    m_results.push_back (res);
    return;
  }

  if (! r.supported) {
    SVRFResult res; res.rule = &r; res.verdict = "SKIP";
    res.info = r.reason.empty () ? "unsupported" : r.reason;
    m_results.push_back (res);
    return;
  }
  if (inputs_unmodeled (r)) {
    SVRFResult res; res.rule = &r; res.verdict = "SKIP";
    res.info = "input layer is edge-typed/unmodeled";
    m_results.push_back (res);
    return;
  }
  if (is_edge_rule (r)) {
    exec_edge_rule (r);
    return;
  }
  if (r.op == "DENSITY") {
    exec_density (r);
    return;
  }

  db::Coord d = to_dbu (r.value);
  db::EdgePairs ep;
  bool have_ep = false;
  db::Region viol;
  try {
    if (r.connectivity != SVRFConnectivity::none) {
      if (m_deck.connects.empty ()) {
        SVRFResult res; res.rule = &r; res.verdict = "SKIP";
        res.info = "connectivity but no CONNECT stack";
        m_results.push_back (res);
        return;
      }
      if (r.op != "EXTERNAL" || r.layer2.empty ()) {
        SVRFResult res; res.rule = &r; res.verdict = "SKIP";
        res.info = "connectivity needs 2-layer EXTERNAL";
        m_results.push_back (res);
        return;
      }
      build_l2n ();
      if (! m_l2n_layers.count (r.layer1) || ! m_l2n_layers.count (r.layer2)) {
        SVRFResult res; res.rule = &r; res.verdict = "SKIP";
        res.info = "net layer has no extracted shapes";
        m_results.push_back (res);
        return;
      }
      db::RegionCheckOptions o = check_options (r);
      o.prop_constraint = (r.connectivity == SVRFConnectivity::same) ? db::SamePropertiesConstraint : db::DifferentPropertiesConstraint;
      db::Region n1 = m_l2n_layers[r.layer1].nets (*m_l2n, db::NPM_NetQualifiedNameOnly, tl::Variant ("net"), 0);
      db::Region n2 = m_l2n_layers[r.layer2].nets (*m_l2n, db::NPM_NetQualifiedNameOnly, tl::Variant ("net"), 0);
      ep = n1.separation_check (n2, d, o);
      have_ep = true;
      ep.polygons (viol);
    } else if (r.op == "AREA") {
      db::Region::area_type area_dbu = (db::Region::area_type) std::llround (r.value / (m_dbu * m_dbu));
      db::RegionAreaFilter f (0, area_dbu, false);
      viol = resolve (r.layer1).filtered (f);
      have_ep = false;
    } else {
      db::Region l1 = resolve (r.layer1);
      if (r.op == "EXTERNAL") {
        db::RegionCheckOptions o = check_options (r);
        ep = r.layer2.empty () ? l1.space_check (d, o) : l1.separation_check (resolve (r.layer2), d, o);
        have_ep = true;
      } else if (r.op == "INTERNAL") {
        if (r.layer2.empty ()) {
          ep = l1.width_check (d, check_options (r, false));
        } else {
          ep = l1.overlap_check (resolve (r.layer2), d, check_options (r));
        }
        have_ep = true;
      } else if (r.op == "NOTCH") {
        ep = l1.notch_check (d, check_options (r, false));
        have_ep = true;
      } else if (r.op == "ENCLOSURE") {
        db::Region outer = r.layer2.empty () ? db::Region () : resolve (r.layer2);
        ep = outer.enclosing_check (l1, d, check_options (r));
        have_ep = true;
      } else {
        SVRFResult res; res.rule = &r; res.verdict = "SKIP";
        res.info = "op " + r.op + " not in core";
        m_results.push_back (res);
        return;
      }
      if (have_ep) {
        ep.polygons (viol);
      }
    }
  } catch (tl::Exception &e) {
    SVRFResult res; res.rule = &r; res.verdict = "ERROR";
    std::string m = e.msg (); if (m.size () > 80) m = m.substr (0, 80);
    res.info = m;
    m_results.push_back (res);
    return;
  } catch (...) {
    SVRFResult res; res.rule = &r; res.verdict = "ERROR";
    res.info = "check error";
    m_results.push_back (res);
    return;
  }
  if (have_ep) {
    ep = drop_coincident_pairs (ep);
    viol.clear ();
    ep.polygons (viol);
  }
  size_t cnt = have_ep ? ep.count () : viol.count ();
  if (cnt > 0 && getenv ("SVRFDRC_VIOBBOX")) {
    int nb = 0;
    for (db::Region::const_iterator vp = viol.begin (); ! vp.at_end () && nb < 5; ++vp, ++nb) {
      db::Box bx = (*vp).box ();
      fprintf (stderr, "VIOBBOX %s %.3f %.3f %.3f %.3f\n", r.name.c_str (),
               bx.left () * m_dbu, bx.bottom () * m_dbu,
               bx.right () * m_dbu, bx.top () * m_dbu);
    }
  }
  m_regions[r.name] = viol;              // this measurement IS an error layer
  SVRFResult res; res.rule = &r;
  res.verdict = cnt == 0 ? "PASS" : "FAIL";
  res.info = std::to_string (cnt);
  m_results.push_back (res);
}

// ---------------------------------------------------------------------------
//  connectivity (net extraction)
// ---------------------------------------------------------------------------

void SVRFEngine::build_l2n ()
{
  if (m_l2n_built) {
    return;
  }
  m_l2n_built = true;
  m_l2n.reset (new db::LayoutToNetlist ("TOP", m_dbu));

  //  register + connect helper. m_l2n_layers keeps the registered Region copies so
  //  Region::nets(...) can later re-query them (like the Python `reg` dict).
  auto get = [&] (const std::string &nm) -> db::Region & {
    std::map<std::string, db::Region>::iterator it = m_l2n_layers.find (nm);
    if (it != m_l2n_layers.end ()) {
      return it->second;
    }
    db::Region r = resolve (nm);
    m_l2n_layers[nm] = r;
    db::Region &ref = m_l2n_layers[nm];
    m_l2n->register_layer (ref, nm);
    m_l2n->connect (ref);              // make it a net-bearing layer
    return ref;
  };

  for (std::vector<std::tuple<std::string, std::string, std::string> >::const_iterator c = m_deck.connects.begin (); c != m_deck.connects.end (); ++c) {
    db::Region &ra = get (std::get<0> (*c));
    db::Region &rb = get (std::get<1> (*c));
    const std::string &via = std::get<2> (*c);
    if (! via.empty ()) {
      db::Region &rv = get (via);
      m_l2n->connect (ra, rv);
      m_l2n->connect (rb, rv);
    } else {
      m_l2n->connect (ra, rb);
    }
  }
  //  register NET AREA RATIO operands so shapes_of_net can see them
  for (std::vector<SVRFDerivation>::const_iterator dv = m_deck.derivations.begin (); dv != m_deck.derivations.end (); ++dv) {
    if (dv->kind == "net_ratio" && dv->supported) {
      for (size_t i = 0; i < dv->operands.size () && i < 2; ++i) {
        get (dv->operands[i]);
      }
    }
  }
  //  register every layer a CONNECTED / NOT CONNECTED rule queries
  for (std::vector<SVRFRule>::const_iterator rl = m_deck.rules.begin (); rl != m_deck.rules.end (); ++rl) {
    if (rl->connectivity != SVRFConnectivity::none) {
      if (! rl->layer1.empty ()) get (rl->layer1);
      if (! rl->layer2.empty ()) get (rl->layer2);
    }
  }
  m_l2n->extract_netlist ();
}

db::Region SVRFEngine::net_area_ratio (const SVRFDerivation &d)
{
  const std::string &A = d.operands[0];
  const std::string &B = d.operands[1];
  build_l2n ();
  //  Use the REGISTERED region objects (references), not copies: shapes_of_net()
  //  resolves the layer via the DSS layer_for_flat map keyed on the region's
  //  delegate identity -- a copy has a different identity and would throw
  //  "Non-hierarchical layers cannot be used in netlist extraction". build_l2n()
  //  registers every net_ratio operand, so both are present; if somehow not, the
  //  ratio is unqueryable -> empty error layer (matches the reference no-violation).
  std::map<std::string, db::Region>::iterator ita = m_l2n_layers.find (A);
  std::map<std::string, db::Region>::iterator itb = m_l2n_layers.find (B);
  if (ita == m_l2n_layers.end () || itb == m_l2n_layers.end ()) {
    return db::Region ();
  }
  db::Region &ra = ita->second;
  db::Region &rb = itb->second;

  std::string cmp = "==";
  double thr = 0.0;
  std::map<std::string, std::string>::const_iterator pc = d.params.find ("cmp");
  if (pc != d.params.end ()) cmp = pc->second;
  std::map<std::string, std::string>::const_iterator pt = d.params.find ("thr");
  if (pt != d.params.end ()) thr = std::atof (pt->second.c_str ());
  auto viol = [&] (double x) -> bool {
    if (cmp == "<")  return x < thr;
    if (cmp == "<=") return x <= thr;
    if (cmp == ">")  return x > thr;
    if (cmp == ">=") return x >= thr;
    if (cmp == "==") return x == thr;
    if (cmp == "!=") return x != thr;
    return false;
  };

  db::Region out;
  db::Netlist *nl = m_l2n->netlist ();
  if (! nl) {
    return out;
  }
  for (db::Netlist::circuit_iterator c = nl->begin_circuits (); c != nl->end_circuits (); ++c) {
    for (db::Circuit::net_iterator net = c->begin_nets (); net != c->end_nets (); ++net) {
      std::unique_ptr<db::Region> sb (m_l2n->shapes_of_net (*net, rb, true));
      double ab = sb ? (double) sb->area () : 0.0;
      if (ab == 0.0) {
        continue;                        // B absent on this net: ratio undefined, skip
      }
      std::unique_ptr<db::Region> sa (m_l2n->shapes_of_net (*net, ra, true));
      double aa = sa ? (double) sa->area () : 0.0;
      if (viol (aa / ab)) {
        out += *sb;
      }
    }
  }
  return out;
}

// ---------------------------------------------------------------------------
//  report -- BYTE-IDENTICAL to run_svrf_drc.py::main()
// ---------------------------------------------------------------------------

void SVRFEngine::write_report (const std::string &report_path, const std::string &deck_path,
                               const std::string &layout_path, const std::string &klayout_version) const
{
  Tally tally;
  for (std::vector<SVRFResult>::const_iterator it = m_results.begin (); it != m_results.end (); ++it) {
    tally.bump (it->verdict);
  }

  std::ostringstream out;

  //  header line 1 -- ".rstrip()" of "# SVRF-native DRC via KLayout <ver>"
  {
    std::string h = "# SVRF-native DRC via KLayout " + klayout_version;
    while (! h.empty () && (h.back () == ' ' || h.back () == '\t')) {
      h.pop_back ();
    }
    out << h << "\n";
  }
  //  header line 2
  out << "# deck=" << deck_path << "  layout=" << layout_path << "  dbu=" << py_float_str (m_dbu) << "\n";
  //  header line 3
  out << "# " << m_deck.layers.size () << " layers, " << m_deck.derivations.size ()
      << " derivations, " << m_deck.rules.size () << " rules  |  " << tally.repr () << "\n";
  out << "\n";

  for (std::vector<SVRFResult>::const_iterator it = m_results.begin (); it != m_results.end (); ++it) {
    const SVRFRule &r = *it->rule;
    std::string tag;
    if (r.op != "COPY") {
      tag = "[metrics=" + r.metrics;
      if (r.has_ignore_angle) {
        tag += ",ignore_angle=" + py_float_str (r.ignore_angle);
      }
      if (r.opposite) {
        tag += ",opposite";
      }
      if (r.whole_edges) {
        tag += ",whole";
      }
      tag += "]";
    } else {
      tag = "[COPY]";
    }

    //  "{verdict:5s} {name:18s} {op} {layer1}{/layer2} {cmp} {value} {tag} -> {info}"
    std::string vpad = it->verdict;
    if (vpad.size () < 5) vpad.append (5 - vpad.size (), ' ');
    std::string npad = r.name;
    if (npad.size () < 18) npad.append (18 - npad.size (), ' ');

    out << vpad << " " << npad << " " << r.op << " " << r.layer1;
    if (! r.layer2.empty ()) {
      out << "/" << r.layer2;
    }
    out << " " << r.cmp << " " << py_float_str (r.value) << " " << tag << " -> " << it->info << "\n";
  }
  out << "\n";
  out << "# tally: " << tally.repr () << "\n";

  //  write the file (LF-terminated, matching Python "\n".join(...) + "\n")
  tl::OutputStream os (report_path);
  os << out.str ();
}

}  // namespace db
