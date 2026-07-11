
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
  void exec_derivation (const SVRFDerivation &d);
  db::Region eval_bool_expr (const std::string &expr, std::vector<std::string> &used);
  db::Region metric_select (const SVRFDerivation &d);
  db::Region rectangles_of (const SVRFDerivation &d);
  db::Region vertex_of (const SVRFDerivation &d);
  db::Edges  build_edges (const SVRFDerivation &d);
  db::Edges  coincident_edges (const db::Edges &ea, const db::Edges &eb, int want); // want: 1 inside, 0 outside, -1 none

  //  -- checks (Engine._exec_rule / _exec_edge_rule / _exec_density) -------
  void exec_rule (const SVRFRule &r);
  void exec_edge_rule (const SVRFRule &r);
  void exec_density (const SVRFRule &r);
  db::RegionCheckOptions check_options (const SVRFRule &r, bool allow_filters = true) const;
  db::EdgesCheckOptions  edge_check_options (const SVRFRule &r) const;
  bool inputs_unmodeled (const SVRFRule &r) const;
  bool is_edge_rule (const SVRFRule &r) const;

  //  -- connectivity (Engine._build_l2n / _l2n_nets / _net_area_ratio) -----
  void build_l2n ();
  db::Region net_area_ratio (const SVRFDerivation &d);

  //  count of edge pairs converted to a violation count (EdgePairs::count)
  static size_t ep_count (const db::EdgePairs &ep) { return ep.count (); }
};

}  // namespace db

#endif
