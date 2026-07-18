
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
#include "dbPolygon.h"
#include "dbTrans.h"
#include "dbBox.h"

#include "tlStream.h"
#include "tlString.h"
#include "tlVariant.h"
#include "tlException.h"
#include "tlThreads.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <sstream>
#include <fstream>
#include <limits>
#include <atomic>
#include <functional>
#include <chrono>

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
  //  --- cell-aware FEOL exemption setup (fork fix #2) --------------------
  //  No-op (returns immediately, m_feol_enabled stays false) unless a config
  //  path was set via set_cell_aware_feol(). Runs before any rule so the shared
  //  footprint index is fully built (and never mutated) during the rule phase.
  setup_cell_aware_feol ();

  //  --- automated waiver management (#10) --------------------------------
  //  Load the pre-approved geometric waivers ONCE here (single-threaded) from
  //  $SVRFDRC_WAIVERS so the rule phase only READS m_waivers. No env => no-op
  //  (m_waivers_enabled stays false) => byte-identical to HEAD.
  load_waivers ();

  //  --- result-slot layout (frozen report order) -------------------------
  //  Report lines are: [ every non-COPY rule in source order ] followed by
  //  [ every COPY rule in source order ]. Assign each rule a fixed slot in that
  //  order and pre-size m_results, so every rule writes its own slot and the byte
  //  order of the report is independent of the execution order (inline vs worker).
  std::size_t n_noncopy = 0, n_copy = 0;
  for (std::vector<SVRFStatement>::const_iterator s = m_deck.statements.begin (); s != m_deck.statements.end (); ++s) {
    if (s->kind == SVRFStatement::Rule) {
      (m_deck.rules[s->index].op == "COPY" ? n_copy : n_noncopy) += 1;
    }
  }
  m_results.assign (n_noncopy + n_copy, SVRFResult ());

  //  --- reorder-safety map -----------------------------------------------
  //  A non-COPY measurement rule may only be deferred to the parallel phase if
  //  NO derivation LATER in source order consumes its output layer -- otherwise
  //  that later derivation must observe the rule's real error layer exactly as in
  //  the serial interleaving (the parser proved rules never consume other rule
  //  outputs, so a derivation is the only possible downstream consumer). Record,
  //  per referenced name, the highest statement index at which a derivation
  //  references it.
  //  Also count how many rules share each output name: a non-unique name would
  //  make two rules write the SAME m_regions[name] error layer, so such rules stay
  //  serial (preserving the source-order last-writer-wins) rather than racing.
  //  (Rule names are unique in the commercial deck, so this excludes nothing there --
  //  it is a correctness guard for arbitrary decks.)
  std::map<std::string, int> name_count;
  for (std::vector<SVRFRule>::const_iterator rr = m_deck.rules.begin (); rr != m_deck.rules.end (); ++rr) {
    if (rr->op != "COPY") {
      name_count[rr->name] += 1;
    }
  }

  std::map<std::string, std::size_t> last_deriv_ref;
  for (std::size_t i = 0; i < m_deck.statements.size (); ++i) {
    const SVRFStatement &st = m_deck.statements[i];
    if (st.kind != SVRFStatement::Derivation) {
      continue;
    }
    const SVRFDerivation &d = m_deck.derivations[st.index];
    std::set<std::string> refs (d.operands.begin (), d.operands.end ());
    {
      //  tokenize the boolean expr the same way eval_bool_expr does
      std::string cur;
      for (std::size_t k = 0; k < d.expr.size (); ++k) {
        char ch = d.expr[k];
        if (ch == '(' || ch == ')' || std::isspace ((unsigned char) ch)) {
          if (! cur.empty ()) { refs.insert (cur); cur.clear (); }
        } else {
          cur += ch;
        }
      }
      if (! cur.empty ()) { refs.insert (cur); }
    }
    for (std::set<std::string>::const_iterator r = refs.begin (); r != refs.end (); ++r) {
      last_deriv_ref[*r] = i;                      // i increases -> keeps the max
    }
  }

  //  --- pass 1 (serial, source order): derivations + main-thread rules ----
  //  Derivations build the flat-Region DAG (must stay serial). Rules that are NOT
  //  safe to parallelize (DENSITY / connectivity / consumed-by-a-later-derivation)
  //  run inline here, exactly at their source position. The rest are collected for
  //  the parallel phase; COPY is deferred to pass 3 (unchanged).
  //  SVRFDRC_TIMING=1: emit per-phase wall-clock to stderr (diagnostic only; does
  //  NOT touch the report). Lets the speedup be attributed to the parallel phase.
  const bool timing = (getenv ("SVRFDRC_TIMING") != 0);
  typedef std::chrono::steady_clock clk;
  clk::time_point t_pass1 = clk::now ();

  std::vector<std::pair<const SVRFRule *, std::size_t> > parallel;
  std::vector<std::pair<const SVRFRule *, std::size_t> > copies;
  if (m_threads > 1) {
    //  fork fix #3: topological-level DERIVATION parallelism. Fills `parallel` /
    //  `copies` with the same rule classification as the serial loop below; runs
    //  the derivations of each level on the worker pool with an inter-level
    //  barrier and the main-thread rules inline at their level. See execute_leveled.
    execute_leveled (n_noncopy, name_count, last_deriv_ref, parallel, copies, timing);
  } else {
    std::size_t noncopy_rank = 0, copy_rank = 0;
    for (std::size_t i = 0; i < m_deck.statements.size (); ++i) {
      const SVRFStatement &st = m_deck.statements[i];
      if (st.kind == SVRFStatement::Derivation) {
        exec_derivation (m_deck.derivations[st.index]);
        continue;
      }
      const SVRFRule &r = m_deck.rules[st.index];
      if (r.op == "COPY") {
        std::size_t slot = n_noncopy + copy_rank;
        copy_rank += 1;
        copies.push_back (std::make_pair (&r, slot));
        continue;
      }
      std::size_t slot = noncopy_rank;
      noncopy_rank += 1;
      std::map<std::string, std::size_t>::const_iterator ld = last_deriv_ref.find (r.name);
      bool consumed_later = (ld != last_deriv_ref.end () && ld->second > i);
      bool dup_name = (name_count[r.name] > 1);
      bool main_thread = (m_threads <= 1) ||
                         consumed_later ||
                         dup_name ||
                         (r.op == "DENSITY") ||
                         (r.op == "ANTENNA") ||     // native antenna builds a private L2N
                         (r.op == "ERC") ||         // native ERC builds a private L2N
                         (r.op == "VSPACE") ||      // voltage-aware spacing builds a private L2N
                         (r.connectivity != SVRFConnectivity::none);
      if (main_thread) {
        exec_rule (r, slot);                         // inline, at source position
      } else {
        parallel.push_back (std::make_pair (&r, slot));
      }
    }
  }

  clk::time_point t_pass2 = clk::now ();
  if (timing) {
    fprintf (stderr, "SVRFDRC_TIMING [live] pass1 (derivations + serial rules) done: %lldms  (%zu parallel rules pending)\n",
             (long long) std::chrono::duration_cast<std::chrono::milliseconds> (t_pass2 - t_pass1).count (), parallel.size ());
  }

  //  --- pass 2 (parallel): the independent measurement rules --------------
  clk::time_point t_warm = t_pass2;
  if (! parallel.empty ()) {
    prewarm_for_parallel (parallel);               // single-threaded realization
    t_warm = clk::now ();
    if (timing) {
      fprintf (stderr, "SVRFDRC_TIMING [live] prewarm (serial merge/bbox realize) done: %lldms\n",
               (long long) std::chrono::duration_cast<std::chrono::milliseconds> (t_warm - t_pass2).count ());
    }
    run_parallel (parallel);                       // worker pool; joins before pass 3
  }
  clk::time_point t_pass3 = clk::now ();

  //  --- pass 3 (serial): COPY, after every error layer exists -------------
  for (std::vector<std::pair<const SVRFRule *, std::size_t> >::iterator c = copies.begin (); c != copies.end (); ++c) {
    exec_rule (*c->first, c->second);
  }
  if (timing) {
    auto ms = [] (clk::time_point a, clk::time_point b) {
      return std::chrono::duration_cast<std::chrono::milliseconds> (b - a).count ();
    };
    fprintf (stderr,
             "SVRFDRC_TIMING threads=%d  pass1(derivations+serial-rules)=%lldms  "
             "prewarm=%lldms  parallel-checks=%lldms  pass3(COPY)=%lldms  "
             "parallel_rules=%zu\n",
             m_threads, (long long) ms (t_pass1, t_pass2), (long long) ms (t_pass2, t_warm),
             (long long) ms (t_warm, t_pass3), (long long) ms (t_pass3, clk::now ()),
             parallel.size ());
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
  //  emit the waiver audit trail (no-op when nothing was waived).
  flush_waiver_audit ();
  //  DFM scoring (#47): advisory weighted aggregate over soft rules (no-op
  //  unless $SVRFDRC_DFM_WEIGHTS is set). Post-processing -> no rule verdicts move.
  compute_dfm_score ();
  //  RVE result DB (#9): KLayout-loadable marker DB from the frozen error
  //  regions (no-op unless $SVRFDRC_RVE_OUT is set). Post-processing only.
  emit_rve_db ();
  return m_results;
}

// ---------------------------------------------------------------------------
//  parallel measurement-rule phase
// ---------------------------------------------------------------------------

namespace
{

//  A tl::Thread that runs a stored closure. Used to build a tiny worker pool from
//  KLayout's native thread primitive (klayout_tl) -- no external -pthread needed by
//  the parity-test g++ line, and no std::thread.
class SVRFFnThread
  : public tl::Thread
{
public:
  std::function<void ()> fn;
  void run () { if (fn) { fn (); } }
};

}

void SVRFEngine::prewarm_for_parallel (const std::vector<std::pair<const SVRFRule *, std::size_t> > &par)
{
  //  1) resolve every parallel-rule input on the MAIN thread and pre-create every
  //     error-layer output slot, so that during the worker phase std::map is only
  //     READ / assigned-in-place -- never structurally mutated (which would race).
  //  2) force each input region's lazy caches (merged polygons + bbox) valid on the
  //     main thread. db::Region checks read those through `mutable` caches; the
  //     FIRST touch fills them. Worker copies inherit the valid flags (FlatRegion /
  //     AsIfFlatRegion copy ctors propagate m_merged_polygons_valid / m_bbox_valid
  //     and share the filled Shapes copy-on-write), so no worker ever writes shared
  //     state. (Note: db::Region::merged() builds a fresh region WITHOUT setting the
  //     source's m_merged_polygons_valid -- begin_merged() is the primitive that
  //     actually warms the in-place cache the checks consult, so we use it here.)
  std::set<std::string> warm_set;
  std::set<std::string> tiled_ops;                // operands consumed by a tiled family
  for (std::vector<std::pair<const SVRFRule *, std::size_t> >::const_iterator p = par.begin (); p != par.end (); ++p) {
    const SVRFRule &r = *p->first;
    m_regions[r.name];                            // pre-create the error-layer slot
    if (! r.layer1.empty ()) { resolve (r.layer1); warm_set.insert (r.layer1); }
    if (! r.layer2.empty ()) { resolve (r.layer2); warm_set.insert (r.layer2); }
    if (tileable (r)) {
      if (! r.layer1.empty ()) { tiled_ops.insert (r.layer1); }
      if (! r.layer2.empty ()) { tiled_ops.insert (r.layer2); }
    }
  }

  //  3) Route B, Phase 2 (Goal 1 -- kill the serial-merge bottleneck): each
  //     distinct operand's merge is INDEPENDENT of every other, so warm them in
  //     PARALLEL across the worker pool instead of one after another. This was the
  //     dominant serial cost on a multi-million-shape design (the >1000 finite-reach
  //     rules share a handful of big metal operands; each is merged exactly once
  //     here). Filling a DISTINCT region's mutable merged/bbox cache touches only
  //     that region's own state -- m_regions is only READ (find) concurrently and
  //     never structurally mutated -- so this is race-free and byte-identical (the
  //     merge result is deterministic regardless of which thread computes it).
  std::vector<std::string> warm (warm_set.begin (), warm_set.end ());
  const std::size_t nwarm = warm.size ();
  auto warm_one = [this, &warm] (std::size_t i) {
    std::map<std::string, db::Region>::iterator it = m_regions.find (warm[i]);
    if (it != m_regions.end ()) {
      db::Region &reg = it->second;
      { db::RegionIterator mi = reg.begin_merged (); (void) mi; }   // fill merged cache
      reg.bbox ();                                                  // fill bbox cache
    }
  };
  int nw = m_threads;
  if (nw < 1) { nw = 1; }
  if ((std::size_t) nw > nwarm) { nw = (int) nwarm; }
  if (nw <= 1 || nwarm <= 1) {
    for (std::size_t i = 0; i < nwarm; ++i) { warm_one (i); }
  } else {
    std::atomic<std::size_t> next (0);
    std::vector<std::unique_ptr<SVRFFnThread> > workers;
    workers.reserve ((std::size_t) nw);
    for (int t = 0; t < nw; ++t) {
      SVRFFnThread *w = new SVRFFnThread ();
      w->fn = [&warm_one, &next, nwarm] () {
        for (;;) {
          std::size_t i = next.fetch_add (1, std::memory_order_relaxed);
          if (i >= nwarm) { break; }
          warm_one (i);
        }
      };
      workers.push_back (std::unique_ptr<SVRFFnThread> (w));
    }
    for (int t = 0; t < nw; ++t) { workers[t]->start (); }
    for (int t = 0; t < nw; ++t) { workers[t]->wait (); }
  }

  //  4) Prefill the merged-operand cache (Goal 1 memoization) for the operands the
  //     TILED families actually consume. Now that each operand's in-place merged
  //     cache is warm, merged_operand() materializes the merged Region once per
  //     distinct operand, so the tiled checks NEVER re-merge and only ever HIT this
  //     cache during the worker phase (no worker-thread map insert -> no race).
  //     Done single-threaded on the main thread.
  for (std::set<std::string>::const_iterator t = tiled_ops.begin (); t != tiled_ops.end (); ++t) {
    merged_operand (*t);
  }
  //    edge-typed operands live in m_edges_ns; as_edges() returns them BY VALUE
  //    (a copy-on-write copy), so workers never touch the stored Edges in place --
  //    no warm needed there. A region operand consumed as edges goes through
  //    resolve(name).edges(), which reads the region's merged cache warmed above.
}

void SVRFEngine::run_parallel (const std::vector<std::pair<const SVRFRule *, std::size_t> > &par)
{
  int nw = m_threads;
  if (nw < 1) {
    nw = 1;
  }
  if ((std::size_t) nw > par.size ()) {
    nw = (int) par.size ();
  }
  const std::size_t n = par.size ();
  std::atomic<std::size_t> next (0);

  //  Publish the rule-level pool WIDTH so a tiled check nested in exec_rule sizes
  //  its own tile-thread pool to m_threads/width and never oversubscribes past
  //  m_threads total. Written before the workers start (happens-before via start()).
  m_rule_pool_width = nw;

  std::vector<std::unique_ptr<SVRFFnThread> > workers;
  workers.reserve ((std::size_t) nw);
  for (int t = 0; t < nw; ++t) {
    SVRFFnThread *w = new SVRFFnThread ();
    //  Dynamic self-scheduling: rules vary in cost by orders of magnitude, so each
    //  worker grabs the next index atomically rather than taking a static slice.
    w->fn = [this, &par, &next, n] () {
      for (;;) {
        std::size_t i = next.fetch_add (1, std::memory_order_relaxed);
        if (i >= n) {
          break;
        }
        this->exec_rule (*par[i].first, par[i].second);
      }
    };
    workers.push_back (std::unique_ptr<SVRFFnThread> (w));
  }
  for (int t = 0; t < nw; ++t) {
    workers[t]->start ();
  }
  for (int t = 0; t < nw; ++t) {
    workers[t]->wait ();                           // join: happens-before for pass 3
  }
  m_rule_pool_width = 0;                            // back on the main thread
}

// ---------------------------------------------------------------------------
//  topological-level DERIVATION parallelism (fork fix #3)
// ---------------------------------------------------------------------------

namespace
{

//  The exact set of layer names a derivation reads: its operands PLUS the tokens
//  of its boolean expr (tokenized the way eval_bool_expr does). This is the
//  complete set of names exec_derivation() can pass to resolve()/as_edges(), so
//  prewarming this set guarantees no worker ever structurally mutates m_regions /
//  m_edges_ns (every resolve is a cache hit). Mirrors the refs computed in
//  SVRFEngine::execute() for last_deriv_ref.
static std::set<std::string> derivation_refs (const db::SVRFDerivation &d)
{
  std::set<std::string> refs (d.operands.begin (), d.operands.end ());
  std::string cur;
  for (std::size_t k = 0; k < d.expr.size (); ++k) {
    char ch = d.expr[k];
    if (ch == '(' || ch == ')' || std::isspace ((unsigned char) ch)) {
      if (! cur.empty ()) { refs.insert (cur); cur.clear (); }
    } else {
      cur += ch;
    }
  }
  if (! cur.empty ()) { refs.insert (cur); }
  return refs;
}

}

void SVRFEngine::prewarm_names (const std::set<std::string> &names)
{
  //  Single-threaded realization of every INPUT a parallel derivation level will
  //  read: resolve() it (drawing + inserting the map node here, never under a
  //  worker) and force its lazy merged-polygon + bbox caches valid. Worker copies
  //  then inherit the filled caches (COW) and only READ shared state. Identical in
  //  spirit to prewarm_for_parallel() but keyed on a name set (derivation inputs).
  for (std::set<std::string>::const_iterator n = names.begin (); n != names.end (); ++n) {
    db::Region &reg = resolve (*n);                 // cache-fill / draw (single-threaded)
    { db::RegionIterator mi = reg.begin_merged (); (void) mi; }   // fill merged cache
    reg.bbox ();                                                  // fill bbox cache
    if (m_edge_layers.count (*n)) {
      std::map<std::string, db::Edges>::iterator ei = m_edges_ns.find (*n);
      if (ei != m_edges_ns.end ()) {
        { db::EdgesIterator me = ei->second.begin_merged (); (void) me; }
        ei->second.bbox ();
      }
    }
  }
}

void SVRFEngine::run_parallel_derivations (const std::vector<const SVRFDerivation *> &batch)
{
  if (batch.empty ()) {
    return;
  }

  //  --- Phase 3: pull the GIANT tileable derivations OUT of the op-level pool ---
  //  The op-level pool parallelizes ACROSS derivations, but a level dominated by a
  //  FEW enormous full-chip booleans/sizes/selects leaves most cores idle while one
  //  worker grinds the giant op single-threaded -- that is the real wall. Run each
  //  such op ALONE on the main thread with m_deriv_pool_width==1, so its internal
  //  tiled build (exec_derivation -> tiled_region_build) claims every core. Big and
  //  small ops in one level are mutually independent (same topological level), so
  //  processing the big ones first, then the small pool, is order-safe. Big ops run
  //  with nullptr sinks (direct m_unmodeled/m_edge_layers inserts) -- safe because
  //  they run single-threaded before the pool spawns.
  std::vector<const SVRFDerivation *> big, small;
  big.reserve (batch.size ());
  small.reserve (batch.size ());
  for (std::vector<const SVRFDerivation *>::const_iterator it = batch.begin (); it != batch.end (); ++it) {
    if (is_big_tileable_deriv (**it)) { big.push_back (*it); } else { small.push_back (*it); }
  }

  if (! big.empty ()) {
    int saved = m_deriv_pool_width;
    m_deriv_pool_width = 1;                          // "alone" -> full tile budget
    for (std::vector<const SVRFDerivation *>::const_iterator it = big.begin (); it != big.end (); ++it) {
      const SVRFDerivation &d = **it;
      exec_derivation (d, 0, 0);                     // main thread; internally tiled
      //  warm its own output for the next level's prewarm (mirrors the pool path)
      std::map<std::string, db::Region>::iterator ri = m_regions.find (d.name);
      if (ri != m_regions.end ()) {
        { db::RegionIterator mi = ri->second.begin_merged (); (void) mi; }
        ri->second.bbox ();
      }
      if (d.edge_typed) {
        std::map<std::string, db::Edges>::iterator ei = m_edges_ns.find (d.name);
        if (ei != m_edges_ns.end ()) {
          { db::EdgesIterator me = ei->second.begin_merged (); (void) me; }
          ei->second.bbox ();
        }
      }
    }
    m_deriv_pool_width = saved;
  }

  if (small.empty ()) {
    return;
  }

  int nw = m_threads;
  if (nw < 1) {
    nw = 1;
  }
  if ((std::size_t) nw > small.size ()) {
    nw = (int) small.size ();
  }
  const std::size_t n = small.size ();
  std::atomic<std::size_t> next (0);

  //  Publish the op-level pool WIDTH so a small op that still tiles internally sizes
  //  its tile pool to m_threads/nw and never oversubscribes past m_threads. When the
  //  pool saturates the cores (nw==m_threads) that budget is 1 -> the small op runs
  //  its plain flat build (no nested threads). Written before the workers start.
  int saved_width = m_deriv_pool_width;
  m_deriv_pool_width = nw;

  //  Per-worker sinks for the two set-typed side effects, merged single-threaded
  //  after the join (the level barrier). No worker touches m_unmodeled /
  //  m_edge_layers structurally -> those sets are frozen (read-only) during the
  //  level, and every m_regions / m_edges_ns node is pre-created so operator[] only
  //  ASSIGNS an existing node (a read-only tree traversal + value write to a slot
  //  no other worker touches). Hence the map structure is immutable and every
  //  mapped value is single-writer for the duration of the level.
  std::vector<std::vector<std::string> > unmodeled_sinks ((std::size_t) nw);
  std::vector<std::vector<std::string> > edge_sinks ((std::size_t) nw);

  std::vector<std::unique_ptr<SVRFFnThread> > workers;
  workers.reserve ((std::size_t) nw);
  for (int t = 0; t < nw; ++t) {
    SVRFFnThread *w = new SVRFFnThread ();
    std::vector<std::string> *usink = &unmodeled_sinks[(std::size_t) t];
    std::vector<std::string> *esink = &edge_sinks[(std::size_t) t];
    w->fn = [this, &small, &next, n, usink, esink] () {
      for (;;) {
        std::size_t i = next.fetch_add (1, std::memory_order_relaxed);
        if (i >= n) {
          break;
        }
        const SVRFDerivation &d = *small[i];
        this->exec_derivation (d, usink, esink);
        //  Warm THIS derivation's own output caches. The slot is single-writer this
        //  level (no other worker reads or writes d.name), so filling its mutable
        //  merged/bbox caches here is race-free AND parallelizes the merge cost --
        //  the next level's prewarm_names() then hits a warm cache instead of
        //  merging on the (single-threaded) main thread.
        std::map<std::string, db::Region>::iterator ri = m_regions.find (d.name);
        if (ri != m_regions.end ()) {
          { db::RegionIterator mi = ri->second.begin_merged (); (void) mi; }
          ri->second.bbox ();
        }
        if (d.edge_typed) {
          std::map<std::string, db::Edges>::iterator ei = m_edges_ns.find (d.name);
          if (ei != m_edges_ns.end ()) {
            { db::EdgesIterator me = ei->second.begin_merged (); (void) me; }
            ei->second.bbox ();
          }
        }
      }
    };
    workers.push_back (std::unique_ptr<SVRFFnThread> (w));
  }
  for (int t = 0; t < nw; ++t) {
    workers[t]->start ();
  }
  for (int t = 0; t < nw; ++t) {
    workers[t]->wait ();                           // join: happens-before the merge
  }
  //  merge the thread-local sinks into the shared sets (single-threaded)
  for (int t = 0; t < nw; ++t) {
    for (std::vector<std::string>::const_iterator s = unmodeled_sinks[(std::size_t) t].begin (); s != unmodeled_sinks[(std::size_t) t].end (); ++s) {
      m_unmodeled.insert (*s);
    }
    for (std::vector<std::string>::const_iterator s = edge_sinks[(std::size_t) t].begin (); s != edge_sinks[(std::size_t) t].end (); ++s) {
      m_edge_layers.insert (*s);
    }
  }
  m_deriv_pool_width = saved_width;                 // restore (main-thread context)
}

void SVRFEngine::execute_leveled (std::size_t n_noncopy,
                                  const std::map<std::string, int> &name_count,
                                  const std::map<std::string, std::size_t> &last_deriv_ref,
                                  std::vector<std::pair<const SVRFRule *, std::size_t> > &parallel,
                                  std::vector<std::pair<const SVRFRule *, std::size_t> > &copies,
                                  bool timing)
{
  typedef std::chrono::steady_clock clk;
  const std::size_t n_stmts = m_deck.statements.size ();

  //  --- (1) fixed source-order report slots ------------------------------
  //  Assign each rule its frozen report slot in SOURCE order (independent of the
  //  level execution order), so the byte order of the report is unchanged.
  std::vector<std::size_t> stmt_slot (n_stmts, 0);
  {
    std::size_t nr = 0, cr = 0;
    for (std::size_t i = 0; i < n_stmts; ++i) {
      const SVRFStatement &st = m_deck.statements[i];
      if (st.kind != SVRFStatement::Rule) {
        continue;
      }
      if (m_deck.rules[st.index].op == "COPY") {
        stmt_slot[i] = n_noncopy + cr;
        cr += 1;
      } else {
        stmt_slot[i] = nr;
        nr += 1;
      }
    }
  }

  //  --- (2) topological level of every statement -------------------------
  //  Source order is already a valid topological order (producer precedes
  //  consumer), so a single forward pass computes level(stmt) = 1 + max level of
  //  any input layer's producer (drawn / external inputs = level 0). Rules are
  //  levelled too: a rule that produces an error layer consumed by a later
  //  derivation lands one level below that derivation, so running the rule inline
  //  at its level (below) commits the error layer before the consumer's level.
  std::map<std::string, int> name_level;
  std::vector<int> stmt_level (n_stmts, 0);
  int maxlvl = 0;
  for (std::size_t i = 0; i < n_stmts; ++i) {
    const SVRFStatement &st = m_deck.statements[i];
    int L = 0;
    std::string outname;
    if (st.kind == SVRFStatement::Derivation) {
      const SVRFDerivation &d = m_deck.derivations[st.index];
      std::set<std::string> refs = derivation_refs (d);
      for (std::set<std::string>::const_iterator r = refs.begin (); r != refs.end (); ++r) {
        std::map<std::string, int>::const_iterator it = name_level.find (*r);
        if (it != name_level.end ()) { L = std::max (L, it->second + 1); }
      }
      outname = d.name;
    } else {
      const SVRFRule &r = m_deck.rules[st.index];
      if (! r.layer1.empty ()) {
        std::map<std::string, int>::const_iterator it = name_level.find (r.layer1);
        if (it != name_level.end ()) { L = std::max (L, it->second + 1); }
      }
      if (! r.layer2.empty ()) {
        std::map<std::string, int>::const_iterator it = name_level.find (r.layer2);
        if (it != name_level.end ()) { L = std::max (L, it->second + 1); }
      }
      outname = r.name;
    }
    stmt_level[i] = L;
    name_level[outname] = L;              // last writer wins (source order => valid)
    maxlvl = std::max (maxlvl, L);
  }

  //  --- (3) pre-create every derivation output slot ----------------------
  //  Workers only ASSIGN their own m_regions / m_edges_ns node; pre-creating all of
  //  them single-threaded means the map STRUCTURE never mutates under the pool
  //  (structural std::map insertion is not thread-safe even for distinct keys).
  //  m_edge_layers is NOT pre-populated: it must gain an edge name only when the
  //  build SUCCEEDS (mirroring the serial path), so it flows through the sink.
  for (std::vector<SVRFDerivation>::const_iterator d = m_deck.derivations.begin (); d != m_deck.derivations.end (); ++d) {
    m_regions[d->name];                   // default-construct the slot
    if (d->edge_typed) {
      m_edges_ns[d->name];                // default-construct the edge slot
    }
  }

  //  --- (4) bucket statements by level (source order preserved) ----------
  std::vector<std::vector<std::size_t> > by_level ((std::size_t) maxlvl + 1);
  for (std::size_t i = 0; i < n_stmts; ++i) {
    by_level[(std::size_t) stmt_level[i]].push_back (i);
  }

  //  --- (5) run levels in order; parallel derivations + barrier per level -
  long long ms_prewarm = 0, ms_pardrv = 0, ms_serialdrv = 0, ms_rules = 0;
  std::size_t n_par_drv = 0, n_serial_drv = 0;
  for (int L = 0; L <= maxlvl; ++L) {
    const std::vector<std::size_t> &lvl = by_level[(std::size_t) L];

    std::vector<const SVRFDerivation *> par_drv;   // parallel-safe derivations
    std::vector<const SVRFDerivation *> ser_drv;   // serial-class (net_ratio / L2N)
    std::vector<std::size_t> rule_stmts;           // this level's rule statements
    std::set<std::string> warm;                    // inputs the parallel batch reads
    for (std::size_t k = 0; k < lvl.size (); ++k) {
      const SVRFStatement &st = m_deck.statements[lvl[k]];
      if (st.kind == SVRFStatement::Derivation) {
        const SVRFDerivation &d = m_deck.derivations[st.index];
        //  net_ratio builds the shared LayoutToNetlist (mutates m_l2n*) -> keep it
        //  on the main thread. Everything else only reads its inputs + writes its
        //  own slot, so it is pool-safe.
        if (d.kind == "net_ratio") {
          ser_drv.push_back (&d);
        } else {
          par_drv.push_back (&d);
          std::set<std::string> refs = derivation_refs (d);
          warm.insert (refs.begin (), refs.end ());
        }
      } else {
        rule_stmts.push_back (lvl[k]);
      }
    }

    //  (5a) realize every input this level's parallel derivations will read
    clk::time_point a0 = clk::now ();
    prewarm_names (warm);
    clk::time_point a1 = clk::now ();

    //  (5b) serial-class derivations inline (single-threaded, before the pool)
    for (std::size_t k = 0; k < ser_drv.size (); ++k) {
      exec_derivation (*ser_drv[k]);       // nullptr sinks: direct set inserts, safe
    }
    clk::time_point a2 = clk::now ();

    //  (5c) parallel-class derivations on the worker pool (barrier on return)
    run_parallel_derivations (par_drv);
    clk::time_point a3 = clk::now ();

    //  (5d) this level's rules, classified exactly as the serial loop. main-thread
    //  rules run inline NOW (their error layer is committed before any higher-level
    //  consumer); the rest defer to the #1 parallel-rule pass / COPY pass.
    for (std::size_t k = 0; k < rule_stmts.size (); ++k) {
      std::size_t i = rule_stmts[k];
      const SVRFRule &r = m_deck.rules[m_deck.statements[i].index];
      std::size_t slot = stmt_slot[i];
      if (r.op == "COPY") {
        copies.push_back (std::make_pair (&r, slot));
        continue;
      }
      std::map<std::string, std::size_t>::const_iterator ld = last_deriv_ref.find (r.name);
      bool consumed_later = (ld != last_deriv_ref.end () && ld->second > i);
      std::map<std::string, int>::const_iterator nc = name_count.find (r.name);
      bool dup_name = (nc != name_count.end () && nc->second > 1);
      bool main_thread = consumed_later ||       // m_threads>1 here (>1 gate in caller)
                         dup_name ||
                         (r.op == "DENSITY") ||
                         (r.op == "ANTENNA") ||     // native antenna builds a private L2N
                         (r.op == "ERC") ||         // native ERC builds a private L2N
                         (r.op == "VSPACE") ||      // voltage-aware spacing builds a private L2N
                         (r.connectivity != SVRFConnectivity::none);
      if (main_thread) {
        exec_rule (r, slot);
      } else {
        parallel.push_back (std::make_pair (&r, slot));
      }
    }
    clk::time_point a4 = clk::now ();

    ms_prewarm   += std::chrono::duration_cast<std::chrono::milliseconds> (a1 - a0).count ();
    ms_serialdrv += std::chrono::duration_cast<std::chrono::milliseconds> (a2 - a1).count ();
    ms_pardrv    += std::chrono::duration_cast<std::chrono::milliseconds> (a3 - a2).count ();
    ms_rules     += std::chrono::duration_cast<std::chrono::milliseconds> (a4 - a3).count ();
    n_par_drv    += par_drv.size ();
    n_serial_drv += ser_drv.size ();
  }

  //  Deferred rule vectors are collected in level order; their execution is
  //  order-independent (each writes its own fixed slot, no rule reads another
  //  rule's output), but sort by slot so the dispatch order matches the serial
  //  path's source order exactly (defensive determinism).
  std::sort (parallel.begin (), parallel.end (),
             [] (const std::pair<const SVRFRule *, std::size_t> &a,
                 const std::pair<const SVRFRule *, std::size_t> &b) { return a.second < b.second; });
  std::sort (copies.begin (), copies.end (),
             [] (const std::pair<const SVRFRule *, std::size_t> &a,
                 const std::pair<const SVRFRule *, std::size_t> &b) { return a.second < b.second; });

  if (timing) {
    fprintf (stderr,
             "SVRFDRC_TIMING [live] leveled derivations: levels=%d  par_derivs=%zu  serial_derivs=%zu  "
             "prewarm=%lldms  parallel-build=%lldms  serial-build=%lldms  inline-rules=%lldms\n",
             maxlvl + 1, n_par_drv, n_serial_drv,
             ms_prewarm, ms_pardrv, ms_serialdrv, ms_rules);
  }
}

// ---------------------------------------------------------------------------
//  cell-aware FEOL over-fire exemption (fork fix #2, Route B)
//
//  On a dense digital design the sign-off deck derives a qualified-cell FEOL
//  exemption from a SINGLE don't-check marker; routing metal OVER a cell carves
//  that marker, so a foundry-qualified std-cell's INTERIOR FEOL space/notch --
//  which passes the deck STANDALONE -- re-fires once flattened into the design.
//  Those fires are flatten artifacts, not backend defects. This exemption DROPS
//  exactly (a subset of) those artifacts, and NEVER a real one, using a strictly
//  geometric discriminator on the EXACT placed cell geometry:
//
//    an error edge-pair is exempted IFF its error BRIDGE (the quadrilateral whose
//    two long sides ARE the two violating edges) is
//      (1) fully inside EXACTLY ONE placed qualified-master footprint (single
//          master -- an inter-cell violation straddles a boundary and is NOT
//          inside any single footprint), AND
//      (2) at least m_feol_strict dbu clear of that footprint's boundary (STRICT
//          interior -- an abutment violation sits ON the cell boundary), AND
//      (3) not touched by ANY top-level (non-cell-interior) FEOL shape (so a real
//          violation formed with top-level / non-qualified geometry is KEPT).
//    Anything else -- 0 covers, >=2 covers, a boundary touch, or a top-level
//    shape -- is KEPT. This is a CONSERVATIVE LOWER BOUND: it can exempt fewer
//    than the ideal artifact set, but it can never false-clean a real violation.
//
//  Footprints come from the qualified-master library GDS (EXACT per-master shape
//  union, NOT a bbox) placed by the DEF -- mirroring the v1.4.49 host attributor's
//  inputs, but TIGHTER (per-master exact geometry + single-master + strict-
//  interior + top-level guard, versus the host's bbox-union upper bound).
// ---------------------------------------------------------------------------

namespace
{

//  DEF orientation -> (rot90_count, mirror_x). Matches the v1.4.49 attributor's
//  _DEF_ORIENT (mirror about X-axis BEFORE rotation == db::Trans(rot, mirr, u)).
bool def_orient (const std::string &o, int &rot, bool &mirr)
{
  if (o == "N")  { rot = 0; mirr = false; return true; }
  if (o == "S")  { rot = 2; mirr = false; return true; }
  if (o == "E")  { rot = 1; mirr = false; return true; }
  if (o == "W")  { rot = 3; mirr = false; return true; }
  if (o == "FN") { rot = 0; mirr = true;  return true; }
  if (o == "FS") { rot = 2; mirr = true;  return true; }
  if (o == "FE") { rot = 1; mirr = true;  return true; }
  if (o == "FW") { rot = 3; mirr = true;  return true; }
  return false;
}

}  // namespace

void SVRFEngine::setup_cell_aware_feol ()
{
  m_feol_enabled = false;
  if (m_feol_cfg_path.empty ()) {
    return;                              // DEFAULT: disabled -> byte-identical report
  }

  //  --- parse the opt-in config ------------------------------------------
  std::ifstream cf (m_feol_cfg_path.c_str ());
  if (! cf) {
    throw tl::Exception ("cell-aware-feol: cannot open config " + m_feol_cfg_path);
  }
  std::string lib_path, def_path;
  std::set<std::string> qualified;
  std::string line;
  while (std::getline (cf, line)) {
    std::string::size_type h = line.find ('#');
    if (h != std::string::npos) {
      line = line.substr (0, h);
    }
    std::istringstream ls (line);
    std::string key;
    if (! (ls >> key)) {
      continue;
    }
    for (std::string::size_type i = 0; i < key.size (); ++i) {
      key[i] = (char) std::tolower ((unsigned char) key[i]);
    }
    if (key == "lib") {
      ls >> lib_path;
    } else if (key == "def") {
      ls >> def_path;
    } else if (key == "strict_dbu") {
      long v = 1;
      ls >> v;
      m_feol_strict = (db::Coord) (v < 1 ? 1 : v);
    } else if (key == "qualified") {
      std::string t;
      while (ls >> t) { qualified.insert (t); }
    } else if (key == "feol_rule") {
      std::string t;
      while (ls >> t) { m_feol_rules.insert (t); }
    } else if (key == "feol_gds") {
      //  raw FEOL layers as "gds/dt" (dt defaults to 0), e.g. "feol_gds 4/0 5/0"
      std::string t;
      while (ls >> t) {
        std::string::size_type sl = t.find ('/');
        int gl = 0, dt = 0;
        std::istringstream gs (sl == std::string::npos ? t : t.substr (0, sl));
        gs >> gl;
        if (sl != std::string::npos) {
          std::istringstream dsx (t.substr (sl + 1));
          dsx >> dt;
        }
        m_feol_gds.push_back (std::make_pair (gl, dt));
      }
    }
  }
  if (lib_path.empty () || def_path.empty () || qualified.empty () ||
      m_feol_rules.empty () || m_feol_gds.empty ()) {
    //  feol_gds is MANDATORY: without the raw FEOL layers the top-level guard
    //  cannot be built, and without that guard a top-level shard inside a cell
    //  footprint could be false-cleaned. Refuse rather than exempt unsafely.
    throw tl::Exception ("cell-aware-feol: config needs 'lib', 'def', 'feol_gds', "
                         ">=1 'qualified' and >=1 'feol_rule'");
  }

  //  --- read the master library GDS; build EXACT per-master footprints ----
  //  Footprint = union of ALL the master's drawn shapes (every layer), merged.
  //  This is the REAL placed cell geometry (NOT a bbox); on a std-cell the well /
  //  implant / rails / abutment layers tile the cell so the union covers the
  //  interior including inter-feature gaps -- exactly the "is this inside the
  //  cell" predicate we need, while excluding any bbox area the cell doesn't own.
  db::Layout lib;
  {
    tl::InputStream stream (lib_path);
    db::Reader reader (stream);
    reader.read (lib);
  }
  double lib_dbu = lib.dbu ();
  double lib_scale = (m_dbu > 0.0 ? lib_dbu / m_dbu : 1.0);   // -> design dbu
  bool need_scale = (lib_scale > 1.0000001 || lib_scale < 0.9999999);

  //  per-master RAW FEOL geometry (m_feol_gds layers), local coords -- placed
  //  below into m_feol_qual_feol for the top-level guard. Kept local to setup.
  std::map<std::string, db::Region> master_feol;

  for (std::set<std::string>::const_iterator q = qualified.begin (); q != qualified.end (); ++q) {
    std::pair<bool, db::cell_index_type> c = lib.cell_by_name (q->c_str ());
    if (! c.first) {
      continue;                          // master absent from lib -> can never attribute -> never exempt
    }
    const db::Cell &cell = lib.cell (c.second);
    //  all-layer footprint (for strict containment)
    db::Region foot;
    for (db::Layout::layer_iterator li = lib.begin_layers (); li != lib.end_layers (); ++li) {
      db::RecursiveShapeIterator si (lib, cell, (*li).first);
      foot += db::Region (si);            // operator+= (const Region&): exported (insert<Region> is not)
    }
    foot.merge ();
    if (foot.empty ()) {
      continue;
    }
    //  raw FEOL geometry (for the top-level guard)
    db::Region ff;
    for (std::vector<std::pair<int, int> >::const_iterator g = m_feol_gds.begin (); g != m_feol_gds.end (); ++g) {
      unsigned int gi = lib.get_layer (db::LayerProperties (g->first, g->second));
      db::RecursiveShapeIterator si (lib, cell, gi);
      ff += db::Region (si);
    }
    ff.merge ();
    if (need_scale) {
      foot.transform (db::ICplxTrans (lib_scale));
      if (! ff.empty ()) {
        ff.transform (db::ICplxTrans (lib_scale));
      }
    }
    db::Region &stored = (m_feol_master[*q] = foot);
    m_feol_master_strict[*q] = stored.sized (- m_feol_strict);   // may be empty for a tiny cell -> strict fails-safe
    master_feol[*q] = ff;
  }
  if (m_feol_master.empty ()) {
    throw tl::Exception ("cell-aware-feol: no qualified master found in lib " + lib_path);
  }

  //  --- parse DEF placements; build the per-instance footprint index ------
  std::string deftext;
  {
    std::ifstream df (def_path.c_str ());
    if (! df) {
      throw tl::Exception ("cell-aware-feol: cannot open DEF " + def_path);
    }
    std::stringstream ds;
    ds << df.rdbuf ();
    deftext = ds.str ();
  }

  //  DEF database units per micron (default 1000); design coord = def_coord *
  //  (1/units) / m_dbu. On a same-PDK flow (units=1000, dbu=0.001) this is 1.
  double def_units = 1000.0;
  {
    std::string::size_type u = deftext.find ("UNITS DISTANCE MICRONS");
    if (u != std::string::npos) {
      std::istringstream us (deftext.substr (u + 22, 64));
      double n = 0;
      if (us >> n && n > 0) {
        def_units = n;
      }
    }
  }
  double def_scale = (def_units * m_dbu > 0.0 ? 1.0 / (def_units * m_dbu) : 1.0);

  //  COMPONENTS block only. Tokenise and parse each "- <inst> <master> ... +
  //  (PLACED|FIXED) ( x y ) <orient> ... ;" record (placement is on the record).
  std::string::size_type cb = deftext.find ("COMPONENTS");
  std::string::size_type ce = deftext.find ("END COMPONENTS");
  if (cb != std::string::npos && ce != std::string::npos && ce > cb) {
    std::istringstream cs (deftext.substr (cb, ce - cb));
    std::string tok;
    std::string inst, master, orient;
    bool in_rec = false, seen_place = false, want_coords = false;
    long cx = 0, cy = 0;
    int coord_i = 0;
    while (cs >> tok) {
      if (tok == "-") {
        in_rec = true; seen_place = false; want_coords = false; coord_i = 0;
        inst.clear (); master.clear (); orient.clear ();
        if (cs >> inst) { cs >> master; }
        continue;
      }
      if (! in_rec) {
        continue;
      }
      if (tok == "PLACED" || tok == "FIXED") {
        seen_place = true; want_coords = true; coord_i = 0;
        continue;
      }
      if (want_coords) {
        if (tok == "(") { continue; }
        if (tok == ")") { want_coords = false; continue; }
        //  two integer coords between the parens
        long v = 0;
        std::istringstream vs (tok);
        if (vs >> v) {
          if (coord_i == 0) { cx = v; coord_i = 1; }
          else if (coord_i == 1) { cy = v; coord_i = 2; }
        }
        continue;
      }
      if (seen_place && orient.empty () &&
          (tok == "N" || tok == "S" || tok == "E" || tok == "W" ||
           tok == "FN" || tok == "FS" || tok == "FE" || tok == "FW")) {
        orient = tok;
        //  emit the placement now that we have master + coords + orient
        std::map<std::string, db::Region>::iterator mi = m_feol_master.find (master);
        int rot = 0; bool mirr = false;
        if (mi != m_feol_master.end () && def_orient (orient, rot, mirr)) {
          db::Vector disp ((db::Coord) std::llround (cx * def_scale),
                           (db::Coord) std::llround (cy * def_scale));
          db::Trans tr (rot, mirr, disp);
          FeolInst fi;
          fi.foot = &mi->second;
          fi.foot_strict = &m_feol_master_strict[master];
          fi.trans = tr;
          fi.pbox = tr * mi->second.bbox ();
          m_feol_insts.push_back (fi);
          std::map<std::string, db::Region>::iterator ffi = master_feol.find (master);
          if (ffi != master_feol.end () && ! ffi->second.empty ()) {
            m_feol_qual_feol += ffi->second.transformed (tr);   // placed RAW FEOL geometry
          }
        }
        continue;
      }
      if (tok == ";") {
        in_rec = false;
        continue;
      }
    }
  }
  m_feol_qual_feol.merge ();

  m_feol_enabled = true;                 // built (even if 0 instances -> exempts nothing, still safe)
}

db::EdgePairs SVRFEngine::feol_exempt_filter (const SVRFRule &r, const db::EdgePairs &ep)
{
  //  top-level (non-cell-interior) FEOL for THIS rule's input layer: the flat
  //  FEOL geometry OUTSIDE every placed qualified footprint. resolve() is a cache
  //  hit here (the rule already resolved layer1; parallel rules were prewarmed),
  //  so it never mutates m_regions under worker threads. An edge-typed input
  //  resolves EMPTY -> we bail out (provable no-op) rather than risk an unsafe
  //  exemption with no top-level guard.
  db::Region feol_all = resolve (r.layer1);
  if (feol_all.empty ()) {
    return ep;
  }
  //  top-level FEOL = this rule's FEOL minus the RAW FEOL geometry of every
  //  placed qualified master. A top-level / non-qualified shard -- even one
  //  sitting inside a cell's footprint rectangle -- survives here (it is not any
  //  master's own geometry), so a violation it forms is never exempted.
  db::Region top_feol = feol_all - m_feol_qual_feol;

  db::EdgePairs kept;
  for (db::EdgePairs::const_iterator it = ep.begin (); ! it.at_end (); ++it) {
    const db::EdgePair &epair = *it;
    bool exempt = false;

    //  error bridge: two long sides ARE the two violating edges.
    db::Polygon bridge = epair.to_polygon (0);
    db::Region P (bridge);                 // explicit Region(const Polygon&) ctor (exported)
    if (! P.empty ()) {
      db::Box pbox = bridge.box ();
      //  (1)+(2) strict single-master containment (cheap: bbox filter + local
      //  boolean per candidate instance). strict_cover==1 => exactly one cell
      //  strictly contains the bridge; ==2 => ambiguous / boundary-touch => keep.
      int strict_cover = 0;
      for (std::size_t k = 0; k < m_feol_insts.size () && strict_cover < 2; ++k) {
        const FeolInst &fi = m_feol_insts[k];
        if (! pbox.inside (fi.pbox)) {
          continue;                      // cannot fully contain the bridge
        }
        db::Region Plocal = P.transformed (fi.trans.inverted ());
        if (! (Plocal - *fi.foot).empty ()) {
          continue;                      // bridge not fully inside this cell's exact footprint
        }
        if (fi.foot_strict->empty () || ! (Plocal - *fi.foot_strict).empty ()) {
          strict_cover = 2;              // covered but touches the cell boundary -> never exempt
        } else {
          strict_cover += 1;             // strictly interior to exactly this cell
        }
      }
      //  (3) top-level guard -- only for a single strict cover (keeps the
      //  expensive interaction test off the hot path of non-candidates).
      if (strict_cover == 1 && top_feol.selected_interacting (P).count () == 0) {
        exempt = true;
      }
    }

    if (! exempt) {
      kept.insert (epair);               // preserve the pair verbatim (incl. symmetric flag)
    }
  }
  return kept;
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
    //  set on a rectilinear layout (a commercial contact-orientation rule collapse).
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
    //  which flooded opposite-active spacing checks (a commercial spacing-rule family).
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

void SVRFEngine::exec_derivation (const SVRFDerivation &d,
                                  std::vector<std::string> *unmodeled_sink,
                                  std::vector<std::string> *edge_layer_sink)
{
  //  Set-typed side effects go to a thread-local sink when running under the
  //  derivation-parallel pool (merged into the shared std::set single-threaded at
  //  the level barrier), or straight into the shared set on the serial path. The
  //  serial (nullptr-sink) branch is the original in-place insert -> byte-identical.
  auto mark_unmodeled = [&] (const std::string &nm) {
    if (unmodeled_sink) { unmodeled_sink->push_back (nm); } else { m_unmodeled.insert (nm); }
  };
  auto mark_edge_layer = [&] (const std::string &nm) {
    if (edge_layer_sink) { edge_layer_sink->push_back (nm); } else { m_edge_layers.insert (nm); }
  };
  if (! d.supported) {
    mark_unmodeled (d.name);
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
      mark_edge_layer (d.name);
      m_regions[d.name] = db::Region ();     // placeholder for region-typed consumers
    } catch (...) {
      mark_unmodeled (d.name);
      m_regions[d.name] = db::Region ();
    }
    return;
  }
  try {
    const std::string &k = d.kind;
    if (k == "bool_expr") {
      //  operand-name set (excludes the AND/OR/NOT/XOR operators + parens), for
      //  both the tile bbox and the unmodeled propagation.
      std::set<std::string> op_names;
      {
        std::string cur;
        auto flush = [&] () {
          if (! cur.empty ()) {
            std::string u = cur; for (char &c : u) c = (char) std::toupper ((unsigned char) c);
            if (u != "AND" && u != "OR" && u != "NOT" && u != "XOR") { op_names.insert (cur); }
            cur.clear ();
          }
        };
        for (std::size_t i = 0; i < d.expr.size (); ++i) {
          char ch = d.expr[i];
          if (ch == '(' || ch == ')' || std::isspace ((unsigned char) ch)) { flush (); }
          else { cur += ch; }
        }
        flush ();
      }
      //  --threads>1 & alone on the main thread & BOOL tiling enabled -> tile
      //  (0-halo, clip inputs to disjoint core, union). Else the exact flat eval.
      if (deriv_tile_enabled (DTC_BOOL) && m_threads > 1 && m_deriv_pool_width <= 1) {
        db::Box bb;
        for (std::set<std::string>::const_iterator n = op_names.begin (); n != op_names.end (); ++n) {
          bb += resolve (*n).bbox ();
        }
        std::string expr = d.expr;
        m_regions[d.name] = tiled_region_build (bb, 0, false,
          [this, expr] (const db::Box &core, const db::Box &) -> db::Region {
            db::Region cbox (core);
            return this->eval_bool_expr_r (expr, [this, &cbox] (const std::string &nm) -> db::Region {
              db::Region loc = this->resolve (nm).selected_interacting (cbox);
              loc &= cbox;                 // clip whole shapes to the disjoint core
              return loc;
            });
          });
      } else {
        std::vector<std::string> used;
        db::Region reg = eval_bool_expr (d.expr, used);
        m_regions[d.name] = reg;
      }
      //  unmodeled propagation (identical to the flat path: any operand unmodeled
      //  taints d). READ of m_unmodeled is safe under the pool -- operands are
      //  lower-level layers committed at a prior barrier.
      for (std::set<std::string>::const_iterator n = op_names.begin (); n != op_names.end (); ++n) {
        if (m_unmodeled.count (*n)) { mark_unmodeled (d.name); break; }
      }
    } else if (k == "bool") {
      if (deriv_tile_enabled (DTC_BOOL) && m_threads > 1 && m_deriv_pool_width <= 1 && ! d.operands.empty ()) {
        db::Box bb;
        for (std::vector<std::string>::const_iterator op = d.operands.begin (); op != d.operands.end (); ++op) {
          bb += resolve (*op).bbox ();
        }
        std::vector<std::string> ops = d.operands;
        std::string sym = d.bool_sym;
        m_regions[d.name] = tiled_region_build (bb, 0, false,
          [this, ops, sym] (const db::Box &core, const db::Box &) -> db::Region {
            db::Region cbox (core);
            db::Region acc = this->resolve (ops[0]).selected_interacting (cbox);
            acc &= cbox;                   // clip whole shapes to the disjoint core
            for (std::size_t i = 1; i < ops.size (); ++i) {
              db::Region r = this->resolve (ops[i]).selected_interacting (cbox);
              r &= cbox;
              if (sym == "&") { acc &= r; }
              else if (sym == "|") { acc |= r; }
              else if (sym == "-") { acc -= r; }
              else if (sym == "^") { acc ^= r; }
            }
            return acc;                    // point-local => already within the core
          });
      } else {
        db::Region acc = resolve (d.operands[0]);
        for (size_t i = 1; i < d.operands.size (); ++i) {
          db::Region &r = resolve (d.operands[i]);
          if (d.bool_sym == "&") acc &= r;
          else if (d.bool_sym == "|") acc |= r;
          else if (d.bool_sym == "-") acc -= r;
          else if (d.bool_sym == "^") acc ^= r;
        }
        m_regions[d.name] = acc;
      }
    } else if (k == "size") {
      //  Build the sizing as a pure function apply_size(in)->out plus a finite
      //  `reach` (the maximum distance a source point can move the result). Both
      //  the flat path and the halo-tiled path apply the IDENTICAL apply_size, so
      //  the tiled result (whole shapes within `reach` of a disjoint core, sized,
      //  then clipped to the core, unioned) is byte-identical.
      std::function<db::Region (const db::Region &)> apply_size;
      db::Coord reach = 0;
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
        //  wide-metal chain (commercial wide-metal spacing-rule phantoms).
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
        //  reach = sum of the per-step absolute displacements (|sized| + |move| =
        //  |v| each step); a safe upper bound on how far a point can travel.
        bool has_value = d.has_value; double dval = d.value;
        for (std::size_t i = 0; i < dirs.size (); ++i) {
          double vv = (i < vals.size ()) ? vals[i] : (has_value ? dval : 0.0);
          reach += std::abs (to_dbu (vv));
        }
        apply_size = [dirs, vals, has_value, dval, this] (const db::Region &in) -> db::Region {
          db::Region r = in;
          for (std::size_t i = 0; i < dirs.size (); ++i) {
            double vv = (i < vals.size ()) ? vals[i] : (has_value ? dval : 0.0);
            db::Coord v = to_dbu (vv);                         // signed
            db::Coord h = v / 2;
            db::Coord t = v - h;                               // dbu-exact split
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
          return r;
        };
      } else {
        auto it_morph = d.params.find ("morph");
        if (it_morph != d.params.end () && d.has_value) {
          //  OVERUNDER = close (grow then shrink); UNDEROVER = open
          //  (shrink then grow). d.value is +ve for SIZE ... BY d; the
          //  two-step derives dense-array cores / removes thin necks. A point can
          //  reach 2d away (grow d then the opposite size probes another d).
          db::Coord dd = to_dbu (std::abs (d.value));
          bool overunder = (it_morph->second == "OVERUNDER");
          reach = 2 * dd;
          apply_size = [dd, overunder] (const db::Region &in) -> db::Region {
            db::Region r = in;
            if (overunder) { r = r.sized (dd); r = r.sized (-dd); }
            else           { r = r.sized (-dd); r = r.sized (dd); }
            return r;
          };
        } else {
          db::Coord dd = to_dbu (d.has_value ? d.value : 0.0);
          reach = std::abs (dd);
          apply_size = [dd] (const db::Region &in) -> db::Region { return in.sized (dd); };
        }
      }
      if (deriv_tile_enabled (DTC_SIZE) && m_threads > 1 && m_deriv_pool_width <= 1 && ! d.operands.empty ()) {
        const db::Region &base = resolve (d.operands[0]);
        db::Box bb = base.bbox ();
        bb = bb.enlarged (db::Vector (reach, reach));          // cover grown geometry
        db::Coord border = reach + 1;
        if (border < 1) { border = 1; }
        m_regions[d.name] = tiled_region_build (bb, border, true,
          [this, &base, &apply_size] (const db::Box &, const db::Box &halo) -> db::Region {
            db::Region loc = base.selected_interacting (db::Region (halo));  // whole shapes in reach
            if (loc.empty ()) { return db::Region (); }
            return apply_size (loc);
          });
      } else {
        m_regions[d.name] = apply_size (resolve (d.operands[0]));
      }
    } else if (k == "select") {
      std::string op = d.select_op;
      for (char &c : op) c = (char) std::toupper ((unsigned char) c);
      bool negate = pflag (d.params, "negate");
      //  parse the INTERACT count qualifier once (needed both to run the op and to
      //  decide tileability -- the counted path uses RAW-polygon semantics that a
      //  halo b-gather cannot reproduce, so counted selects stay flat).
      size_t cmin = 1;
      size_t cmax = std::numeric_limits<size_t>::max ();
      {
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
      }
      bool counted = (op != "INSIDE" && op != "OUTSIDE" && op != "CUT" &&
                      (cmin != 1 || cmax != std::numeric_limits<size_t>::max ()));

      //  the select as a pure function of (a, b): every branch selects WHOLE a
      //  polygons (or, for negated CUT, the point-local a - cut). Identical to the
      //  flat code below; a tile hands it whole a polygons touching the core plus
      //  every b polygon touching those, so each polygon's verdict is unchanged.
      auto apply_select = [op, negate, cmin, cmax] (const db::Region &a, const db::Region &b) -> db::Region {
        if (op == "INSIDE") {
          return negate ? a.selected_not_inside (b) : a.selected_inside (b);
        } else if (op == "OUTSIDE") {
          return negate ? a.selected_not_outside (b) : a.selected_outside (b);
        } else if (op == "CUT") {
          //  Calibre CUT A B: polygons of A that STRADDLE B's boundary (overlap
          //  area yet not wholly inside). Overlapping-minus-wholly-inside == the
          //  true straddle set (empty for a foundry-clean cell).
          db::Region cut = a.selected_overlapping (b).selected_not_inside (b);
          return negate ? (a - cut) : cut;
        } else {
          //  INTERACT / TOUCH / ENCLOSE. Counted qualifier counts the OTHER layer's
          //  ORIGINAL (unmerged) polygons -- KLayout would otherwise union the 4
          //  EXPAND-EDGE strips of a square into one ring and ==4 never matches.
          if (cmin != 1 || cmax != std::numeric_limits<size_t>::max ()) {
            db::Region braw (b);
            braw.set_merged_semantics (false);
            return negate ? a.selected_not_interacting (braw, cmin, cmax)
                          : a.selected_interacting (braw, cmin, cmax);
          }
          return negate ? a.selected_not_interacting (b, cmin, cmax)
                        : a.selected_interacting (b, cmin, cmax);
        }
      };

      if (deriv_tile_enabled (DTC_SELECT) && m_threads > 1 && m_deriv_pool_width <= 1
          && ! counted && ! d.operands.empty ()) {
        const db::Region &a_full = resolve (d.operands[0]);
        bool has_b = d.operands.size () > 1;
        db::Region empty_b;
        const db::Region &b_full = has_b ? resolve (d.operands[1]) : empty_b;
        db::Box bb = a_full.bbox ();
        //  whole-shape halo: pick the WHOLE a polygons touching each disjoint core,
        //  then every b polygon touching those a polygons -> each a polygon's
        //  verdict is exactly the flat one. Union + merge dedups polygons a polygon
        //  reached from >1 core.
        m_regions[d.name] = tiled_region_build (bb, 0, false,
          [&a_full, &b_full, has_b, &apply_select] (const db::Box &core, const db::Box &) -> db::Region {
            db::Region a_i = a_full.selected_interacting (db::Region (core));
            if (a_i.empty ()) { return db::Region (); }
            db::Region b_i = has_b ? b_full.selected_interacting (a_i) : db::Region ();
            return apply_select (a_i, b_i);
          });
      } else {
        db::Region a = resolve (d.operands[0]);
        db::Region b = d.operands.size () > 1 ? resolve (d.operands[1]) : db::Region ();
        m_regions[d.name] = apply_select (a, b);
      }
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
      //  layers). Distinct from the per-shape EXTENTS op below. The commercial deck's
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
      //  collapsed (a commercial contact-orientation rule). as_edges() consults the edge table
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
      mark_unmodeled (d.name);
      m_regions[d.name] = db::Region ();
    }
  } catch (...) {
    //  an unsupported/failed derivation -> empty region + unmodeled (dependent
    //  rules honestly SKIP rather than false-PASS)
    mark_unmodeled (d.name);
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
//  On the full-FEOL spm GDS this phantom class alone accounted for
//  the per-cell-count families (imp enclosure x1069, NPSD/PPSD waves, ...).
//  Projection overlap of two PARALLEL edges along a's direction, in
//  (unnormalized but internally consistent) projected scalar units. Only the
//  SIGN is used, so the direction magnitude is irrelevant. >0 => the two edges
//  face each other over a real interval (a 2D error REGION can form between
//  them); <=0 => they meet only at a corner point (zero overlap) or are fully
//  offset — no facing region forms.
static long long parallel_proj_overlap (const db::Edge &a, const db::Edge &b)
{
  long long dx = a.dx (), dy = a.dy ();
  if (dx == 0 && dy == 0) {
    return -1;                        //  degenerate edge — treat as no overlap
  }
  long long a1 = (long long) a.p1 ().x () * dx + (long long) a.p1 ().y () * dy;
  long long a2 = (long long) a.p2 ().x () * dx + (long long) a.p2 ().y () * dy;
  long long b1 = (long long) b.p1 ().x () * dx + (long long) b.p1 ().y () * dy;
  long long b2 = (long long) b.p2 ().x () * dx + (long long) b.p2 ().y () * dy;
  long long amin = std::min (a1, a2), amax = std::max (a1, a2);
  long long bmin = std::min (b1, b2), bmax = std::max (b1, b2);
  return std::min (amax, bmax) - std::max (amin, bmin);
}

static db::EdgePairs drop_coincident_pairs (const db::EdgePairs &ep,
                                            bool region_out, bool abut_rule)
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
    //  Corner-only jog of an ABUTTING-layer interface: two PARALLEL edges of
    //  the two abutting layers meeting only at a corner (zero projection
    //  overlap) — the ubiquitous active/well/implant staircase jog of an
    //  abutting foundry cell. Calibre's `ABUT` modifier governs exactly this
    //  abutting-edge corner handling; `ABUT>0<90` (parsed into ignore_angle)
    //  excludes the 0deg/90deg abutment corners. KLayout's Euclidian
    //  separation_check still reports the corner-to-corner distance, so drop
    //  those pairs — but ONLY for rules the deck author wrote with an ABUT
    //  qualifier (abut_rule == has_ignore_angle). This gate is load-bearing for
    //  §4.05: a plain Euclidian/Square `EXTERNAL met < s REGION` (metal / via /
    //  net spacing, INCLUDING the `NOT CONNECTED` different-net path) carries NO
    //  ABUT, so its genuine diagonal corner-to-corner violations are NEVER
    //  dropped — Euclidian/Square metrics legitimately flag a diagonal
    //  short/pinch and masking one would be a false-clean (worse than a
    //  false-fail). Documented residual: a genuine NON-touching diagonal gap
    //  between two ABUT-rule layers is also dropped; ABUT rules are net-unaware
    //  abutting-implant/active/well interface checks where such a gap is not the
    //  manufacturing class they target, and foundry cells are DRC-clean by
    //  construction. A facing-edge spacing violation always has projection
    //  overlap > 0, so this never masks a facing (parallel-run) gap on any rule.
    if (region_out && abut_rule && parallel && parallel_proj_overlap (a, b) <= 0) {
      continue;
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

void SVRFEngine::exec_edge_rule (const SVRFRule &r, std::size_t slot)
{
  if (r.op != "EXTERNAL" && r.op != "INTERNAL" && r.op != "ENCLOSURE") {
    SVRFResult res; res.verdict = "SKIP"; res.rule = &r;
    res.info = "edge op " + r.op + " has no Edges check";
    m_results[slot] = res;
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
    m_results[slot] = res;
    return;
  }
  ep = drop_coincident_pairs (ep, r.region_out, r.has_ignore_angle);
  //  cell-aware FEOL over-fire exemption (fork fix #2): only when opt-in AND this
  //  rule is a named FEOL space/notch rule. Disabled => this block is skipped ->
  //  byte-identical. (For an edge-typed layer the filter is a provable no-op --
  //  its top-level FEOL region resolves empty -- so it stays safe here too.)
  if (m_feol_enabled && m_feol_rules.count (r.name)) {
    ep = feol_exempt_filter (r, ep);
  }
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
  m_results[slot] = res;
}

// ---------------------------------------------------------------------------
//  density windowing
// ---------------------------------------------------------------------------

void SVRFEngine::exec_density (const SVRFRule &r, std::size_t slot)
{
  db::Region reg = resolve (r.layer1);
  if (! r.layer2.empty ()) {
    reg = reg | resolve (r.layer2);
  }
  //  A DENSITY check's denominator is the DESIGN, not the measured layer's own
  //  footprint: "no WINDOW" (or an "INSIDE OF LAYER <container>" clause, which
  //  this parser doesn't specially recognize -- e.g. a foundry's SUB=EXTENT
  //  idiom) means "across the whole chip", exactly what the nullary EXTENT
  //  derivation already computes (m_layout.cell(m_top).bbox(), see the
  //  "layout_extent" case above). Using reg.bbox() here instead made a SPARSE
  //  layer's own tiny local footprint look artificially dense -- e.g. 2 small
  //  vias placed near each other read as ~93% "density" against their own
  //  0.15um^2 bounding box, firing a 10%-max-density rule the true chip-wide
  //  reading (0.0014%) never comes close to -- and was PLACEMENT-dependent:
  //  the identical total via area at two different locations verified FAIL
  //  then PASS purely from how tightly the shapes happened to cluster.
  db::Box extent = m_layout.cell (m_top).bbox ();
  if (extent.empty ()) {
    SVRFResult res; res.rule = &r; res.verdict = "PASS"; res.info = "0";
    m_results[slot] = res;
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
    windows.push_back (extent);
  } else {
    db::Coord s = r.has_step ? to_dbu (r.step) : W;
    if (s <= 0) {
      s = W;
    }
    while (((long long) (extent.width () / s) + 1) * ((long long) (extent.height () / s) + 1) > 20000) {
      s *= 2;
    }
    for (db::Coord y = extent.bottom (); y < extent.top (); y += s) {
      for (db::Coord x = extent.left (); x < extent.right (); x += s) {
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
  m_results[slot] = res;
}

// ---------------------------------------------------------------------------
//  Route B, Phase 1: spatial tiling of finite-reach EXTERNAL (space/separation)
//
//  Studied from KLayout's OWN tiled DRC (src/drc/.../_drc_engine.rb::_tcmd +
//  db::TilingProcessor): a tile collects every WHOLE shape within `border` of its
//  core (dbTilingProcessor.cc:597-606 confine_region keeps shapes uncut) and runs
//  the check per tile. KLayout's generic edge-pair output receiver then keeps a
//  pair by TOUCH (`first.clipped(core) || second.clipped(core)`,
//  dbTilingProcessor.h:240) -- correct for stitching a Region geometry, but it
//  DOUBLE-COUNTS an edge-pair whose two edges (or one long facing run) straddle
//  more than one core, so it is NOT byte-identical for an edge-pair *count*
//  report. We keep KLayout's whole-shape halo collection and replace the touch
//  receiver with an EXACT-geometric dedup, which IS byte-identical: because each
//  tile sees the WHOLE merged polygons, every tile that computes a given violation
//  computes the byte-identical edge pair, so a set keyed on the endpoint coords
//  collapses the duplicates to precisely the flat set.
// ---------------------------------------------------------------------------

bool SVRFEngine::tileable (const SVRFRule &r) const
{
  //  finite-reach families only (worst-case interaction bounded by the rule value
  //  d): EXTERNAL space/separation, INTERNAL 1-layer width, NOTCH, 2-layer
  //  ENCLOSURE. DENSITY / connectivity / net-aware checks have unbounded reach and
  //  stay on the serial flat path.
  bool family;
  if (r.op == "EXTERNAL") {
    family = (r.connectivity == SVRFConnectivity::none);   // net-aware stays serial
  } else if (r.op == "INTERNAL") {
    family = r.layer2.empty ();          // width (1-layer); 2-layer overlap stays flat
  } else if (r.op == "NOTCH") {
    family = true;
  } else if (r.op == "ENCLOSURE") {
    family = ! r.layer2.empty ();        // needs the enclosing (outer) layer
  } else {
    family = false;
  }
  if (! family) {
    return false;
  }
  if (const char *env = getenv ("SVRFDRC_TILE_SPACE")) {
    return atoi (env) > 0;                           // explicit N>0 enables, 0 disables
  }
  return m_threads > 1;                              // default: follow --threads
}

const db::Region &
SVRFEngine::merged_operand (const std::string &name)
{
  {
    tl::MutexLocker lock (&m_merged_ops_mutex);
    std::map<std::string, db::Region>::iterator it = m_merged_ops.find (name);
    if (it != m_merged_ops.end ()) {
      return it->second;                 // already merged -> reuse (never recompute)
    }
  }
  //  Merge OUTSIDE the lock: distinct operands merge in PARALLEL and only the tiny
  //  map insert is serialized. resolve(name) is a cache hit here (prewarm resolved
  //  every parallel-rule operand on the main thread), so no structural mutation of
  //  m_regions races; and when prewarm warmed that region's merged cache, .merged()
  //  is a cheap copy-on-write of the already-merged shapes. If two threads race the
  //  SAME name they both produce the byte-identical merge and the first insert wins.
  db::Region merged = resolve (name).merged ();
  { db::RegionIterator wi = merged.begin_merged (); (void) wi; }   // warm merged cache
  merged.bbox ();                                                  // warm bbox cache
  tl::MutexLocker lock (&m_merged_ops_mutex);
  std::map<std::string, db::Region>::iterator it = m_merged_ops.find (name);
  if (it != m_merged_ops.end ()) {
    return it->second;
  }
  db::Region &slot = m_merged_ops[name];
  slot = merged;
  return slot;
}

db::EdgePairs
SVRFEngine::tiled_check (TiledKind kind, const db::Region &pa, const db::Region *pb,
                         db::Coord d, const db::RegionCheckOptions &o) const
{
  const bool two_layer = (pb != 0);

  //  per-tile flat check: run the IDENTICAL flat family check on a sub-region.
  //  pa/pb are ALREADY merged (merged_operand), so the check merges nothing
  //  further -> every tile that contains a violation computes the byte-identical
  //  edge pair. For 2-layer families pa is the check's PRIMARY operand (l1 for
  //  separation, the OUTER/enclosing layer for enclosure) and pb the secondary.
  auto flat_of = [&] (const db::Region &a, const db::Region *b) -> db::EdgePairs {
    switch (kind) {
      case TK_SPACE:      return a.space_check (d, o);
      case TK_SEPARATION: return a.separation_check (*b, d, o);
      case TK_WIDTH:      return a.width_check (d, o);
      case TK_NOTCH:      return a.notch_check (d, o);
      case TK_ENCLOSURE:  return a.enclosing_check (*b, d, o);
    }
    return db::EdgePairs ();
  };

  //  (0) tile-thread budget. Never oversubscribe past m_threads when nested under
  //  the rule-level worker pool: use m_threads/rule_pool_width tile threads. When
  //  running on the MAIN thread (rule_pool_width==0) use all m_threads -- that is
  //  the single-dominant-rule case spatial tiling exists FOR. When the rule-level
  //  pool already saturates the cores this resolves to 1 tile thread, and (unless
  //  a grid was force-requested) we skip tiling entirely and run the plain flat
  //  check on the merged operands: same byte-identical result, none of the
  //  per-tile halo-select overhead that would otherwise erode the rule-level gain.
  int pool = (m_rule_pool_width > 0 ? m_rule_pool_width : 1);
  int nw = (m_threads > 0 ? m_threads : 1) / pool;
  if (nw < 1) {
    nw = 1;
  }
  bool forced = false;
  int per = 0;
  if (const char *env = getenv ("SVRFDRC_TILE_SPACE")) {
    per = atoi (env);
    forced = (per > 0);
  }
  if (nw <= 1 && ! forced) {
    return flat_of (pa, pb);             // auto-flat: no parallelism to gain
  }

  //  (1) bbox of the already-merged operands.
  db::Box bb = pa.bbox ();
  if (two_layer) {
    bb += pb->bbox ();
  }
  if (bb.empty ()) {
    return db::EdgePairs ();
  }

  //  (2) halo = the rule's finite reach (+1 dbu touch-safety). A neighbour polygon
  //  farther than this can never form a violation with a polygon in the core.
  //  Matches KLayout's own DRC tile border: value for Euclidian/Projection,
  //  1.5*value for Square (_drc_layer.rb ~4409).
  db::Coord reach = d;
  if (o.metrics == db::Square) {
    reach = (db::Coord) std::llround (std::ceil (1.5 * (double) d));
  }
  db::Coord border = reach + 1;
  if (border < 1) {
    border = 1;
  }

  //  (3) tile grid. Cores tile-COVER the bbox; halos = cores grown by border. With
  //  the exact dedup the grid NEVER changes the result (only the work split), so
  //  any covering grid is byte-identical -- we only need every violation's location
  //  to land in some core. per-axis: SVRFDRC_TILE_SPACE=N forces N (stress test),
  //  else auto ~ sqrt(threads). (`per`/`forced` were resolved with the thread
  //  budget above.)
  if (per <= 0) {
    per = (int) std::ceil (std::sqrt ((double) std::max (1, m_threads)));
  }
  if (per < 1) {
    per = 1;
  }
  int nx = per, ny = per;
  if (! forced) {
    //  auto: don't cut tiles below ~4*border (halo overhead would dominate and it
    //  buys no parallelism). Forced grids are honoured verbatim (boundary stress).
    long long minspan = (long long) border * 4 + 1;
    while (nx > 1 && ((long long) bb.width () / nx) < minspan) {
      nx -= 1;
    }
    while (ny > 1 && ((long long) bb.height () / ny) < minspan) {
      ny -= 1;
    }
  }

  std::vector<db::Box> cores;
  cores.reserve ((std::size_t) nx * (std::size_t) ny);
  const db::Coord x0 = bb.left (), y0 = bb.bottom ();
  const long long W = bb.width (), H = bb.height ();
  for (int iy = 0; iy < ny; ++iy) {
    db::Coord cy0 = (db::Coord) (y0 + (H * iy) / ny);
    db::Coord cy1 = (iy + 1 == ny) ? bb.top () : (db::Coord) (y0 + (H * (iy + 1)) / ny);
    for (int ix = 0; ix < nx; ++ix) {
      db::Coord cx0 = (db::Coord) (x0 + (W * ix) / nx);
      db::Coord cx1 = (ix + 1 == nx) ? bb.right () : (db::Coord) (x0 + (W * (ix + 1)) / nx);
      cores.push_back (db::Box (cx0, cy0, cx1, cy1));
    }
  }
  const std::size_t ntiles = cores.size ();

  //  (4) per-tile check into a per-tile slot (no shared mutable merge structure ->
  //  deterministic result for any thread count and across re-runs). Each tile
  //  collects the WHOLE polygons interacting with its halo and runs the IDENTICAL
  //  flat check on that sub-region.
  std::vector<db::EdgePairs> per_tile (ntiles);
  const db::Vector bvec (border, border);
  auto do_tile = [&] (std::size_t i) {
    db::Region hb;
    hb.insert (cores[i].enlarged (bvec));
    db::Region a = pa.selected_interacting (hb);
    if (a.empty ()) {
      return;                                        // nothing of the primary near this core
    }
    if (two_layer) {
      db::Region b = pb->selected_interacting (hb);
      per_tile[i] = flat_of (a, &b);
    } else {
      per_tile[i] = flat_of (a, 0);
    }
  };

  //  nw (tile-thread count) was resolved from the m_threads/rule_pool_width budget
  //  at the top; clamp it to the number of tiles actually produced.
  if ((std::size_t) nw > ntiles) {
    nw = (int) ntiles;
  }
  if (nw <= 1 || ntiles <= 1) {
    for (std::size_t i = 0; i < ntiles; ++i) {
      do_tile (i);
    }
  } else {
    std::atomic<std::size_t> next (0);
    std::vector<std::unique_ptr<SVRFFnThread> > workers;
    workers.reserve ((std::size_t) nw);
    for (int t = 0; t < nw; ++t) {
      SVRFFnThread *w = new SVRFFnThread ();
      w->fn = [&do_tile, &next, ntiles] () {
        for (;;) {
          std::size_t i = next.fetch_add (1, std::memory_order_relaxed);
          if (i >= ntiles) {
            break;
          }
          do_tile (i);
        }
      };
      workers.push_back (std::unique_ptr<SVRFFnThread> (w));
    }
    for (int t = 0; t < nw; ++t) {
      workers[t]->start ();
    }
    for (int t = 0; t < nw; ++t) {
      workers[t]->wait ();
    }
  }

  //  (5) merge with EXACT geometric dedup. Every tile that sees both whole operand
  //  polygons of a given violation computes the SAME violation, so a per-violation
  //  key collapses the duplicates to exactly the flat set.
  //
  //  CRITICAL (verified against KLayout's own space_check): the (first,second)
  //  assignment and each edge's p1->p2 DIRECTION are NOT canonical -- they depend
  //  on the input polygon processing order, which differs between tiles (and vs
  //  the flat run). So the naive 8-coord "as-reported" key sees the SAME violation
  //  under two different orderings and fails to collapse it -> a boundary-straddling
  //  pair double-counts (measured: single-layer space 33->35/37, M1 space 35->38 at
  //  fine grids). The key must therefore be ORDER-INDEPENDENT: normalize each edge
  //  to sorted endpoints (kills p1<->p2 direction) and sort the two edges (kills
  //  first<->second swap). Two DISTINCT violations always have a distinct unordered
  //  {undirected-edge, undirected-edge} set, so this never over-collapses. Result
  //  is byte-identical to flat for ANY grid / thread count (proven: canon-dedup ==
  //  flat count at every grid 1..64).
  db::EdgePairs out;
  std::set<std::array<db::Coord, 8> > seen;
  for (std::size_t i = 0; i < ntiles; ++i) {
    for (db::EdgePairs::const_iterator p = per_tile[i].begin (); ! p.at_end (); ++p) {
      const db::Edge &e1 = (*p).first ();
      const db::Edge &e2 = (*p).second ();
      db::Coord a[4] = { e1.p1 ().x (), e1.p1 ().y (), e1.p2 ().x (), e1.p2 ().y () };
      db::Coord b[4] = { e2.p1 ().x (), e2.p1 ().y (), e2.p2 ().x (), e2.p2 ().y () };
      //  undirected each edge: order its two endpoints (x, then y)
      if (a[0] > a[2] || (a[0] == a[2] && a[1] > a[3])) {
        std::swap (a[0], a[2]); std::swap (a[1], a[3]);
      }
      if (b[0] > b[2] || (b[0] == b[2] && b[1] > b[3])) {
        std::swap (b[0], b[2]); std::swap (b[1], b[3]);
      }
      //  unordered pair: order the two (now-undirected) edges lexicographically
      bool a_first = std::lexicographical_compare (a, a + 4, b, b + 4)
                     || std::equal (a, a + 4, b);
      std::array<db::Coord, 8> k;
      if (a_first) {
        k = {{ a[0], a[1], a[2], a[3], b[0], b[1], b[2], b[3] }};
      } else {
        k = {{ b[0], b[1], b[2], b[3], a[0], a[1], a[2], a[3] }};
      }
      if (seen.insert (k).second) {
        out.insert (*p);
      }
    }
  }
  return out;
}

// ---------------------------------------------------------------------------
//  Route B, Phase 3: spatial tiling of the giant single DERIVATION ops
// ---------------------------------------------------------------------------

bool SVRFEngine::deriv_tile_enabled (DerivTileClass c) const
{
  //  master off switch first (SVRFDRC_TILE_DERIV=0 disables all derivation tiling)
  if (const char *m = getenv ("SVRFDRC_TILE_DERIV")) {
    if (atoi (m) == 0) { return false; }
  }
  const char *e = 0;
  switch (c) {
    case DTC_BOOL:   e = getenv ("SVRFDRC_TILE_DERIV_BOOL");   break;
    case DTC_SIZE:   e = getenv ("SVRFDRC_TILE_DERIV_SIZE");   break;
    case DTC_SELECT: e = getenv ("SVRFDRC_TILE_DERIV_SELECT"); break;
  }
  if (e) {
    return atoi (e) > 0;                 // explicit override for this class
  }
  return true;                           // default on (call sites also gate on m_threads>1)
}

bool SVRFEngine::is_big_tileable_deriv (const SVRFDerivation &d)
{
  if (m_threads <= 1 || d.edge_typed || ! d.supported) {
    return false;
  }
  DerivTileClass c;
  const std::string &k = d.kind;
  if (k == "bool" || k == "bool_expr") { c = DTC_BOOL; }
  else if (k == "size")                { c = DTC_SIZE; }
  else if (k == "select")              { c = DTC_SELECT; }
  else                                 { return false; }
  if (! deriv_tile_enabled (c)) {
    return false;
  }
  std::size_t big = 4000;                // merged-polygon count threshold (tunable)
  if (const char *e = getenv ("SVRFDRC_TILE_DERIV_BIG")) {
    long v = atol (e);
    if (v > 0) { big = (std::size_t) v; }
  }
  //  the largest input operand decides: a giant full-chip op has a big operand.
  //  Inputs are prewarmed (execute_leveled::prewarm_names before this runs), so
  //  count() reads the warm merged cache -- cheap.
  std::set<std::string> refs = derivation_refs (d);
  std::size_t maxc = 0;
  for (std::set<std::string>::const_iterator n = refs.begin (); n != refs.end (); ++n) {
    std::map<std::string, db::Region>::iterator it = m_regions.find (*n);
    if (it != m_regions.end ()) {
      std::size_t cnt = it->second.count ();
      if (cnt > maxc) { maxc = cnt; }
      if (maxc >= big) { return true; }
    }
  }
  return maxc >= big;
}

db::Region
SVRFEngine::tiled_region_build (const db::Box &bb, db::Coord border, bool clip_to_core,
                                const std::function<db::Region (const db::Box &, const db::Box &)> &per_tile) const
{
  if (bb.empty ()) {
    return db::Region ();
  }

  //  (0) tile-thread budget: never oversubscribe past m_threads when this op is one
  //  of several on the op-level derivation pool. m_deriv_pool_width<=1 => the op is
  //  running alone (main thread) and may claim every core.
  int pool = (m_deriv_pool_width > 0 ? m_deriv_pool_width : 1);
  int nw = (m_threads > 0 ? m_threads : 1) / pool;
  if (nw < 1) {
    nw = 1;
  }

  //  (1) grid: aim for ~4x tiles per tile-thread so the dynamic-grab pool load-
  //  balances the giant op (a few oversized tiles would leave threads idle at the
  //  tail). SVRFDRC_TILE_DERIV_GRID=N forces N tiles/axis (stress test).
  int per;
  if (const char *g = getenv ("SVRFDRC_TILE_DERIV_GRID")) {
    per = atoi (g);
    if (per < 1) { per = 1; }
  } else {
    per = (int) std::ceil (std::sqrt (4.0 * (double) std::max (1, nw)));
    if (per < 1) { per = 1; }
  }
  int nx = per, ny = per;

  //  don't cut a tile below ~4*border (the halo overhead would dominate and buy no
  //  parallelism). For point-local booleans border==0 so this never clamps.
  long long minspan = (long long) border * 4 + 1;
  while (nx > 1 && ((long long) bb.width () / nx) < minspan) { nx -= 1; }
  while (ny > 1 && ((long long) bb.height () / ny) < minspan) { ny -= 1; }

  std::vector<db::Box> cores;
  cores.reserve ((std::size_t) nx * (std::size_t) ny);
  const db::Coord x0 = bb.left (), y0 = bb.bottom ();
  const long long W = bb.width (), H = bb.height ();
  for (int iy = 0; iy < ny; ++iy) {
    db::Coord cy0 = (db::Coord) (y0 + (H * iy) / ny);
    db::Coord cy1 = (iy + 1 == ny) ? bb.top () : (db::Coord) (y0 + (H * (iy + 1)) / ny);
    for (int ix = 0; ix < nx; ++ix) {
      db::Coord cx0 = (db::Coord) (x0 + (W * ix) / nx);
      db::Coord cx1 = (ix + 1 == nx) ? bb.right () : (db::Coord) (x0 + (W * (ix + 1)) / nx);
      cores.push_back (db::Box (cx0, cy0, cx1, cy1));
    }
  }
  const std::size_t ntiles = cores.size ();

  //  (2) per-tile build into a private slot (no shared mutable structure -> the
  //  result is deterministic for any thread count / grid).
  std::vector<db::Region> per_tile_out (ntiles);
  const db::Vector bvec (border, border);
  auto do_tile = [&] (std::size_t i) {
    db::Box halo = cores[i].enlarged (bvec);
    db::Region r = per_tile (cores[i], halo);
    if (clip_to_core) {
      r &= db::Region (cores[i]);        // finite-reach: keep only this core's share
    }
    per_tile_out[i] = r;
  };

  if ((std::size_t) nw > ntiles) {
    nw = (int) ntiles;
  }
  if (nw <= 1 || ntiles <= 1) {
    for (std::size_t i = 0; i < ntiles; ++i) {
      do_tile (i);
    }
  } else {
    std::atomic<std::size_t> next (0);
    std::vector<std::unique_ptr<SVRFFnThread> > workers;
    workers.reserve ((std::size_t) nw);
    for (int t = 0; t < nw; ++t) {
      SVRFFnThread *w = new SVRFFnThread ();
      w->fn = [&do_tile, &next, ntiles] () {
        for (;;) {
          std::size_t i = next.fetch_add (1, std::memory_order_relaxed);
          if (i >= ntiles) {
            break;
          }
          do_tile (i);
        }
      };
      workers.push_back (std::unique_ptr<SVRFFnThread> (w));
    }
    for (int t = 0; t < nw; ++t) { workers[t]->start (); }
    for (int t = 0; t < nw; ++t) { workers[t]->wait (); }
  }

  //  (3) stitch: raw-insert every tile's polygons then merge ONCE. The point set is
  //  exactly the flat op's; merge() collapses it to the canonical polygon set, so
  //  the stored geometry is byte-identical to the flat derivation (bool/size tiles
  //  are disjoint after the core clip -> the merge only stitches boundary
  //  fragments; select tiles share whole polygons -> the merge dedups them).
  db::Region out;
  for (std::size_t i = 0; i < ntiles; ++i) {
    for (db::Region::const_iterator p = per_tile_out[i].begin (); ! p.at_end (); ++p) {
      out.insert (*p);
    }
  }
  return out.merged ();
}

db::Region
SVRFEngine::eval_bool_expr_r (const std::string &expr,
                              const std::function<db::Region (const std::string &)> &res)
{
  //  Structurally identical recursive descent to eval_bool_expr, but every atom is
  //  resolved through `res` (a tile hands a clipped-to-core view of each operand).
  //  Because AND/OR/NOT/XOR are point-local, evaluating the SAME expression on the
  //  disjoint-core-clipped operands and unioning the tiles reproduces the flat
  //  result exactly.
  std::vector<std::string> toks;
  {
    std::string cur;
    for (std::size_t i = 0; i < expr.size (); ++i) {
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
    if (! cur.empty ()) { toks.push_back (cur); }
  }

  std::size_t pos = 0;
  std::function<db::Region ()> expression;
  std::function<db::Region ()> atom = [&] () -> db::Region {
    if (pos < toks.size () && toks[pos] == "(") {
      pos += 1;
      db::Region v = expression ();
      if (pos < toks.size () && toks[pos] == ")") { pos += 1; }
      return v;
    }
    std::string t = toks[pos];
    pos += 1;
    return res (t);
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
      if (u == "AND") { val &= rhs; }
      else if (u == "OR") { val |= rhs; }
      else if (u == "NOT") { val -= rhs; }
      else { val ^= rhs; }
    }
    return val;
  };
  return expression ();
}

// ---------------------------------------------------------------------------
//  measurement rule dispatch
// ---------------------------------------------------------------------------

void SVRFEngine::exec_rule (const SVRFRule &r, std::size_t slot)
{
  //  COPY: report a previously-computed error layer (foundry rule naming)
  if (r.op == "COPY") {
    const std::string &src = r.layer1;
    if (m_unmodeled.count (src)) {           // antenna / net-ratio etc: honest SKIP, never PASS
      SVRFResult res; res.rule = &r; res.verdict = "SKIP";
      res.info = "errlayer '" + src + "' routed to dedicated checker";
      m_results[slot] = res;
      return;
    }
    std::map<std::string, db::Region>::iterator it = m_regions.find (src);
    db::Region reg;
    if (it == m_regions.end ()) {            // COPY of a plain drawn layer -> report its shapes
      if (! drawn (src, reg)) {
        SVRFResult res; res.rule = &r; res.verdict = "SKIP";
        res.info = "errlayer '" + src + "' unresolved";
        m_results[slot] = res;
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
    m_results[slot] = res;
    return;
  }

  if (! r.supported) {
    SVRFResult res; res.rule = &r; res.verdict = "SKIP";
    res.info = r.reason.empty () ? "unsupported" : r.reason;
    m_results[slot] = res;
    return;
  }
  if (inputs_unmodeled (r)) {
    SVRFResult res; res.rule = &r; res.verdict = "SKIP";
    res.info = "input layer is edge-typed/unmodeled";
    m_results[slot] = res;
    return;
  }
  if (is_edge_rule (r)) {
    exec_edge_rule (r, slot);
    return;
  }
  if (r.op == "DENSITY") {
    exec_density (r, slot);
    return;
  }
  if (r.op == "ANTENNA") {
    antenna_check (r, slot);           // native in-engine staged antenna (fork #20)
    return;
  }
  if (r.op == "PROPERTY") {
    exec_property (r, slot);           // eqDRC (#8): equation-based per-shape check
    return;
  }
  if (r.op == "ERC") {
    exec_erc (r, slot);                // ERC (#13): native electrical-rule check
    return;
  }
  if (r.op == "VSPACE") {
    exec_vspace (r, slot);             // #12: net-voltage-dependent spacing
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
        m_results[slot] = res;
        return;
      }
      if (r.op != "EXTERNAL" || r.layer2.empty ()) {
        SVRFResult res; res.rule = &r; res.verdict = "SKIP";
        res.info = "connectivity needs 2-layer EXTERNAL";
        m_results[slot] = res;
        return;
      }
      build_l2n ();
      if (! m_l2n_layers.count (r.layer1) || ! m_l2n_layers.count (r.layer2)) {
        SVRFResult res; res.rule = &r; res.verdict = "SKIP";
        res.info = "net layer has no extracted shapes";
        m_results[slot] = res;
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
      //  Route B: for every finite-reach family, tileable(r) engages the
      //  byte-identical spatial-tiling parallel path (which auto-falls-back to the
      //  plain flat check when tiling would not add parallelism). The operands go
      //  in ALREADY-MERGED (merged_operand -> each distinct operand merged once).
      const bool tile = tileable (r);
      if (r.op == "EXTERNAL") {
        db::RegionCheckOptions o = check_options (r);
        if (tile) {
          const db::Region &pa = merged_operand (r.layer1);
          if (r.layer2.empty ()) {
            ep = tiled_check (TK_SPACE, pa, 0, d, o);
          } else {
            const db::Region &pb = merged_operand (r.layer2);
            ep = tiled_check (TK_SEPARATION, pa, &pb, d, o);
          }
        } else {
          ep = r.layer2.empty () ? l1.space_check (d, o) : l1.separation_check (resolve (r.layer2), d, o);
        }
        have_ep = true;
      } else if (r.op == "INTERNAL") {
        if (r.layer2.empty ()) {
          db::RegionCheckOptions o = check_options (r, false);
          ep = tile ? tiled_check (TK_WIDTH, merged_operand (r.layer1), 0, d, o)
                    : l1.width_check (d, o);
        } else {
          ep = l1.overlap_check (resolve (r.layer2), d, check_options (r));   // 2-layer: flat
        }
        have_ep = true;
      } else if (r.op == "NOTCH") {
        db::RegionCheckOptions o = check_options (r, false);
        ep = tile ? tiled_check (TK_NOTCH, merged_operand (r.layer1), 0, d, o)
                  : l1.notch_check (d, o);
        have_ep = true;
      } else if (r.op == "ENCLOSURE") {
        db::RegionCheckOptions o = check_options (r);
        if (tile) {
          //  flat is outer.enclosing_check(l1): primary = outer (layer2),
          //  secondary = the enclosed layer1.
          const db::Region &outer = merged_operand (r.layer2);
          const db::Region &inner = merged_operand (r.layer1);
          ep = tiled_check (TK_ENCLOSURE, outer, &inner, d, o);
        } else {
          db::Region outer = r.layer2.empty () ? db::Region () : resolve (r.layer2);
          ep = outer.enclosing_check (l1, d, o);
        }
        have_ep = true;
      } else {
        SVRFResult res; res.rule = &r; res.verdict = "SKIP";
        res.info = "op " + r.op + " not in core";
        m_results[slot] = res;
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
    m_results[slot] = res;
    return;
  } catch (...) {
    SVRFResult res; res.rule = &r; res.verdict = "ERROR";
    res.info = "check error";
    m_results[slot] = res;
    return;
  }
  if (have_ep) {
    ep = drop_coincident_pairs (ep, r.region_out, r.has_ignore_angle);
    //  cell-aware FEOL over-fire exemption (fork fix #2): only when opt-in AND
    //  this rule is a named FEOL space/notch rule. Disabled => skipped ->
    //  byte-identical. r.layer1 was resolved above (cache hit / prewarmed) so the
    //  filter's resolve() never mutates m_regions under worker threads.
    if (m_feol_enabled && m_feol_rules.count (r.name)) {
      ep = feol_exempt_filter (r, ep);
    }
    viol.clear ();
    ep.polygons (viol);
  }
  //  automated waiver management (#10): geometry-anchored suppression of
  //  pre-approved markers (no-op unless $SVRFDRC_WAIVERS names a waiver file).
  std::size_t waived = 0;
  maybe_apply_waivers (r, ep, have_ep, viol, waived);
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
  m_results[slot] = res;
}

// ---------------------------------------------------------------------------
//  eqDRC (#8): equation-based DRC -- a small property expression evaluator
// ---------------------------------------------------------------------------

namespace {

//  Tokenize a property expression into identifiers / numbers / operators.
//  Literals are plain decimals (no exponent form -- '-' is always an operator).
static std::vector<std::string> prop_tokens (const std::string &expr)
{
  std::vector<std::string> out;
  size_t i = 0, n = expr.size ();
  while (i < n) {
    char c = expr[i];
    if (isspace ((unsigned char) c)) { ++i; continue; }
    if (c == '(' || c == ')' || c == '+' || c == '-' || c == '*' || c == '/') {
      out.push_back (std::string (1, c)); ++i; continue;
    }
    size_t j = i;
    while (j < n) {
      char d = expr[j];
      if (isspace ((unsigned char) d) || d == '(' || d == ')' ||
          d == '+' || d == '-' || d == '*' || d == '/') break;
      ++j;
    }
    out.push_back (expr.substr (i, j - i));
    i = j;
  }
  return out;
}

//  Recursive-descent evaluator with the usual precedence:
//    expr   := term (('+'|'-') term)*
//    term   := factor (('*'|'/') factor)*
//    factor := number | property | '(' expr ')' | ('+'|'-') factor
//  On any structural error (unbalanced paren, unknown token, divide-by-zero)
//  the `ok` flag is cleared and evaluation returns 0.
struct PropEval
{
  const std::vector<std::string> &tok;
  size_t pos;
  double area, perim, w, h;
  bool ok;
  PropEval (const std::vector<std::string> &t, double a, double p, double ww, double hh)
    : tok (t), pos (0), area (a), perim (p), w (ww), h (hh), ok (true) { }

  const std::string *peek () const { return pos < tok.size () ? &tok[pos] : 0; }

  double parse_expr ()
  {
    double v = parse_term ();
    while (ok) {
      const std::string *t = peek ();
      if (! t) break;
      if (*t == "+") { ++pos; v += parse_term (); }
      else if (*t == "-") { ++pos; v -= parse_term (); }
      else break;
    }
    return v;
  }
  double parse_term ()
  {
    double v = parse_factor ();
    while (ok) {
      const std::string *t = peek ();
      if (! t) break;
      if (*t == "*") { ++pos; v *= parse_factor (); }
      else if (*t == "/") { ++pos; double d = parse_factor (); if (d == 0.0) { ok = false; return 0.0; } v /= d; }
      else break;
    }
    return v;
  }
  double parse_factor ()
  {
    if (pos >= tok.size ()) { ok = false; return 0.0; }
    const std::string &t = tok[pos++];
    if (t == "(") {
      double v = parse_expr ();
      if (pos >= tok.size () || tok[pos] != ")") { ok = false; return 0.0; }
      ++pos;
      return v;
    }
    if (t == "-") return -parse_factor ();
    if (t == "+") return parse_factor ();
    if (t == ")" || t == "*" || t == "/") { ok = false; return 0.0; }
    std::string up;
    up.reserve (t.size ());
    for (size_t k = 0; k < t.size (); ++k) up.push_back ((char) toupper ((unsigned char) t[k]));
    if (up == "AREA") return area;
    if (up == "PERIMETER" || up == "PERIM") return perim;
    if (up == "WIDTH") return w;
    if (up == "HEIGHT") return h;
    char *end = 0;
    double d = strtod (t.c_str (), &end);
    if (end && *end == '\0' && end != t.c_str ()) return d;
    ok = false;
    return 0.0;
  }
};

} // namespace

double SVRFEngine::eval_prop_expr (const std::string &expr, double area, double perim,
                                   double w, double h, bool &ok)
{
  std::vector<std::string> tok = prop_tokens (expr);
  if (tok.empty ()) { ok = false; return 0.0; }
  PropEval ev (tok, area, perim, w, h);
  double v = ev.parse_expr ();
  if (! ev.ok || ev.pos != tok.size ()) { ok = false; return 0.0; }
  ok = true;
  return v;
}

void SVRFEngine::exec_property (const SVRFRule &r, std::size_t slot)
{
  //  validate the expression once (dummy eval) -> honest SKIP if malformed.
  bool ok = true;
  eval_prop_expr (r.prop_expr, 1.0, 1.0, 1.0, 1.0, ok);
  if (! ok) {
    SVRFResult res; res.rule = &r; res.verdict = "SKIP";
    res.info = "unparsable property expr";
    m_results[slot] = res;
    return;
  }
  db::Region reg;
  try {
    reg = resolve (r.layer1);
  } catch (tl::Exception &e) {
    SVRFResult res; res.rule = &r; res.verdict = "ERROR";
    std::string m = e.msg (); if (m.size () > 80) m = m.substr (0, 80);
    res.info = m; m_results[slot] = res; return;
  } catch (...) {
    SVRFResult res; res.rule = &r; res.verdict = "ERROR";
    res.info = "property resolve error"; m_results[slot] = res; return;
  }

  const bool dbg = getenv ("SVRFDRC_PROPVAL") != 0;
  db::Region viol;
  std::size_t idx = 0;
  for (db::Region::const_iterator p = reg.begin_merged (); ! p.at_end (); ++p, ++idx) {
    const db::Polygon &poly = *p;
    //  Per-shape measured properties in um. AREA/PERIMETER are exact integer DBU
    //  measures scaled by the DBU; a scale-free equation (e.g. PERIMETER*PERIMETER
    //  / AREA) is therefore reproducible to the bit regardless of the DBU.
    double area  = (double) poly.area () * m_dbu * m_dbu;   // um^2
    double perim = (double) poly.perimeter () * m_dbu;      // um
    db::Box bb = poly.box ();
    double pw = (double) bb.width () * m_dbu;               // um
    double ph = (double) bb.height () * m_dbu;              // um
    bool eok = true;
    double val = eval_prop_expr (r.prop_expr, area, perim, pw, ph, eok);
    if (! eok) continue;
    bool bad = false;
    if      (r.cmp == "<")  bad = (val <  r.value);
    else if (r.cmp == "<=") bad = (val <= r.value);
    else if (r.cmp == ">")  bad = (val >  r.value);
    else if (r.cmp == ">=") bad = (val >= r.value);
    else if (r.cmp == "==") bad = (val == r.value);
    if (dbg) {
      fprintf (stderr, "PROPVAL %s #%zu val=%.6f cmp %s thr=%.6f -> %s\n",
               r.name.c_str (), idx, val, r.cmp.c_str (), r.value, bad ? "FAIL" : "ok");
    }
    if (bad) viol.insert (poly);
  }

  //  waiver suppression also applies to eqDRC violations (geometry-anchored).
  db::EdgePairs no_ep;
  std::size_t waived = 0;
  maybe_apply_waivers (r, no_ep, false, viol, waived);

  m_regions[r.name] = viol;
  std::size_t cnt = viol.count ();
  SVRFResult res; res.rule = &r;
  res.verdict = cnt == 0 ? "PASS" : "FAIL";
  res.info = std::to_string (cnt);
  m_results[slot] = res;
}

// ---------------------------------------------------------------------------
//  automated waiver management (#10): geometry-anchored marker suppression
// ---------------------------------------------------------------------------

void SVRFEngine::load_waivers ()
{
  m_waivers_enabled = false;
  m_waivers.clear ();
  const char *path = getenv ("SVRFDRC_WAIVERS");
  if (! path || ! *path) return;
  std::ifstream f (path);
  if (! f) return;
  std::string line;
  while (std::getline (f, line)) {
    size_t h = line.find ('#');                    // strip trailing comment
    if (h != std::string::npos) line = line.substr (0, h);
    std::istringstream ss (line);
    std::string rule; double x1, y1, x2, y2;
    if (! (ss >> rule >> x1 >> y1 >> x2 >> y2)) continue;   // blank / malformed -> skip
    WaiverBox wb;
    wb.l = std::min (x1, x2); wb.r = std::max (x1, x2);
    wb.b = std::min (y1, y2); wb.t = std::max (y1, y2);
    m_waivers[rule].push_back (wb);
  }
  m_waivers_enabled = ! m_waivers.empty ();
}

void SVRFEngine::maybe_apply_waivers (const SVRFRule &r, db::EdgePairs &ep, bool have_ep,
                                      db::Region &viol, std::size_t &waived)
{
  waived = 0;
  if (! m_waivers_enabled) return;

  //  boxes keyed to this rule name PLUS the wildcard "*".
  std::vector<WaiverBox> boxes;
  std::map<std::string, std::vector<WaiverBox> >::const_iterator it = m_waivers.find (r.name);
  if (it != m_waivers.end ()) boxes.insert (boxes.end (), it->second.begin (), it->second.end ());
  it = m_waivers.find ("*");
  if (it != m_waivers.end ()) boxes.insert (boxes.end (), it->second.begin (), it->second.end ());
  if (boxes.empty ()) return;

  //  a marker is waived only when its bbox is FULLY CONTAINED in a waiver box
  //  (partial overlap / wrong coordinate never suppresses a real violation).
  auto contained = [&] (const db::Box &b) -> const SVRFEngine::WaiverBox * {
    double l = b.left () * m_dbu, bo = b.bottom () * m_dbu;
    double rr = b.right () * m_dbu, tp = b.top () * m_dbu;
    for (size_t k = 0; k < boxes.size (); ++k) {
      const WaiverBox &w = boxes[k];
      if (w.l <= l && w.r >= rr && w.b <= bo && w.t >= tp) return &boxes[k];
    }
    return 0;
  };

  std::vector<std::string> local_log;
  auto logline = [&] (const db::Box &b, const SVRFEngine::WaiverBox *w) {
    char buf[256];
    snprintf (buf, sizeof (buf),
              "WAIVED rule=%s marker_bbox_um=[%.4f,%.4f,%.4f,%.4f] waiver_box_um=[%.4f,%.4f,%.4f,%.4f]",
              r.name.c_str (), b.left () * m_dbu, b.bottom () * m_dbu,
              b.right () * m_dbu, b.top () * m_dbu, w->l, w->b, w->r, w->t);
    local_log.push_back (buf);
  };

  if (have_ep) {
    //  the driving count is ep.count(): filter edge pairs (and log here).
    db::EdgePairs kept;
    for (db::EdgePairs::const_iterator eit = ep.begin (); ! eit.at_end (); ++eit) {
      db::Box b = (*eit).bbox ();
      const SVRFEngine::WaiverBox *w = contained (b);
      if (w) { ++waived; logline (b, w); }
      else   { kept.insert (*eit); }
    }
    ep = kept;
  }

  //  filter the marker Region so the error layer excludes waived shapes. When
  //  have_ep the count already came from ep -> do NOT re-count / re-log here.
  db::Region keptr;
  for (db::Region::const_iterator vit = viol.begin (); ! vit.at_end (); ++vit) {
    db::Box b = (*vit).box ();
    const SVRFEngine::WaiverBox *w = contained (b);
    if (w) {
      if (! have_ep) { ++waived; logline (b, w); }
    } else {
      keptr.insert (*vit);
    }
  }
  viol = keptr;

  if (! local_log.empty ()) {
    tl::MutexLocker lock (&m_waiver_mx);
    for (size_t k = 0; k < local_log.size (); ++k) m_waiver_log.push_back (local_log[k]);
  }
}

void SVRFEngine::flush_waiver_audit () const
{
  if (m_waiver_log.empty ()) return;
  //  stderr trail (always) + optional file named by $SVRFDRC_WAIVER_AUDIT.
  for (size_t k = 0; k < m_waiver_log.size (); ++k) {
    fprintf (stderr, "%s\n", m_waiver_log[k].c_str ());
  }
  const char *ap = getenv ("SVRFDRC_WAIVER_AUDIT");
  if (ap && *ap) {
    std::ofstream af (ap);
    if (af) {
      for (size_t k = 0; k < m_waiver_log.size (); ++k) af << m_waiver_log[k] << "\n";
    }
  }
}

// ---------------------------------------------------------------------------
//  DFM scoring (#47): weighted soft-rule aggregate (advisory, post-processing)
// ---------------------------------------------------------------------------

void SVRFEngine::compute_dfm_score () const
{
  const char *wf = getenv ("SVRFDRC_DFM_WEIGHTS");
  if (! wf || ! *wf) return;
  std::ifstream f (wf);
  if (! f) return;
  std::map<std::string, double> weights;
  std::string line;
  while (std::getline (f, line)) {
    size_t h = line.find ('#');
    if (h != std::string::npos) line = line.substr (0, h);
    std::istringstream ss (line);
    std::string rn; double wt;
    if (ss >> rn >> wt) weights[rn] = wt;
  }
  if (weights.empty ()) return;

  //  Aggregate weight_i * viol_i over the SOFT rules, reading each rule's REAL
  //  violation count straight out of its frozen report slot (info == the count
  //  for PASS/FAIL; SKIP/ERROR contribute 0). No verdict is altered.
  double score = 0.0;
  int nsoft = 0;
  long total_viol = 0;
  std::vector<std::string> detail;
  for (std::vector<SVRFResult>::const_iterator it = m_results.begin (); it != m_results.end (); ++it) {
    if (! it->rule) continue;
    std::map<std::string, double>::const_iterator w = weights.find (it->rule->name);
    if (w == weights.end ()) continue;
    long cnt = 0;
    if (it->verdict == "FAIL" || it->verdict == "PASS") {
      cnt = strtol (it->info.c_str (), 0, 10);
    }
    double contrib = w->second * (double) cnt;
    score += contrib;
    total_viol += cnt;
    ++nsoft;
    char buf[256];
    snprintf (buf, sizeof (buf), "DFM soft rule=%s weight=%.4f viol=%ld contrib=%.4f",
              it->rule->name.c_str (), w->second, cnt, contrib);
    detail.push_back (buf);
  }

  char hdr[128];
  snprintf (hdr, sizeof (hdr), "DFM score=%.4f soft_rules=%d total_viol=%ld", score, nsoft, total_viol);
  fprintf (stderr, "%s\n", hdr);
  for (size_t k = 0; k < detail.size (); ++k) fprintf (stderr, "%s\n", detail[k].c_str ());

  const char *op = getenv ("SVRFDRC_DFM_OUT");
  if (op && *op) {
    std::ofstream o (op);
    if (o) {
      o << hdr << "\n";
      for (size_t k = 0; k < detail.size (); ++k) o << detail[k] << "\n";
    }
  }
}

// ---------------------------------------------------------------------------
//  RVE-style result database (#9): KLayout-loadable .lyrdb marker DB.
// ---------------------------------------------------------------------------

namespace {

//  minimal XML text escape for the handful of category/cell strings we emit.
static std::string xml_escape (const std::string &s)
{
  std::string o;
  o.reserve (s.size ());
  for (char c : s) {
    switch (c) {
      case '&':  o += "&amp;";  break;
      case '<':  o += "&lt;";   break;
      case '>':  o += "&gt;";   break;
      case '"':  o += "&quot;"; break;
      default:   o += c;        break;
    }
  }
  return o;
}

//  print a um coordinate the way KLayout's rdb value strings expect: fixed
//  decimals with trailing zeros (and a dangling '.') trimmed, so an integer
//  micron reads back "3" and a DBU-exact value reads back "3.8" / "0.25".
static std::string fmt_um (double v)
{
  char buf[64];
  snprintf (buf, sizeof (buf), "%.6f", v);
  std::string s (buf);
  std::string::size_type dot = s.find ('.');
  if (dot != std::string::npos) {
    std::string::size_type last = s.find_last_not_of ('0');
    if (last == dot) last = dot - 1;                 // strip the '.' too
    s.erase (last + 1);
  }
  if (s == "-0") s = "0";
  return s;
}

}  // namespace

void SVRFEngine::emit_rve_db () const
{
  const char *path = getenv ("SVRFDRC_RVE_OUT");
  if (! path || ! *path) return;
  std::ofstream o (path);
  if (! o) return;

  const std::string top = m_layout.cell_name (m_top);

  //  Collect FAILing rules in source order (dedup category names). Each rule's
  //  error markers live in m_regions[rule->name] -- the SAME frozen error layer
  //  the report counted, so the DB can never disagree with the verdict.
  std::vector<std::string> cats;                       // unique category (rule) names
  std::set<std::string> seen;
  for (std::vector<SVRFResult>::const_iterator it = m_results.begin (); it != m_results.end (); ++it) {
    if (! it->rule || it->verdict != "FAIL") continue;
    if (seen.insert (it->rule->name).second) cats.push_back (it->rule->name);
  }

  o << "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n";
  o << "<report-database>\n";
  o << " <description>svrfdrc native SVRF/DRC run</description>\n";
  o << " <original-file/>\n";
  o << " <generator>svrfdrc</generator>\n";
  o << " <top-cell>" << xml_escape (top) << "</top-cell>\n";
  o << " <tags>\n </tags>\n";

  o << " <categories>\n";
  for (size_t i = 0; i < cats.size (); ++i) {
    o << "  <category>\n";
    o << "   <name>" << xml_escape (cats[i]) << "</name>\n";
    o << "   <description/>\n";
    o << "   <categories>\n   </categories>\n";
    o << "  </category>\n";
  }
  o << " </categories>\n";

  o << " <cells>\n";
  o << "  <cell>\n";
  o << "   <name>" << xml_escape (top) << "</name>\n";
  o << "   <variant/>\n   <layout-name/>\n";
  o << "   <references>\n   </references>\n";
  o << "  </cell>\n";
  o << " </cells>\n";

  //  one <item> per error marker polygon, category = the rule name.
  long total_items = 0;
  o << " <items>\n";
  for (std::vector<SVRFResult>::const_iterator it = m_results.begin (); it != m_results.end (); ++it) {
    if (! it->rule || it->verdict != "FAIL") continue;
    std::map<std::string, db::Region>::const_iterator ri = m_regions.find (it->rule->name);
    if (ri == m_regions.end ()) continue;
    //  The item's <category> is a category PATH, not a raw name: KLayout's rdb
    //  reader splits it on "." (rdb::Categories::category_by_name). A foundry rule
    //  name is DOTTED by convention (e.g. "M1.S.1"), so it must be emitted the way
    //  KLayout's OWN rdb::Category::path() emits it -- quoted when it is not a bare
    //  word over [A-Za-z0-9_$]. Without this, every dotted rule produced a .lyrdb
    //  that KLayout itself refused to load ("... is not a valid category path").
    const std::string cat = xml_escape (tl::to_word_or_quoted_string (it->rule->name, "_$"));
    for (db::Region::const_iterator p = ri->second.begin (); ! p.at_end (); ++p) {
      db::Polygon poly = *p;
      o << "  <item>\n";
      o << "   <tags/>\n";
      o << "   <category>" << cat << "</category>\n";
      o << "   <cell>" << xml_escape (top) << "</cell>\n";
      o << "   <visited>false</visited>\n";
      o << "   <multiplicity>1</multiplicity>\n";
      o << "   <comment/>\n   <image/>\n";
      o << "   <values>\n";
      o << "    <value>polygon: (";
      bool first = true;
      for (db::Polygon::polygon_contour_iterator h = poly.begin_hull (); h != poly.end_hull (); ++h) {
        if (! first) o << ";";
        first = false;
        o << fmt_um ((*h).x () * m_dbu) << "," << fmt_um ((*h).y () * m_dbu);
      }
      o << ")</value>\n";
      o << "   </values>\n";
      o << "  </item>\n";
      ++total_items;
    }
  }
  o << " </items>\n";
  o << "</report-database>\n";

  fprintf (stderr, "SVRFDRC_RVE wrote %s categories=%zu items=%ld\n",
           path, cats.size (), total_items);
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
//  native in-engine ANTENNA (fork feature #20)
// ---------------------------------------------------------------------------
//
//  ANTENNA <metal> <gate> <cmp> <ratio> -- the charge-ratio SVRF op, evaluated
//  directly on db::LayoutToNetlist instead of being routed to a separate tool.
//  STAGED connectivity: at the etch stage of `metal` only the conductors AT OR
//  BELOW it participate (rank in the CONNECT graph, measured from the gate base),
//  so an upper-metal jumper deposited AFTER this etch cannot relieve the antenna.
//  Per net: ratio = area(metal on net) / area(gate on net); a net with no gate or
//  no metal at this stage is not an antenna node and is skipped. The report count
//  is the number of over-limit nets (PASS iff 0), matching every other rule.
void SVRFEngine::antenna_check (const SVRFRule &r, std::size_t slot)
{
  auto skip = [&] (const std::string &why) {
    SVRFResult res; res.rule = &r; res.verdict = "SKIP"; res.info = why;
    m_results[slot] = res;
  };
  if (m_deck.connects.empty ()) {
    skip ("ANTENNA needs a CONNECT stack for staged connectivity");
    return;
  }
  const std::string metal = r.layer1;
  const std::string gate  = r.layer2;

  //  --- CONNECT graph (undirected): node set + adjacency --------------------
  std::set<std::string> nodes;
  std::map<std::string, std::set<std::string> > adj;
  auto add_edge = [&] (const std::string &a, const std::string &b) {
    if (a.empty () || b.empty ()) return;
    nodes.insert (a); nodes.insert (b);
    adj[a].insert (b); adj[b].insert (a);
  };
  for (std::vector<std::tuple<std::string, std::string, std::string> >::const_iterator c = m_deck.connects.begin (); c != m_deck.connects.end (); ++c) {
    const std::string &a = std::get<0> (*c), &b = std::get<1> (*c), &v = std::get<2> (*c);
    if (! v.empty ()) { add_edge (a, v); add_edge (b, v); }
    else { add_edge (a, b); }
  }
  if (! nodes.count (metal)) {
    skip ("ANTENNA metal layer not in the CONNECT stack");
    return;
  }

  //  --- gate base: the CONNECT node the gate denominator rides --------------
  //  gate = poly AND active is a bool/bool_expr derivation; its operand that is a
  //  CONNECT-stack conductor (the poly) is the base. A gate that is itself a
  //  conductor rides itself.
  std::string base;
  for (std::vector<SVRFDerivation>::const_iterator dv = m_deck.derivations.begin (); dv != m_deck.derivations.end (); ++dv) {
    if (dv->name == gate && (dv->kind == "bool" || dv->kind == "bool_expr")) {
      for (std::vector<std::string>::const_iterator op = dv->operands.begin (); op != dv->operands.end (); ++op) {
        if (nodes.count (*op)) { base = *op; break; }
      }
      break;
    }
  }
  if (base.empty () && nodes.count (gate)) {
    base = gate;
  }
  if (base.empty ()) {
    skip ("ANTENNA gate base layer not resolvable to a CONNECT node");
    return;
  }

  //  --- BFS rank from the base (height above the gate) ----------------------
  std::map<std::string, int> rank;
  {
    std::vector<std::string> q;
    rank[base] = 0; q.push_back (base);
    for (std::size_t h = 0; h < q.size (); ++h) {
      const std::string u = q[h];
      const std::set<std::string> &nb = adj[u];
      for (std::set<std::string>::const_iterator w = nb.begin (); w != nb.end (); ++w) {
        if (! rank.count (*w)) { rank[*w] = rank[u] + 1; q.push_back (*w); }
      }
    }
  }
  if (! rank.count (metal)) {
    skip ("ANTENNA metal layer not reachable from the gate base");
    return;
  }
  const int mrank = rank[metal];
  auto in_stage = [&] (const std::string &n) -> bool {
    std::map<std::string, int>::const_iterator it = rank.find (n);
    return it != rank.end () && it->second <= mrank;
  };

  //  --- build a PRIVATE staged L2N (only conductors AT OR BELOW `metal`) -----
  db::LayoutToNetlist l2n ("TOP", m_dbu);
  std::map<std::string, db::Region> reg;         // stable: std::map never invalidates nodes
  auto get = [&] (const std::string &nm) -> db::Region & {
    std::map<std::string, db::Region>::iterator it = reg.find (nm);
    if (it != reg.end ()) return it->second;
    reg[nm] = resolve (nm);                       // main-thread only: cache mutation is safe
    db::Region &ref = reg[nm];
    l2n.register_layer (ref, nm);
    l2n.connect (ref);                            // net-bearing (intra-layer)
    return ref;
  };
  for (std::set<std::string>::const_iterator n = nodes.begin (); n != nodes.end (); ++n) {
    if (in_stage (*n)) { get (*n); }
  }
  //  the gate denominator rides the base net (inter-layer connect, not self-net)
  reg["__gate__"] = resolve (gate);
  db::Region &greg = reg["__gate__"];
  l2n.register_layer (greg, "__gate__");
  l2n.connect (greg, get (base));
  //  apply CONNECT edges restricted to the stage (a via bridges only if in-stage)
  for (std::vector<std::tuple<std::string, std::string, std::string> >::const_iterator c = m_deck.connects.begin (); c != m_deck.connects.end (); ++c) {
    const std::string &a = std::get<0> (*c), &b = std::get<1> (*c), &v = std::get<2> (*c);
    if (! in_stage (a) || ! in_stage (b)) continue;
    if (! v.empty () && in_stage (v)) {
      l2n.connect (get (a), get (v));
      l2n.connect (get (b), get (v));
    } else {
      l2n.connect (get (a), get (b));
    }
  }
  db::Region &mreg = get (metal);

  bool built = true;
  try {
    l2n.extract_netlist ();
  } catch (tl::Exception &e) {
    SVRFResult res; res.rule = &r; res.verdict = "ERROR";
    std::string m = e.msg (); if (m.size () > 80) m = m.substr (0, 80);
    res.info = m; m_results[slot] = res; built = false;
  } catch (...) {
    SVRFResult res; res.rule = &r; res.verdict = "ERROR";
    res.info = "antenna extract error"; m_results[slot] = res; built = false;
  }
  if (! built) return;

  //  --- per-net ratio = area(metal on net) / area(gate on net) --------------
  auto cmp_bad = [&] (double ratio) -> bool {
    if (r.cmp == "<")  return ratio <  r.value;
    if (r.cmp == "<=") return ratio <= r.value;
    if (r.cmp == ">=") return ratio >= r.value;
    if (r.cmp == "==") return ratio == r.value;
    if (r.cmp == "!=") return ratio != r.value;
    return ratio > r.value;                        // ">" default: antenna over-limit
  };
  std::size_t viol = 0;
  double worst = 0.0;
  db::Netlist *nl = l2n.netlist ();
  if (nl) {
    for (db::Netlist::circuit_iterator ci = nl->begin_circuits (); ci != nl->end_circuits (); ++ci) {
      for (db::Circuit::net_iterator net = ci->begin_nets (); net != ci->end_nets (); ++net) {
        std::unique_ptr<db::Region> sg (l2n.shapes_of_net (*net, greg, true));
        double ga = sg ? (double) sg->area () : 0.0;
        if (ga <= 0.0) continue;                   // no gate -> not an antenna node
        std::unique_ptr<db::Region> sm (l2n.shapes_of_net (*net, mreg, true));
        double ma = sm ? (double) sm->area () : 0.0;
        if (ma <= 0.0) continue;                   // net carries no metal at this stage
        double ratio = ma / ga;                    // dimensionless (dbu^2 cancels)
        if (ratio > worst) worst = ratio;
        if (cmp_bad (ratio)) ++viol;
      }
    }
  }

  if (getenv ("SVRFDRC_ANTENNA_RATIO")) {
    fprintf (stderr, "ANTENNA_RATIO %s metal=%s gate=%s stage_rank<=%d worst=%.6f cmp=%s limit=%.6f viol=%zu\n",
             r.name.c_str (), metal.c_str (), gate.c_str (), mrank,
             worst, r.cmp.c_str (), r.value, viol);
  }
  SVRFResult res; res.rule = &r;
  res.verdict = viol == 0 ? "PASS" : "FAIL";
  res.info = std::to_string (viol);
  m_results[slot] = res;
}

// ---------------------------------------------------------------------------
//  native ERC (fork feature #13)
// ---------------------------------------------------------------------------
//
//  Electrical rule checks that are DERIVABLE FROM GEOMETRY + the deck's own
//  CONNECT stack -- no schematic, no netlist input:
//
//    ERC FLOATING <layer> <tie>   a net that carries <layer> shapes but NEVER
//                                 reaches a <tie> shape (floating gate / missing
//                                 tie-down / undriven node).
//    ERC UNCONNECTED <layer>      a net whose ONLY conductor is <layer> itself --
//                                 an isolated island wired to nothing.
//
//  Both are solved on a PRIVATE db::LayoutToNetlist built from the CONNECT stack,
//  so the shared m_l2n (and therefore every pre-existing rule's verdict) is never
//  perturbed. The reported count is the number of offending NETS; the violation
//  region is the offending <layer> geometry, so waivers and the RVE marker DB
//  anchor to it exactly like any other rule.

void SVRFEngine::connect_graph (std::set<std::string> &nodes,
                                std::map<std::string, std::set<std::string> > &adj) const
{
  for (std::vector<std::tuple<std::string, std::string, std::string> >::const_iterator c = m_deck.connects.begin (); c != m_deck.connects.end (); ++c) {
    const std::string &a = std::get<0> (*c), &b = std::get<1> (*c), &v = std::get<2> (*c);
    nodes.insert (a); nodes.insert (b);
    if (! v.empty ()) {
      nodes.insert (v);
      adj[a].insert (v); adj[v].insert (a);
      adj[b].insert (v); adj[v].insert (b);
    } else {
      adj[a].insert (b); adj[b].insert (a);
    }
  }
}

std::string SVRFEngine::layer_base (const std::string &name, const std::set<std::string> &nodes) const
{
  if (nodes.count (name)) {
    return name;
  }
  for (std::vector<SVRFDerivation>::const_iterator dv = m_deck.derivations.begin (); dv != m_deck.derivations.end (); ++dv) {
    if (dv->name == name && (dv->kind == "bool" || dv->kind == "bool_expr")) {
      for (std::vector<std::string>::const_iterator op = dv->operands.begin (); op != dv->operands.end (); ++op) {
        if (nodes.count (*op)) {
          return *op;
        }
      }
      break;
    }
  }
  return std::string ();
}

void SVRFEngine::exec_erc (const SVRFRule &r, std::size_t slot)
{
  auto skip = [&] (const std::string &why) {
    SVRFResult res; res.rule = &r; res.verdict = "SKIP"; res.info = why;
    m_results[slot] = res;
  };
  if (m_deck.connects.empty ()) {
    skip ("ERC needs a CONNECT stack for connectivity");
    return;
  }

  std::set<std::string> nodes;
  std::map<std::string, std::set<std::string> > adj;
  connect_graph (nodes, adj);

  //  Every operand must be tie-able to the CONNECT stack, else there is no
  //  electrical question to answer -> honest SKIP, never PASS.
  const std::string base1 = layer_base (r.layer1, nodes);
  if (base1.empty ()) {
    skip ("ERC layer '" + r.layer1 + "' not resolvable to a CONNECT node");
    return;
  }
  std::string base2;
  if (! r.layer2.empty ()) {
    base2 = layer_base (r.layer2, nodes);
    if (base2.empty ()) {
      skip ("ERC layer '" + r.layer2 + "' not resolvable to a CONNECT node");
      return;
    }
  }

  //  --- private L2N over the WHOLE CONNECT stack ---------------------------
  //  Materialise EVERY operand region on the shared cache BEFORE any of them is
  //  handed to the private L2N: db::LayoutToNetlist::register_layer() adopts the
  //  region it is given, and a later resolve() of a derivation whose operands
  //  were already adopted would evaluate to empty.
  resolve (r.layer1);
  if (! r.layer2.empty ()) { resolve (r.layer2); }
  for (std::set<std::string>::const_iterator n = nodes.begin (); n != nodes.end (); ++n) {
    resolve (*n);
  }

  db::LayoutToNetlist l2n ("TOP", m_dbu);
  std::map<std::string, db::Region> reg;          // std::map: node pointers stay stable
  auto get = [&] (const std::string &nm) -> db::Region & {
    std::map<std::string, db::Region>::iterator it = reg.find (nm);
    if (it != reg.end ()) return it->second;
    reg[nm] = resolve (nm);                        // main-thread only (ERC never parallelised)
    db::Region &ref = reg[nm];
    l2n.register_layer (ref, nm);
    l2n.connect (ref);                             // net-bearing (intra-layer)
    return ref;
  };
  for (std::set<std::string>::const_iterator n = nodes.begin (); n != nodes.end (); ++n) {
    get (*n);
  }
  for (std::vector<std::tuple<std::string, std::string, std::string> >::const_iterator c = m_deck.connects.begin (); c != m_deck.connects.end (); ++c) {
    const std::string &a = std::get<0> (*c), &b = std::get<1> (*c), &v = std::get<2> (*c);
    if (! v.empty ()) {
      l2n.connect (get (a), get (v));
      l2n.connect (get (b), get (v));
    } else {
      l2n.connect (get (a), get (b));
    }
  }
  //  A derived marker (e.g. gate = poly AND active) is NOT a conductor: register
  //  it as a probe layer riding its base node, so shapes_of_net can report it.
  auto probe = [&] (const std::string &name, const std::string &base) -> db::Region & {
    if (name == base) {
      return get (name);                           // the layer IS a stack conductor
    }
    const std::string key = "__probe_" + name + "__";
    std::map<std::string, db::Region>::iterator it = reg.find (key);
    if (it != reg.end ()) return it->second;
    reg[key] = resolve (name);
    db::Region &ref = reg[key];
    l2n.register_layer (ref, key);
    l2n.connect (ref, get (base));                 // rides the base net; never bridges
    return ref;
  };
  db::Region &p1 = probe (r.layer1, base1);
  db::Region *p2 = 0;
  if (! r.layer2.empty ()) {
    p2 = &probe (r.layer2, base2);
  }

  try {
    l2n.extract_netlist ();
  } catch (tl::Exception &e) {
    SVRFResult res; res.rule = &r; res.verdict = "ERROR";
    std::string m = e.msg (); if (m.size () > 80) m = m.substr (0, 80);
    res.info = m; m_results[slot] = res; return;
  } catch (...) {
    SVRFResult res; res.rule = &r; res.verdict = "ERROR";
    res.info = "ERC extract error"; m_results[slot] = res; return;
  }

  //  --- per-net verdict ----------------------------------------------------
  const bool dbg = getenv ("SVRFDRC_ERCNET") != 0;
  const bool floating = (r.erc_check == "FLOATING");
  db::Region viol;
  std::size_t nviol = 0;
  db::Netlist *nl = l2n.netlist ();
  if (nl) {
    for (db::Netlist::circuit_iterator ci = nl->begin_circuits (); ci != nl->end_circuits (); ++ci) {
      for (db::Circuit::net_iterator net = ci->begin_nets (); net != ci->end_nets (); ++net) {
        std::unique_ptr<db::Region> s1 (l2n.shapes_of_net (*net, p1, true));
        if (! s1 || s1->empty ()) {
          continue;                                // this net does not carry layer1
        }
        bool bad = false;
        if (floating) {
          std::unique_ptr<db::Region> s2 (l2n.shapes_of_net (*net, *p2, true));
          bad = (! s2 || s2->empty ());            // never reaches the tie -> floating
        } else {
          //  UNCONNECTED: the net must carry NO conductor other than layer1's base.
          bad = true;
          for (std::set<std::string>::const_iterator n = nodes.begin (); n != nodes.end () && bad; ++n) {
            if (*n == base1) continue;
            std::unique_ptr<db::Region> so (l2n.shapes_of_net (*net, get (*n), true));
            if (so && ! so->empty ()) {
              bad = false;                         // some other conductor is on this net
            }
          }
        }
        if (dbg) {
          fprintf (stderr, "ERCNET %s check=%s net=%s layer1_shapes=%zu -> %s\n",
                   r.name.c_str (), r.erc_check.c_str (), net->expanded_name ().c_str (),
                   (size_t) s1->count (), bad ? "VIOLATION" : "ok");
        }
        if (bad) {
          ++nviol;
          viol += *s1;                             // the offending geometry itself
        }
      }
    }
  }

  //  waiver suppression applies to ERC violations too (geometry-anchored).
  db::EdgePairs no_ep;
  std::size_t waived = 0;
  std::size_t before = viol.count ();
  maybe_apply_waivers (r, no_ep, false, viol, waived);
  if (waived > 0 && before > 0) {
    //  a waived marker removes its net from the count (never inflates it)
    std::size_t after = viol.count ();
    std::size_t drop = (before - after);
    nviol = (drop >= nviol) ? 0 : (nviol - drop);
  }

  m_regions[r.name] = viol;
  SVRFResult res; res.rule = &r;
  res.verdict = nviol == 0 ? "PASS" : "FAIL";
  res.info = std::to_string (nviol);
  m_results[slot] = res;
}

// ---------------------------------------------------------------------------
//  voltage-aware / net-voltage-dependent spacing (fork feature #12)
// ---------------------------------------------------------------------------
//
//    VOLTAGE <marker-layer> <volts>          (deck statement, source order)
//    VSPACE  <layer> <cmp> <base> PER_VOLT <k>
//
//  The spacing REQUIRED between two DIFFERENT nets is
//
//      required(i,j) = base + k * |V_i - V_j|      [um]
//
//  and the pair is a violation when the measured spacing is BELOW it. Each net's
//  voltage is read from geometry: a net that touches a shape on a VOLTAGE marker
//  layer is held at that layer's volts. This is the geometry-derivable half of
//  Calibre's voltage-dependent DRC -- no schematic, no annotated netlist.
//
//  Deliberately CONSERVATIVE where the layout is ambiguous, so the check can
//  never be false-clean:
//    * a net touching SEVERAL markers takes the LARGEST |V| -> the LARGEST
//      required spacing;
//    * a net touching NO marker is treated as 0 V (the base rule still applies).
//
//  Solved on a PRIVATE db::LayoutToNetlist (same discipline as ANTENNA #20 and
//  ERC #13), so the shared m_l2n and every pre-existing verdict are untouched.
//  Pairs are enumerated over NETS (not shapes) with a bbox reject at the pair's
//  own required distance, so only nets that could possibly interact are checked.

void SVRFEngine::exec_vspace (const SVRFRule &r, std::size_t slot)
{
  auto skip = [&] (const std::string &why) {
    SVRFResult res; res.rule = &r; res.verdict = "SKIP"; res.info = why;
    m_results[slot] = res;
  };
  if (! r.has_per_volt) {
    //  no voltage term -> this is just EXTERNAL; refuse to masquerade as one.
    skip ("VSPACE needs a PER_VOLT modifier");
    return;
  }
  if (m_deck.connects.empty ()) {
    skip ("VSPACE needs a CONNECT stack for connectivity");
    return;
  }
  if (m_deck.voltages.empty ()) {
    skip ("VSPACE needs at least one VOLTAGE domain");
    return;
  }

  std::set<std::string> nodes;
  std::map<std::string, std::set<std::string> > adj;
  connect_graph (nodes, adj);
  const std::string base1 = layer_base (r.layer1, nodes);
  if (base1.empty ()) {
    skip ("VSPACE layer '" + r.layer1 + "' not resolvable to a CONNECT node");
    return;
  }

  //  Materialise every operand on the shared cache BEFORE the private L2N adopts
  //  any of them (register_layer takes over the region it is handed).
  resolve (r.layer1);
  for (std::set<std::string>::const_iterator n = nodes.begin (); n != nodes.end (); ++n) {
    resolve (*n);
  }
  std::vector<std::pair<db::Region, double> > vmarks;      // marker geometry -> volts
  for (std::vector<std::pair<std::string, double> >::const_iterator v = m_deck.voltages.begin (); v != m_deck.voltages.end (); ++v) {
    if (m_unmodeled.count (v->first)) {
      continue;                                            // unmodeled marker: ignored, never assumed
    }
    db::Region vr;
    try {
      vr = resolve (v->first);
    } catch (...) {
      continue;
    }
    if (! vr.empty ()) {
      vmarks.push_back (std::make_pair (vr, v->second));
    }
  }
  if (vmarks.empty ()) {
    skip ("no VOLTAGE marker layer carries geometry");
    return;
  }

  db::LayoutToNetlist l2n ("TOP", m_dbu);
  std::map<std::string, db::Region> reg;
  auto get = [&] (const std::string &nm) -> db::Region & {
    std::map<std::string, db::Region>::iterator it = reg.find (nm);
    if (it != reg.end ()) return it->second;
    reg[nm] = resolve (nm);
    db::Region &ref = reg[nm];
    l2n.register_layer (ref, nm);
    l2n.connect (ref);
    return ref;
  };
  for (std::set<std::string>::const_iterator n = nodes.begin (); n != nodes.end (); ++n) {
    get (*n);
  }
  for (std::vector<std::tuple<std::string, std::string, std::string> >::const_iterator c = m_deck.connects.begin (); c != m_deck.connects.end (); ++c) {
    const std::string &a = std::get<0> (*c), &b = std::get<1> (*c), &v = std::get<2> (*c);
    if (! v.empty ()) {
      l2n.connect (get (a), get (v));
      l2n.connect (get (b), get (v));
    } else {
      l2n.connect (get (a), get (b));
    }
  }
  //  the measured layer, as a probe when it is a derived marker rather than a node
  db::Region *meas = 0;
  if (r.layer1 == base1) {
    meas = &get (r.layer1);
  } else {
    const std::string key = "__probe_" + r.layer1 + "__";
    reg[key] = resolve (r.layer1);
    db::Region &ref = reg[key];
    l2n.register_layer (ref, key);
    l2n.connect (ref, get (base1));
    meas = &ref;
  }

  try {
    l2n.extract_netlist ();
  } catch (tl::Exception &e) {
    SVRFResult res; res.rule = &r; res.verdict = "ERROR";
    std::string m = e.msg (); if (m.size () > 80) m = m.substr (0, 80);
    res.info = m; m_results[slot] = res; return;
  } catch (...) {
    SVRFResult res; res.rule = &r; res.verdict = "ERROR";
    res.info = "VSPACE extract error"; m_results[slot] = res; return;
  }

  //  --- per-net measured shapes + voltage ----------------------------------
  struct VNet
  {
    db::Region shapes;                   // this net's shapes on the MEASURED layer
    double volts;
    db::Box bbox;
    std::string name;
  };
  const bool dbg = getenv ("SVRFDRC_VSPACE") != 0;
  std::vector<VNet> vnets;
  db::Netlist *nl = l2n.netlist ();
  if (nl) {
    for (db::Netlist::circuit_iterator ci = nl->begin_circuits (); ci != nl->end_circuits (); ++ci) {
      for (db::Circuit::net_iterator net = ci->begin_nets (); net != ci->end_nets (); ++net) {
        std::unique_ptr<db::Region> sm (l2n.shapes_of_net (*net, *meas, true));
        if (! sm || sm->empty ()) {
          continue;                      // net carries none of the measured layer
        }
        //  every conductor shape of this net -- a voltage marker may be drawn over
        //  any layer of the domain, not only the measured one.
        db::Region all (*sm);
        for (std::set<std::string>::const_iterator n = nodes.begin (); n != nodes.end (); ++n) {
          std::unique_ptr<db::Region> so (l2n.shapes_of_net (*net, get (*n), true));
          if (so && ! so->empty ()) {
            all += *so;
          }
        }
        double volts = 0.0;              // unmarked net: the base rule still applies
        bool marked = false;
        for (std::size_t k = 0; k < vmarks.size (); ++k) {
          if (! all.selected_interacting (vmarks[k].first).empty ()) {
            //  CONSERVATIVE on ambiguity: the largest magnitude wins, so a net in
            //  two domains demands the LARGEST spacing, never the smallest.
            if (! marked || std::fabs (vmarks[k].second) > std::fabs (volts)) {
              volts = vmarks[k].second;
            }
            marked = true;
          }
        }
        VNet vn;
        vn.shapes = *sm;
        vn.volts = volts;
        vn.bbox = sm->bbox ();
        vn.name = net->expanded_name ();
        vnets.push_back (vn);
        if (dbg) {
          fprintf (stderr, "VSPACE %s net=%s shapes=%zu volts=%.6f%s\n",
                   r.name.c_str (), vn.name.c_str (), (size_t) vn.shapes.count (),
                   volts, marked ? "" : " (unmarked)");
        }
      }
    }
  }

  //  --- pairwise different-net check at the pair's OWN required spacing -----
  db::RegionCheckOptions o = check_options (r);
  db::EdgePairs ep;
  db::Region viol;
  try {
    for (std::size_t i = 0; i < vnets.size (); ++i) {
      for (std::size_t j = i + 1; j < vnets.size (); ++j) {
        double req = r.value + r.per_volt * std::fabs (vnets[i].volts - vnets[j].volts);
        db::Coord d = to_dbu (req);
        if (d <= 0) {
          continue;
        }
        db::Box bi = vnets[i].bbox.enlarged (db::Vector (d, d));
        if (! bi.touches (vnets[j].bbox)) {
          continue;                      // cannot interact within this pair's reach
        }
        db::EdgePairs pe = vnets[i].shapes.separation_check (vnets[j].shapes, d, o);
        if (dbg && pe.count () > 0) {
          fprintf (stderr, "VSPACE %s pair %s(%.6fV)/%s(%.6fV) required=%.6f -> %zu\n",
                   r.name.c_str (), vnets[i].name.c_str (), vnets[i].volts,
                   vnets[j].name.c_str (), vnets[j].volts, req, (size_t) pe.count ());
        }
        ep += pe;
      }
    }
  } catch (tl::Exception &e) {
    SVRFResult res; res.rule = &r; res.verdict = "ERROR";
    std::string m = e.msg (); if (m.size () > 80) m = m.substr (0, 80);
    res.info = m; m_results[slot] = res; return;
  } catch (...) {
    SVRFResult res; res.rule = &r; res.verdict = "ERROR";
    res.info = "VSPACE check error"; m_results[slot] = res; return;
  }
  ep.polygons (viol);

  std::size_t waived = 0;
  maybe_apply_waivers (r, ep, true, viol, waived);
  std::size_t cnt = ep.count ();

  m_regions[r.name] = viol;
  SVRFResult res; res.rule = &r;
  res.verdict = cnt == 0 ? "PASS" : "FAIL";
  res.info = std::to_string (cnt);
  m_results[slot] = res;
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
      if (r.has_per_volt) {
        tag += ",per_volt=" + py_float_str (r.per_volt);   // #12: only VSPACE sets it
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

    //  eqDRC (#8): PROPERTY renders its measured expression inline so the report
    //  stays auditable (only PROPERTY rules take this branch -> no other op moves).
    if (r.op == "PROPERTY") {
      out << vpad << " " << npad << " PROPERTY " << r.layer1 << " { " << r.prop_expr << " } "
          << r.cmp << " " << py_float_str (r.value) << " " << tag << " -> " << it->info << "\n";
      continue;
    }

    //  ERC (#13) carries no cmp/value -- it states the error condition itself.
    //  Only ERC rules take this branch, so no other op's line moves.
    if (r.op == "ERC") {
      out << vpad << " " << npad << " ERC " << r.erc_check << " " << r.layer1;
      if (! r.layer2.empty ()) {
        out << "/" << r.layer2;
      }
      out << " " << tag << " -> " << it->info << "\n";
      continue;
    }

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
