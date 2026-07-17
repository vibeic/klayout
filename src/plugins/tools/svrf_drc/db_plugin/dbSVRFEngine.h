
/*

  dbSVRFEngine.h -- native C++ port of svrf_klayout/run_svrf_drc.py

  Executes a parsed Calibre/SVRF DRC deck (db::SVRFDeck) DIRECTLY on KLayout's
  own db:: geometry engine -- db::Region / db::Edges / db::EdgePairs checks and
  db::LayoutToNetlist connectivity -- with NO Python/Ruby/pya interpreter and NO
  intermediate .drc file. This is the C++ equivalent of the reference
  `run_svrf_drc.py::Engine`, one-to-one:

      pya.Region.space_check     -> db::Region::space_check
      pya.Region.width_check     -> db::Region::width_check
      pya.Region.separation_check-> db::Region::separation_check
      pya.Region.notch_check     -> db::Region::notch_check
      pya.Region.overlap_check   -> db::Region::overlap_check
      pya.Region.enclosing_check -> db::Region::enclosing_check
      pya.Region.with_area/...   -> db::Region::filtered(db::RegionAreaFilter/...)
      pya.Region.interacting/... -> db::Region::selected_interacting/...
      pya.LayoutToNetlist        -> db::LayoutToNetlist

  Report format is BYTE-IDENTICAL to run_svrf_drc.py::main() -- the vibe-ic
  plugin's _parse_svrf_tally / _classify_svrf_fails parse those exact lines.
  Freeze it; see NATIVE_CPP_PORT_ROADMAP.md "HARD CONTRACTS".

  honest-SKIP doctrine (never false-PASS): ANTENNA / edge-typed-without-polygon
  -equivalent / connectivity-without-CONNECT-stack / net layer with no extracted
  shapes / unmodeled derivations -> emit "SKIP", never "PASS".

  Contains NO vendor data.

*/

#ifndef HDR_dbSVRFEngine
#define HDR_dbSVRFEngine

#include "dbSVRFDeck.h"

#include "dbLayout.h"
#include "dbRegion.h"
#include "dbEdges.h"
#include "dbEdgePairs.h"
#include "dbLayoutToNetlist.h"

#include <string>
#include <vector>
#include <map>
#include <set>
#include <memory>
#include <utility>
#include <cstddef>

namespace db
{

//  One line of the report: a measurement rule or COPY, with its verdict.
//    verdict -- "PASS" | "FAIL" | "SKIP" | "ERROR"
//    info    -- for PASS/FAIL: the violation count (as text); for SKIP/ERROR: the reason
struct SVRFResult
{
  std::string verdict;
  const SVRFRule *rule = 0;      // points into SVRFDeck::rules (source-order rule)
  std::string info;             // count (PASS/FAIL) or reason (SKIP/ERROR)
};

//  The interpreter. Mirrors run_svrf_drc.py::Engine field-for-field:
//    regions      name -> db::Region (drawn / derived / error layer)
//    edges_ns     name -> db::Edges  (edge-typed derived layers)
//    edge_layers  names that are Edges layers
//    unmodeled    names whose layer is genuinely unsupported (-> dependent rule SKIP)
//    results      (verdict, rule, info) for measurement + COPY statements
class SVRFEngine
{
public:
  //  Reads `layout_path` into an owned db::Layout. `top_cell_name` selects the top
  //  cell when the layout has multiple tops (else the single top_cell is used and a
  //  multi-top layout is an honest error -- pass an explicit cell). `deck` is the
  //  already-parsed deck (parse_deck of the .rule text).
  SVRFEngine (const std::string &layout_path, const SVRFDeck &deck,
              const std::string &top_cell_name = std::string ());

  //  Run every statement in source order (derivations + rules, then COPY). Returns
  //  the accumulated results (also available via results()).
  const std::vector<SVRFResult> &execute ();

  //  Number of worker threads for the parallel MEASUREMENT-rule phase. N<=1 (the
  //  default) reproduces today's EXACT serial behaviour byte-for-byte -- every
  //  rule runs inline in source order and no thread is spawned. N>1 dispatches the
  //  independent measurement rules (not DENSITY / not connectivity / not consumed
  //  by a later derivation) across a worker pool AFTER a single-threaded
  //  pre-realization pass; the emitted report is order-stable (slot-indexed) and
  //  byte-identical to the N=1 report regardless of scheduling.
  void set_threads (int n) { m_threads = n; }
  int threads () const { return m_threads; }

  //  --- cell-aware FEOL over-fire exemption (OPT-IN; Route B, fork fix #2) ----
  //  An EMPTY config path (the DEFAULT) => the exemption is DISABLED and NONE of
  //  the code below runs, so the emitted report is BYTE-IDENTICAL to the
  //  threading-only (#1) binary. When a config path is set the engine, at the
  //  head of execute(), builds a per-PLACED-INSTANCE EXACT footprint index (from
  //  the qualified-master library GDS + the DEF placements named in the config)
  //  and, for ONLY the FEOL space/notch rules named in the config, DROPS an error
  //  edge-pair IFF BOTH its edges lie STRICTLY INTERIOR to a SINGLE qualified
  //  master's exact placed footprint AND no top-level (non-cell-interior) FEOL
  //  shape forms it. This is a CONSERVATIVE lower bound: on ANY doubt the
  //  violation is KEPT (the exemption never changes an inter-cell / boundary /
  //  top-level verdict -> never false-clean). See dbSVRFEngine.cc for the exact
  //  discriminator and its safety argument.
  void set_cell_aware_feol (const std::string &cfg_path) { m_feol_cfg_path = cfg_path; }
  const std::string &cell_aware_feol () const { return m_feol_cfg_path; }

  //  Emit the frozen report to `report_path`. `klayout_version` fills the header
  //  "# SVRF-native DRC via KLayout <ver>" line (empty is allowed -> trailing space
  //  is stripped, matching the Python `.rstrip()`).
  void write_report (const std::string &report_path, const std::string &deck_path,
                     const std::string &layout_path,
                     const std::string &klayout_version) const;

  const std::vector<SVRFResult> &results () const { return m_results; }
  double dbu () const { return m_dbu; }

private:
  db::Layout m_layout;
  db::cell_index_type m_top;
  double m_dbu = 0.001;
  const SVRFDeck &m_deck;

  std::map<std::string, db::Region> m_regions;
  std::map<std::string, db::Edges>  m_edges_ns;
  std::set<std::string> m_edge_layers;
  std::set<std::string> m_unmodeled;
  std::vector<SVRFResult> m_results;
  int m_threads = 1;                 // worker count for the parallel rule phase

  //  -- cell-aware FEOL exemption state (fork fix #2) ----------------------
  //  Built ONCE by setup_cell_aware_feol() at the head of execute() and only
  //  READ during the (possibly parallel) rule phase -> safe to share across
  //  worker threads. m_feol_enabled stays false unless a config path was set.
  std::string m_feol_cfg_path;                         // "" => disabled (byte-identical)
  bool m_feol_enabled = false;
  db::Coord m_feol_strict = 1;                          // strict-interior margin (dbu, >=1)
  std::set<std::string> m_feol_rules;                  // rule names the exemption applies to
  std::vector<std::pair<int, int> > m_feol_gds;        // raw FEOL (gds,datatype) layers (for the top-level guard)
  std::map<std::string, db::Region> m_feol_master;         // master -> local exact footprint (all-layer union)
  std::map<std::string, db::Region> m_feol_master_strict;  // master -> footprint eroded inward by m_feol_strict
  struct FeolInst
  {
    const db::Region *foot;        // -> m_feol_master[master]        (std::map node ptr: stable)
    const db::Region *foot_strict; // -> m_feol_master_strict[master]
    db::Trans trans;               // placement transform (DEF orient + origin), dbu
    db::Box pbox;                  // placed-footprint bbox (fast reject)
  };
  std::vector<FeolInst> m_feol_insts;                  // one per placed qualified instance
  //  Merged union of every placed qualified master's RAW FEOL geometry (the
  //  m_feol_gds layers) -- NOT the all-layer footprint. The top-level guard
  //  subtracts THIS from the rule's FEOL so that a top-level / non-qualified
  //  FEOL shard sitting INSIDE a cell's footprint rectangle still shows up as
  //  top-level (a footprint-rectangle subtraction would wrongly erase it).
  db::Region m_feol_qual_feol;

  //  built lazily from the CONNECT stack for connectivity / net-area-ratio rules
  std::unique_ptr<db::LayoutToNetlist> m_l2n;
  std::map<std::string, db::Region> m_l2n_layers;   // name -> registered Region copy
  bool m_l2n_built = false;

  //  -- namespace resolution (Engine._drawn / .resolve) --------------------
  //  Build a Region from the drawn (layer,datatype) purposes bound to `name`, or
  //  null when `name` is not a drawn layer.
  bool drawn (const std::string &name, db::Region &out) const;
  db::Region &resolve (const std::string &name);        // drawn|derived|error|empty
  db::Edges as_edges (const std::string &name);
  db::Coord to_dbu (double um) const;

  //  -- derivations (Engine._exec_derivation and helpers) ------------------
  //  When run on a worker thread (derivation-parallel phase) the two set-typed
  //  side effects (m_unmodeled / m_edge_layers structural inserts) MUST NOT touch
  //  the shared std::set concurrently. Pass per-worker sinks: the names are pushed
  //  to the sink (a thread-local vector) and merged into the shared set single-
  //  threaded at the level barrier. nullptr sinks (the serial threads<=1 path)
  //  reproduce the exact in-place set inserts -> byte-identical.
  void exec_derivation (const SVRFDerivation &d,
                        std::vector<std::string> *unmodeled_sink = 0,
                        std::vector<std::string> *edge_layer_sink = 0);
  db::Region eval_bool_expr (const std::string &expr, std::vector<std::string> &used);
  db::Region metric_select (const SVRFDerivation &d);
  db::Region rectangles_of (const SVRFDerivation &d);
  db::Region vertex_of (const SVRFDerivation &d);
  db::Edges  build_edges (const SVRFDerivation &d);
  db::Edges  coincident_edges (const db::Edges &ea, const db::Edges &eb, int want); // want: 1 inside, 0 outside, -1 none

  //  -- checks (Engine._exec_rule / _exec_edge_rule / _exec_density) -------
  //  Each writes its single report line into m_results[slot] (a pre-assigned,
  //  source-order slot) rather than push_back, so the report byte-order is
  //  identical regardless of whether the rule ran inline or on a worker thread.
  void exec_rule (const SVRFRule &r, std::size_t slot);
  void exec_edge_rule (const SVRFRule &r, std::size_t slot);
  void exec_density (const SVRFRule &r, std::size_t slot);
  db::RegionCheckOptions check_options (const SVRFRule &r, bool allow_filters = true) const;
  db::EdgesCheckOptions  edge_check_options (const SVRFRule &r) const;
  bool inputs_unmodeled (const SVRFRule &r) const;
  bool is_edge_rule (const SVRFRule &r) const;

  //  -- Route B, Phase 1: spatial tiling of finite-reach EXTERNAL checks ----
  //  gate: engage the tiled space/separation path (op EXTERNAL, non-connectivity)
  //  when --threads>1 (or SVRFDRC_TILE_SPACE=N>0 forces N tiles/axis; =0 disables).
  bool tiled_space_enabled (const SVRFRule &r) const;
  //  Byte-identical parallel space_check/separation_check by spatial tiles. The
  //  operands are merged ONCE (single-threaded) so the tiling unit is the WHOLE
  //  flat polygon; each tile collects the whole polygons within `border` (the
  //  rule's finite reach) of its core via selected_interacting -- never a cut
  //  fragment -- and runs the IDENTICAL flat check on that sub-region. Every tile
  //  that sees both operands of a violation therefore computes the byte-identical
  //  edge pair, and an EXACT-geometric dedup at merge collapses those duplicates
  //  to exactly the flat edge-pair set. Result (and hence the report COUNT) is
  //  independent of grid and thread count -> byte-identical to the flat path.
  db::EdgePairs tiled_external_check (db::Region &l1, const db::Region *l2,
                                      db::Coord d, const db::RegionCheckOptions &o) const;

  //  -- parallel measurement-rule phase -----------------------------------
  //  Single-threaded pre-realization: resolve every parallel-rule input on the
  //  main thread (so std::map is never structurally mutated under threads) and
  //  force each input region's lazy merged/bbox caches valid (so worker copies
  //  only READ shared state). Pre-create each rule's error-layer slot too.
  void prewarm_for_parallel (const std::vector<std::pair<const SVRFRule *, std::size_t> > &par);
  //  Dispatch the pre-realized rules across m_threads workers (dynamic grab via
  //  an atomic index). Each worker calls exec_rule(r, slot) into its own slot.
  void run_parallel (const std::vector<std::pair<const SVRFRule *, std::size_t> > &par);

  //  -- topological-level DERIVATION parallelism (fork fix #3) -------------
  //  Extends #1 (rule-check threading) to the DOMINANT serial cost: the ~15911
  //  derivation builds. The derivations + the rule error layers they consume form
  //  a data-dependency DAG (a statement reads layers produced by earlier ones).
  //  execute_leveled() topologically LEVELS every statement (level 0 = reads only
  //  drawn layers; level k reads only levels <k), then for each level runs the
  //  DERIVATIONS of that level in PARALLEL on the #1 worker pool (an inter-level
  //  BARRIER guarantees producers finish before consumers start) while running the
  //  RULES of that level exactly as the serial path would (main-thread inline, or
  //  deferred to the #1 parallel-rule pass / COPY pass -- filled into `parallel` /
  //  `copies`). Only entered when m_threads>1; the threads<=1 path is the original
  //  serial source-order loop, byte-identical to HEAD. The parallel path writes the
  //  identical m_results for any thread count (every layer is deterministic in its
  //  committed lower-level inputs; report slots are fixed source-order ranks).
  void execute_leveled (std::size_t n_noncopy,
                        const std::map<std::string, int> &name_count,
                        const std::map<std::string, std::size_t> &last_deriv_ref,
                        std::vector<std::pair<const SVRFRule *, std::size_t> > &parallel,
                        std::vector<std::pair<const SVRFRule *, std::size_t> > &copies,
                        bool timing);
  //  Single-threaded realization of a set of INPUT layer names before a parallel
  //  derivation level: resolve() each (so std::map is never structurally mutated
  //  under workers) and force its lazy merged/bbox caches valid (so worker copies
  //  only READ already-filled state). Region names warm m_regions[name]; edge-typed
  //  names additionally warm the stored m_edges_ns[name].
  void prewarm_names (const std::set<std::string> &names);
  //  Dispatch a batch of mutually-independent (same-level) derivations across the
  //  worker pool; each worker executes exec_derivation() into its own pre-created
  //  m_regions/m_edges_ns slot with a thread-local unmodeled/edge sink, then warms
  //  its own output. Sinks are merged into the shared sets single-threaded on join.
  void run_parallel_derivations (const std::vector<const SVRFDerivation *> &batch);

  //  -- cell-aware FEOL exemption (fork fix #2) ----------------------------
  //  Parse the config, read the master library GDS + DEF, and build the exact
  //  per-placed-instance footprint index. A no-op (leaves m_feol_enabled false)
  //  when no config path was set -> byte-identical default. Runs single-threaded
  //  at the head of execute(), before any rule.
  void setup_cell_aware_feol ();
  //  Return a copy of `ep` with the qualified-cell-interior over-fire edge-pairs
  //  removed. CONSERVATIVE: an edge-pair is dropped ONLY when its error bridge is
  //  strictly interior to a SINGLE placed qualified footprint and no top-level
  //  FEOL shape forms it; anything ambiguous is kept. Called ONLY for rules whose
  //  name is in m_feol_rules, after drop_coincident_pairs.
  db::EdgePairs feol_exempt_filter (const SVRFRule &r, const db::EdgePairs &ep);

  //  -- connectivity (Engine._build_l2n / _l2n_nets / _net_area_ratio) -----
  void build_l2n ();
  db::Region net_area_ratio (const SVRFDerivation &d);

  //  count of edge pairs converted to a violation count (EdgePairs::count)
  static size_t ep_count (const db::EdgePairs &ep) { return ep.count (); }
};

}  // namespace db

#endif
