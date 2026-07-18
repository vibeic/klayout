
/*

  dbSVRFDeck.h -- self-contained C++17 port of svrf_klayout/svrf_parse.py

  Reads a Calibre/SVRF-format DRC deck into interpreter-ready rule + derivation
  objects. This is a faithful, byte-parity port of the reference Python parser:
  it emits no other language and depends only on the C++ standard library
  (<regex> for the ECMAScript patterns) -- NO KLayout / pya / db headers -- so it
  builds with plain g++ today and drops into the KLayout build later.

  Contains NO vendor data.

*/

#ifndef HDR_dbSVRFDeck
#define HDR_dbSVRFDeck

#include <string>
#include <vector>
#include <map>
#include <utility>
#include <tuple>

namespace db
{

//  connectivity qualifier on a measurement rule
//    none      -- no net awareness
//    same      -- CONNECTED     (same net)
//    different -- NOT CONNECTED (different nets)
//  NOTE: named SVRFConnectivity (not Connectivity) to avoid colliding with the
//  existing db::Connectivity class (dbHierNetworkProcessor.h) once this header is
//  compiled inside the KLayout db tree.
enum class SVRFConnectivity { none, same, different };

//  A geometric measurement (rule). Modifiers are carried as native-check
//  parameters (metrics / ignore_angle / opposite / projection / connectivity)
//  exactly as in the reference Rule dataclass.
struct SVRFRule
{
  std::string name;
  std::string op;                        // EXTERNAL | INTERNAL | ENCLOSURE | NOTCH | AREA | ...
  std::string layer1;
  std::string layer2;                    // empty == None
  std::string cmp;
  double value = 0.0;

  std::string metrics = "euclidian";     // euclidian | projection | square
  bool has_ignore_angle = false;
  double ignore_angle = 0.0;             // ABUT angle -> ignore_angle
  bool has_min_projection = false;
  double min_projection = 0.0;
  bool has_max_projection = false;
  double max_projection = 0.0;
  bool opposite = false;                 // OPPOSITE -> opposite_filter
  bool whole_edges = false;              // WHOLE -> whole_edges
  bool has_shielded = false;             // SHIELDED / TRANSPARENT
  bool shielded = false;
  bool region_out = false;               // REGION -> output polygons
  bool singular = false;
  SVRFConnectivity connectivity = SVRFConnectivity::none;
  bool has_window = false;
  double window = 0.0;                    // DENSITY WINDOW w (um)
  bool has_step = false;
  double step = 0.0;                      // DENSITY STEP s (um)
  //  eqDRC (#8): equation-based DRC. When op == "PROPERTY", prop_expr holds the
  //  per-shape numeric expression over the built-in measured properties
  //  (AREA, PERIMETER, WIDTH, HEIGHT of the shape bounding box); the shape is a
  //  violation when   eval(prop_expr) <cmp> value   is TRUE (Calibre convention:
  //  the rule states the ERROR condition). Empty for every other op.
  std::string prop_expr;
  bool supported = true;
  std::string reason;
  std::string raw;
};

//  A layer-derivation assignment (produces a derived layer, not an error output).
struct SVRFDerivation
{
  std::string name;
  std::string kind;                      // bool|bool_expr|size|select|passthrough|empty|
                                         // holes|rectangles|extents|merge|metric_select|
                                         // vertex|net_ratio|with_edge|expand|edge|unknown
  std::string expr;
  std::vector<std::string> operands;
  std::string bool_sym;                  // & | - ^   (empty == None)
  std::string size_op;                   // SIZE | GROW | SHRINK | ... (empty == None)
  bool has_value = false;
  double value = 0.0;
  std::string select_op;                 // (empty == None)
  std::string metric;                    // AREA | LENGTH | ANGLE | PERIMETER (empty == None)
  bool has_lo = false;                   // bounds = (lo, hi)
  double lo = 0.0;
  bool has_hi = false;
  double hi = 0.0;
  bool has_neq = false;
  double neq = 0.0;
  //  miscellaneous per-branch params (net_ratio cmp/thr, expand/edge direction
  //  flags, rectangle bbox); mirrors the reference `params` dict. Not part of the
  //  canonical dump, so kept as opaque string key/values.
  std::map<std::string, std::string> params;
  bool edge_typed = false;               // result is edges, not polygons
  bool supported = true;
  std::string reason;
  std::string raw;
};

//  A statement in SOURCE ORDER -- a tagged reference into either the rules or the
//  derivations vector (one unified namespace, executed top-to-bottom).
struct SVRFStatement
{
  enum Kind { Rule, Derivation } kind;
  size_t index;                          // index into SVRFDeck::rules or ::derivations
  //  explicit ctors: the default-member-initializer form disqualifies aggregate
  //  init under -std=c++11 (the KLayout build standard), so provide constructors.
  SVRFStatement () : kind (Rule), index (0) { }
  SVRFStatement (Kind k, size_t i) : kind (k), index (i) { }
};

struct SVRFDeck
{
  std::map<std::string, std::vector<std::pair<int, int> > > layers;
  std::vector<SVRFDerivation> derivations;
  std::vector<SVRFRule> rules;
  //  (layer_a, layer_b, via) -- via empty == None
  std::vector<std::tuple<std::string, std::string, std::string> > connects;
  std::vector<SVRFStatement> statements;
};

//  Parse a Calibre/SVRF deck. Mirrors svrf_parse.parse_deck:
//    strip_comments -> preprocess (#DEFINE/#IFDEF flag stack) -> parse_layers ->
//    connects -> named blocks + assignments in source order.
SVRFDeck parse_deck (const std::string &text);

//  Individual stages, exposed for testing (mirror the Python module functions).
std::string strip_comments (const std::string &text);
std::string preprocess (const std::string &text);
std::map<std::string, std::vector<std::pair<int, int> > > parse_layers (const std::string &text);

}  // namespace db

#endif
