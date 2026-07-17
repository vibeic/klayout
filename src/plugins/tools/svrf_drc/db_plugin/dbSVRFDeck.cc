
/*

  dbSVRFDeck.cc -- self-contained C++17 port of svrf_klayout/svrf_parse.py

  Byte-parity port of the reference Python parser. Standard library only
  (<regex> for the ECMAScript patterns). Contains NO vendor data.

*/

#include "dbSVRFDeck.h"

#include <regex>
#include <set>
#include <sstream>
#include <cmath>
#include <algorithm>

namespace db
{

// ── small text helpers ──────────────────────────────────────────────────────

//  Split like Python str.splitlines(): on '\n' (and a trailing '\r' stripped for
//  '\r\n'), with no trailing empty element when the text ends in a newline.
static std::vector<std::string> splitlines (const std::string &text)
{
  std::vector<std::string> out;
  std::string cur;
  for (char c : text) {
    if (c == '\n') {
      if (!cur.empty () && cur.back () == '\r') {
        cur.pop_back ();
      }
      out.push_back (cur);
      cur.clear ();
    } else {
      cur.push_back (c);
    }
  }
  if (!cur.empty ()) {
    if (cur.back () == '\r') {
      cur.pop_back ();
    }
    out.push_back (cur);
  }
  return out;
}

//  Split on runs of ASCII whitespace, dropping empties (like Python str.split()).
static std::vector<std::string> split_ws (const std::string &s)
{
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v') {
      if (!cur.empty ()) { out.push_back (cur); cur.clear (); }
    } else {
      cur.push_back (c);
    }
  }
  if (!cur.empty ()) { out.push_back (cur); }
  return out;
}

static std::string to_upper (const std::string &s)
{
  std::string r (s);
  std::transform (r.begin (), r.end (), r.begin (),
                  [] (unsigned char c) { return (char) std::toupper (c); });
  return r;
}

static std::string join (const std::vector<std::string> &v, const std::string &sep)
{
  std::string r;
  for (size_t i = 0; i < v.size (); ++i) {
    if (i) { r += sep; }
    r += v[i];
  }
  return r;
}

static int count_char (const std::string &s, char c)
{
  return (int) std::count (s.begin (), s.end (), c);
}

// ── shared token classifiers ────────────────────────────────────────────────

//  fullmatch [A-Za-z_][\w.$:]* — the ":" admits SVRF temp names
//  (name:tmp); without it `X:tmp = ...` parses as an assignment to X and
//  silently OVERWRITES the base layer (commercial wide-metal chains).
static const std::regex &ident_re ()
{
  static const std::regex re ("[A-Za-z_][\\w.$:]*", std::regex::ECMAScript);
  return re;
}

//  fullmatch -?[0-9]*\.?[0-9]+
static const std::regex &number_re ()
{
  static const std::regex re ("-?[0-9]*\\.?[0-9]+", std::regex::ECMAScript);
  return re;
}

static bool is_identifier (const std::string &t)
{
  return std::regex_match (t, ident_re ());
}

static bool is_number (const std::string &t)
{
  return std::regex_match (t, number_re ());
}

//  _BOOL_OPS = {"AND":"&","OR":"|","NOT":"-","XOR":"^"}
static const std::map<std::string, std::string> &bool_ops ()
{
  static const std::map<std::string, std::string> m = {
    {"AND", "&"}, {"OR", "|"}, {"NOT", "-"}, {"XOR", "^"}};
  return m;
}

static const std::set<std::string> &size_ops ()
{
  static const std::set<std::string> s = {
    "SIZE", "GROW", "SHRINK", "OVERSIZE", "UNDERSIZE"};
  return s;
}

static const std::set<std::string> &select_ops ()
{
  static const std::set<std::string> s = {
    "INTERACT", "INSIDE", "OUTSIDE", "TOUCH", "CUT", "ENCLOSE"};
  return s;
}

//  _UNARY_LAYER_OPS
static const std::map<std::string, std::string> &unary_layer_ops ()
{
  static const std::map<std::string, std::string> m = {
    {"HOLES", "holes"}, {"RECTANGLE", "rectangles"}, {"RECTANGLES", "rectangles"},
    {"EXTENT", "extents"}, {"EXTENTS", "extents"}, {"MERGE", "merge"},
    {"MERGED", "merge"}, {"DRAWN", "passthrough"}, {"COPY", "passthrough"}};
  return m;
}

static const std::set<std::string> &edge_ops ()
{
  static const std::set<std::string> s = {
    "COIN", "COINCIDENT", "STRAIGHT", "CONVEX", "ACUTE", "SNAP", "SHADOW"};
  return s;
}

static const std::set<std::string> &metric_ops ()
{
  static const std::set<std::string> s = {"AREA", "PERIMETER", "LENGTH", "ANGLE"};
  return s;
}

//  _KEYWORDS = bool | size | select | unary | edge | metric | {literals}
static const std::set<std::string> &keywords ()
{
  static const std::set<std::string> s = [] () {
    std::set<std::string> k;
    for (auto &kv : bool_ops ()) { k.insert (kv.first); }
    for (auto &v : size_ops ()) { k.insert (v); }
    for (auto &v : select_ops ()) { k.insert (v); }
    for (auto &kv : unary_layer_ops ()) { k.insert (kv.first); }
    for (auto &v : edge_ops ()) { k.insert (v); }
    for (auto &v : metric_ops ()) { k.insert (v); }
    for (const char *v : {"BY", "WITH", "EMPTY", "ONLY", "NET", "AS", "TO", "OF",
                          "IN", "OPPOSITE", "PROJECTING", "REGION", "SINGULAR",
                          "ABUT", "EXTENDED", "EDGE", "EDGES", "EXPAND", "VERTEX",
                          "ASPECT", "RATIO", "CENTERS", "PATH", "INNER", "OUTER",
                          "TOUCH", "BOTH", "STEP", "OVERUNDER", "UNDEROVER"}) {
      k.insert (v);
    }
    return k;
  } ();
  return s;
}

static bool is_keyword (const std::string &upper_tok)
{
  return keywords ().count (upper_tok) != 0;
}

// ── strip_comments ──────────────────────────────────────────────────────────

std::string strip_comments (const std::string &text)
{
  //  _BLOCK_COMMENT = /\*.*?\*/ (DOTALL) -> [\s\S] for a newline-spanning body;
  //  _LINE_COMMENT  = //[^\n]*   ; block comments removed first, then line ones.
  static const std::regex block (R"(/\*[\s\S]*?\*/)", std::regex::ECMAScript);
  static const std::regex line (R"(//[^\n]*)", std::regex::ECMAScript);
  std::string t = std::regex_replace (text, block, "");
  t = std::regex_replace (t, line, "");
  return t;
}

// ── preprocess (flag-based #DEFINE/#IFDEF stack, NOT text substitution) ───────

std::string preprocess (const std::string &text)
{
  static const std::regex pp (
    R"(^\s*#\s*(DEFINE|UNDEF|IFDEF|IFNDEF|ELSE|ELIF|ENDIF)\b(.*)$)",
    std::regex::ECMAScript | std::regex::icase);

  std::set<std::string> defines;
  //  each frame: {active_here, any_branch_taken}
  std::vector<std::pair<bool, bool> > stack;

  auto parent_active = [&] () -> bool {
    for (auto &f : stack) { if (!f.first) { return false; } }
    return true;
  };
  auto parent_active_below_top = [&] () -> bool {
    for (size_t i = 0; i + 1 < stack.size (); ++i) {
      if (!stack[i].first) { return false; }
    }
    return true;
  };

  std::vector<std::string> out;
  for (const std::string &linestr : splitlines (text)) {
    std::smatch m;
    if (std::regex_search (linestr, m, pp, std::regex_constants::match_continuous)) {
      std::string drv = to_upper (m[1].str ());
      std::vector<std::string> arg = split_ws (m[2].str ());
      std::string sym = arg.empty () ? std::string () : arg[0];
      if (drv == "IFDEF") {
        bool cond = parent_active () && defines.count (sym);
        stack.push_back ({cond, cond});
      } else if (drv == "IFNDEF") {
        bool cond = parent_active () && !defines.count (sym);
        stack.push_back ({cond, cond});
      } else if (drv == "ELIF") {
        if (!stack.empty ()) {
          bool taken = stack.back ().second;
          bool parent = parent_active_below_top ();
          bool cond = parent && (!taken) && defines.count (sym);
          stack.back () = {cond, taken || cond};
        }
      } else if (drv == "ELSE") {
        if (!stack.empty ()) {
          bool taken = stack.back ().second;
          bool parent = parent_active_below_top ();
          stack.back () = {parent && (!taken), true};
        }
      } else if (drv == "ENDIF") {
        if (!stack.empty ()) { stack.pop_back (); }
      } else if (drv == "DEFINE") {
        if (parent_active () && !sym.empty ()) { defines.insert (sym); }
      } else if (drv == "UNDEF") {
        if (parent_active () && !sym.empty ()) { defines.erase (sym); }
      }
      continue;                            // the directive line is consumed
    }
    if (parent_active ()) {
      out.push_back (linestr);
    }
  }
  return join (out, "\n");
}

// ── parse_layers (DIRECT + two-level LAYER MAP) ──────────────────────────────

std::map<std::string, std::vector<std::pair<int, int> > > parse_layers (const std::string &text)
{
  static const std::regex layer_direct (
    R"(^\s*LAYER\s+([A-Za-z_][\w.$]*)\s+(\d+)\s+(\d+)\s*$)",
    std::regex::ECMAScript | std::regex::icase);
  static const std::regex layer_index (
    R"(^\s*LAYER\s+([A-Za-z_][\w.$]*)\s+(\d+)\s*$)",
    std::regex::ECMAScript | std::regex::icase);
  static const std::regex layer_map (
    R"(^\s*LAYER\s+MAP\s+(\d+)\s+DATATYPE\s+(\d+)\s+(\d+)\s*$)",
    std::regex::ECMAScript | std::regex::icase);

  std::vector<std::string> lines = splitlines (text);

  //  index -> [(gds, dt), ...]  (union order preserved)
  std::map<int, std::vector<std::pair<int, int> > > idx_to_gds;
  for (const std::string &ln : lines) {
    std::smatch m;
    if (std::regex_match (ln, m, layer_map)) {
      int gds = std::stoi (m[1].str ());
      int dt = std::stoi (m[2].str ());
      int idx = std::stoi (m[3].str ());
      idx_to_gds[idx].push_back ({gds, dt});
    }
  }

  std::map<std::string, std::vector<std::pair<int, int> > > out;
  for (const std::string &ln : lines) {           // DIRECT: name gds datatype
    std::smatch m;
    if (std::regex_match (ln, m, layer_direct)) {
      out[m[1].str ()] = { {std::stoi (m[2].str ()), std::stoi (m[3].str ())} };
    }
  }
  for (const std::string &ln : lines) {           // TWO-LEVEL: name index (+ MAP)
    std::smatch m;
    if (std::regex_match (ln, m, layer_index)) {
      std::string name = m[1].str ();
      int idx = std::stoi (m[2].str ());
      if (out.count (name)) {
        continue;                                 // a direct binding already won
      }
      auto it = idx_to_gds.find (idx);
      if (it != idx_to_gds.end ()) {
        out[name] = it->second;
      } else {
        out[name] = { {idx, 0} };
      }
    }
  }
  return out;
}

// ── connects ─────────────────────────────────────────────────────────────────

static std::vector<std::tuple<std::string, std::string, std::string> >
parse_connects (const std::string &text)
{
  static const std::regex connect_re (
    R"(^\s*S?CONNECT\s+([A-Za-z_][\w.$]*)\s+([A-Za-z_][\w.$]*)(?:\s+BY\s+([A-Za-z_][\w.$]*))?)",
    std::regex::ECMAScript | std::regex::icase);
  std::vector<std::tuple<std::string, std::string, std::string> > out;
  for (const std::string &ln : splitlines (text)) {
    std::smatch m;
    if (std::regex_search (ln, m, connect_re, std::regex_constants::match_continuous)) {
      out.push_back (std::make_tuple (m[1].str (), m[2].str (),
                                      m[3].matched ? m[3].str () : std::string ()));
    }
  }
  return out;
}

// ── rule modifiers ──────────────────────────────────────────────────────────

//  _MEAS_RE groups: 1=op 2=layer1 3=layer2? 4=cmp 5=value 6=tail
static const std::regex &meas_re ()
{
  static const std::regex re (
    R"(\b(INTERNAL|INT|EXTERNAL|EXT|ENCLOSURE|ENC|WIDTH|SPACE|AREA|NOTCH|DENSITY|ANTENNA)\b\s+)"
    R"(([A-Za-z_][\w.$]*)(?:\s+([A-Za-z_][\w.$]*))?\s*)"
    R"((<=|<|==|>=|>)\s*([0-9]*\.?[0-9]+)\s*(.*)$)",
    std::regex::ECMAScript | std::regex::icase);
  return re;
}

static void parse_modifiers (SVRFRule &rule, const std::string &tail)
{
  static const std::regex re_proj (
    R"(\bPROJECTING\b|\bPARA(?:LLEL)?\s+ONLY\b|\bPARALLEL\b)",
    std::regex::ECMAScript | std::regex::icase);
  static const std::regex re_square (R"(\bSQUARE\b)", std::regex::ECMAScript | std::regex::icase);
  //  _ABUT_RE: capture the upper angle bound
  static const std::regex re_abut (
    R"(\bABUT\b[^\n]*?([0-9]+(?:\.[0-9]+)?)(?:\s*<\s*([0-9]+(?:\.[0-9]+)?))?)",
    std::regex::ECMAScript | std::regex::icase);
  static const std::regex re_projlen (
    R"(\bPROJECTING\b\s*(?:<=|<|>=|>)?\s*([0-9]*\.?[0-9]+))",
    std::regex::ECMAScript | std::regex::icase);
  static const std::regex re_opposite (R"(\bOPPOSITE\b)", std::regex::ECMAScript | std::regex::icase);
  static const std::regex re_whole (R"(\bWHOLE\b)", std::regex::ECMAScript | std::regex::icase);
  static const std::regex re_shielded (R"(\bSHIELDED\b)", std::regex::ECMAScript | std::regex::icase);
  static const std::regex re_transparent (R"(\bTRANSPARENT\b)", std::regex::ECMAScript | std::regex::icase);
  static const std::regex re_region (R"(\bREGION\b)", std::regex::ECMAScript | std::regex::icase);
  static const std::regex re_singular (R"(\bSINGULAR\b)", std::regex::ECMAScript | std::regex::icase);
  static const std::regex re_notconn (R"(\bNOT\s+CONNECTED\b)", std::regex::ECMAScript | std::regex::icase);
  static const std::regex re_conn (R"(\bCONNECTED\b)", std::regex::ECMAScript | std::regex::icase);

  std::smatch m;
  if (std::regex_search (tail, re_proj)) {
    rule.metrics = "projection";
  }
  if (std::regex_search (tail, re_square)) {
    rule.metrics = "square";
  }
  if (std::regex_search (tail, m, re_abut)) {
    std::string v = m[2].matched ? m[2].str () : m[1].str ();
    rule.has_ignore_angle = true;
    rule.ignore_angle = std::stod (v);
  }
  if (std::regex_search (tail, m, re_projlen)) {
    rule.has_max_projection = true;
    rule.max_projection = std::stod (m[1].str ());
  }
  if (std::regex_search (tail, re_opposite)) {
    rule.opposite = true;
  }
  if (std::regex_search (tail, re_whole)) {
    rule.whole_edges = true;
  }
  if (std::regex_search (tail, re_shielded)) {
    rule.has_shielded = true;
    rule.shielded = true;
  } else if (std::regex_search (tail, re_transparent)) {
    rule.has_shielded = true;
    rule.shielded = false;
  }
  if (std::regex_search (tail, re_region)) {
    rule.region_out = true;
  }
  if (std::regex_search (tail, re_singular)) {
    rule.singular = true;
  }
  if (std::regex_search (tail, re_notconn)) {
    rule.connectivity = SVRFConnectivity::different;
  } else if (std::regex_search (tail, re_conn)) {
    rule.connectivity = SVRFConnectivity::same;
  }
}

//  _OPCANON = {"EXT":"EXTERNAL","INT":"INTERNAL","ENC":"ENCLOSURE","WIDTH":"INTERNAL"}
static std::string opcanon (const std::string &op)
{
  static const std::map<std::string, std::string> m = {
    {"EXT", "EXTERNAL"}, {"INT", "INTERNAL"}, {"ENC", "ENCLOSURE"}, {"WIDTH", "INTERNAL"}};
  auto it = m.find (op);
  return it != m.end () ? it->second : op;
}

static SVRFRule make_rule (const std::string &name, const std::smatch &meas)
{
  std::string op = to_upper (meas[1].str ());
  op = opcanon (op);

  SVRFRule r;
  r.name = name;
  r.op = op;
  r.layer1 = meas[2].str ();
  r.layer2 = meas[3].matched ? meas[3].str () : std::string ();
  r.cmp = meas[4].str ();
  r.value = std::stod (meas[5].str ());
  std::string full = meas[0].str ();
  //  raw = meas.group(0).strip()
  {
    size_t a = full.find_first_not_of (" \t\r\n\f\v");
    size_t b = full.find_last_not_of (" \t\r\n\f\v");
    r.raw = (a == std::string::npos) ? std::string () : full.substr (a, b - a + 1);
  }
  std::string tail = meas[6].matched ? meas[6].str () : std::string ();
  parse_modifiers (r, tail);

  if (op == "DENSITY") {
    static const std::regex re_window (R"(\bWINDOW\s+(-?[0-9]*\.?[0-9]+))",
                                       std::regex::ECMAScript | std::regex::icase);
    static const std::regex re_step (R"(\bSTEP\s+(-?[0-9]*\.?[0-9]+))",
                                     std::regex::ECMAScript | std::regex::icase);
    std::smatch mw, ms;
    if (std::regex_search (tail, mw, re_window)) {
      r.has_window = true;
      r.window = std::stod (mw[1].str ());
    }
    if (std::regex_search (tail, ms, re_step)) {
      r.has_step = true;
      r.step = std::stod (ms[1].str ());
    } else {
      //  step defaults to window
      r.has_step = r.has_window;
      r.step = r.window;
    }
  } else if (op == "ANTENNA") {
    r.supported = false;
    r.reason = "ANTENNA (charge ratio) routed to antenna checker, not geometric core";
  }
  return r;
}

// ── derivation helpers ──────────────────────────────────────────────────────

//  _layer_toks: identifiers that are not keywords
static std::vector<std::string> layer_toks (const std::vector<std::string> &toks)
{
  std::vector<std::string> out;
  for (const std::string &t : toks) {
    if (is_identifier (t) && !is_keyword (to_upper (t))) {
      out.push_back (t);
    }
  }
  return out;
}

//  re.findall(r'\(|\)|[^\s()]+', expr)
static std::vector<std::string> raw_tokens (const std::string &expr)
{
  std::vector<std::string> out;
  size_t i = 0, n = expr.size ();
  while (i < n) {
    char c = expr[i];
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v') {
      ++i;
    } else if (c == '(' || c == ')') {
      out.push_back (std::string (1, c));
      ++i;
    } else {
      size_t j = i;
      while (j < n) {
        char d = expr[j];
        if (d == ' ' || d == '\t' || d == '\n' || d == '\r' || d == '\f' || d == '\v'
            || d == '(' || d == ')') {
          break;
        }
        ++j;
      }
      out.push_back (expr.substr (i, j - i));
      i = j;
    }
  }
  return out;
}

//  _pure_boolean
static bool pure_boolean (const std::vector<std::string> &rawtoks)
{
  bool has_op = false;
  for (const std::string &t : rawtoks) {
    std::string u = to_upper (t);
    if (t == "(" || t == ")") {
      continue;
    }
    if (bool_ops ().count (u)) {
      has_op = true;
      continue;
    }
    if (is_identifier (t) && !is_keyword (u)) {
      continue;
    }
    return false;
  }
  return has_op;
}

//  _rel_bounds: fill (lo=mn, hi=mx) from (<=|<|>=|>|==) N occurrences
static void rel_bounds (const std::string &expr, bool &has_lo, double &lo,
                        bool &has_hi, double &hi)
{
  static const std::regex re (R"((<=|<|>=|>|==)\s*(-?[0-9]*\.?[0-9]+))",
                              std::regex::ECMAScript);
  has_lo = has_hi = false;
  lo = hi = 0.0;
  auto begin = std::sregex_iterator (expr.begin (), expr.end (), re);
  auto end = std::sregex_iterator ();
  for (auto it = begin; it != end; ++it) {
    std::string rel = (*it)[1].str ();
    double v = std::stod ((*it)[2].str ());
    if (rel == "<" || rel == "<=") {
      has_hi = true; hi = v;
    } else if (rel == ">" || rel == ">=") {
      has_lo = true; lo = v;
    } else {                               // ==
      has_lo = has_hi = true; lo = hi = v;
    }
  }
}

//  _neq
static bool find_neq (const std::string &expr, double &neq)
{
  static const std::regex re (R"(!=\s*(-?[0-9]*\.?[0-9]+))", std::regex::ECMAScript);
  std::smatch m;
  if (std::regex_search (expr, m, re)) {
    neq = std::stod (m[1].str ());
    return true;
  }
  return false;
}

//  _nums
static std::vector<double> nums (const std::vector<std::string> &toks)
{
  std::vector<double> out;
  for (const std::string &t : toks) {
    if (is_number (t)) {
      out.push_back (std::stod (t));
    }
  }
  return out;
}

// ── _parse_derivation ───────────────────────────────────────────────────────

static SVRFDerivation parse_derivation (const std::string &name, const std::string &expr)
{
  //  toks = expr.replace("(", " ").replace(")", " ").split()
  std::string flat = expr;
  std::replace (flat.begin (), flat.end (), '(', ' ');
  std::replace (flat.begin (), flat.end (), ')', ' ');
  std::vector<std::string> toks = split_ws (flat);
  std::vector<std::string> up;
  up.reserve (toks.size ());
  for (const std::string &t : toks) { up.push_back (to_upper (t)); }
  std::set<std::string> U (up.begin (), up.end ());

  SVRFDerivation d;
  d.name = name;
  d.kind = "unknown";
  d.expr = expr;
  {
    size_t a = expr.find_first_not_of (" \t\r\n\f\v");
    size_t b = expr.find_last_not_of (" \t\r\n\f\v");
    d.raw = (a == std::string::npos) ? std::string () : expr.substr (a, b - a + 1);
  }

  auto has = [&] (const char *k) { return U.count (k) != 0; };

  if (toks.empty ()) {
    d.supported = false; d.reason = "empty rhs"; return d;
  }
  if (toks.size () == 1 && up[0] == "EMPTY") {
    d.kind = "empty"; return d;
  }

  //  Calibre PREFIX boolean: <OP> layerA layerB [...]
  if (bool_ops ().count (up[0]) && toks.size () >= 3) {
    bool all_ok = true;
    for (size_t i = 1; i < toks.size (); ++i) {
      if (!is_identifier (toks[i]) || is_keyword (to_upper (toks[i]))) {
        all_ok = false; break;
      }
    }
    if (all_ok) {
      d.kind = "bool";
      d.bool_sym = bool_ops ().at (up[0]);
      d.operands.assign (toks.begin () + 1, toks.end ());
      return d;
    }
  }

  //  pure boolean expression (any nesting over plain layers)
  std::vector<std::string> rawtoks = raw_tokens (expr);
  if (pure_boolean (rawtoks)) {
    d.kind = "bool_expr";
    for (const std::string &t : rawtoks) {
      if (t == "(" || t == ")") { continue; }
      if (bool_ops ().count (to_upper (t))) { continue; }
      d.operands.push_back (t);
    }
    return d;
  }

  std::vector<std::string> lt = layer_toks (toks);

  //  COPY / DRAWN <layer> -> alias  (COPY empty -> empty)
  if (up[0] == "COPY" || up[0] == "DRAWN") {
    if (has ("EMPTY") || lt.empty ()) {
      d.kind = "empty";
    } else {
      d.kind = "passthrough";
      d.operands = {lt[0]};
    }
    return d;
  }

  //  A NET AREA RATIO B <cmp> N
  if (has ("NET") && has ("RATIO")) {
    d.kind = "net_ratio";
    for (size_t i = 0; i < lt.size () && i < 2; ++i) { d.operands.push_back (lt[i]); }
    static const std::regex re (R"((<=|>=|==|!=|<|>)\s*(-?[0-9]*\.?[0-9]+))",
                                std::regex::ECMAScript);
    std::smatch m;
    if (std::regex_search (expr, m, re)) {
      d.params["cmp"] = m[1].str ();
      d.params["thr"] = m[2].str ();
    } else {
      d.params["cmp"] = "==";
      d.params["thr"] = "0.0";
    }
    if (d.operands.size () < 2) {
      d.supported = false; d.reason = "NET AREA RATIO needs two layers";
    }
    return d;
  }

  //  SIZE / GROW / SHRINK / OVERSIZE / UNDERSIZE
  for (const std::string &t : up) {
    if (size_ops ().count (t)) {
      std::vector<double> nn = nums (toks);
      d.kind = "size";
      d.size_op = t;
      if (!lt.empty ()) { d.operands = {lt[0]}; }
      if (!nn.empty ()) { d.has_value = true; d.value = nn[0]; }
      if ((t == "SHRINK" || t == "UNDERSIZE") && d.has_value) {
        d.value = -std::abs (d.value);
      }
      //  one-sided qualifier (SHRINK X RIGHT BY 5 ...). Treating it as an
      //  isotropic size silently corrupts every wide-metal derivation
      //  (commercial wide-metal chains) — the exact one-sided form is
      //  a half-size on that axis plus a half translation (erosion/dilation
      //  by an off-center segment), applied in the engine.
      //  Decks NEST these on ONE line — SHRINK(SHRINK(SHRINK(SHRINK X R 5)
      //  L 5) T 5) B 5 — and the paren-flattening tokenizer used to keep
      //  only the FIRST direction (a single R5: long rails survived ->
      //  phantom wide metal). Collect ALL directions + values in token
      //  order (innermost-out) and let the engine apply them sequentially.
      {
        std::string dirs, vals;
        size_t vi = 0;
        for (size_t ti = 0; ti < up.size (); ++ti) {
          if (up[ti] == "RIGHT" || up[ti] == "LEFT" ||
              up[ti] == "TOP" || up[ti] == "BOTTOM") {
            if (!dirs.empty ()) { dirs += ","; vals += ","; }
            dirs += up[ti];
            double v = (vi < nn.size ()) ? nn[vi] : (nn.empty () ? 0.0 : nn.back ());
            if ((t == "SHRINK" || t == "UNDERSIZE")) { v = -std::abs (v); }
            vals += std::to_string (v);
            ++vi;
          }
        }
        if (!dirs.empty ()) {
          d.params["dir"] = dirs;
          d.params["dir_vals"] = vals;
        }
      }
      //  morphological OVERUNDER (close) / UNDEROVER (open) modifier:
      //  Calibre `SIZE L BY d OVERUNDER` = oversize by d THEN undersize by d
      //  (closing: fills notches/gaps < 2d, bridges near shapes); UNDEROVER =
      //  undersize THEN oversize (opening: erases spikes/thin necks < 2d,
      //  keeps only cores). These are how the deck DERIVES dense-array cores
      //  (CT.S/CT.OT contact-array membership, implant array_not_check) — a
      //  plain isotropic size treats every shape as an array member and floods
      //  spurious spacing/enclosure violations. Capture the token; the engine
      //  applies the two-step.
      for (const auto &u : up) {
        if (u == "OVERUNDER" || u == "UNDEROVER") { d.params["morph"] = u; break; }
      }
      if (d.operands.empty ()) {
        d.supported = false; d.reason = "size without a layer";
      }
      return d;
    }
  }

  //  VERTEX <layer> op N
  if (has ("VERTEX")) {
    d.kind = "vertex";
    if (!lt.empty ()) { d.operands = {lt[0]}; }
    rel_bounds (expr, d.has_lo, d.lo, d.has_hi, d.hi);
    if (d.operands.empty ()) {
      d.supported = false; d.reason = "vertex without a layer";
    }
    return d;
  }

  //  RECTANGLE <layer> [ W BY H ] [ ASPECT N ]
  if (has ("RECTANGLE") || has ("RECTANGLES")) {
    d.kind = "rectangles";
    if (!lt.empty ()) { d.operands = {lt[0]}; }
    static const std::regex re_by (R"(\bBY\b)", std::regex::ECMAScript | std::regex::icase);
    static const std::regex re_aspect (R"(\bASPECT\b)", std::regex::ECMAScript | std::regex::icase);
    if (std::regex_search (expr, re_by)) {
      std::smatch m;
      std::regex_search (expr, m, re_by);
      std::string a = m.prefix ().str ();
      std::string b = m.suffix ().str ();
      bool hl, hh; double lo, hi;
      rel_bounds (a, hl, lo, hh, hi);
      d.params["w"] = (hl ? std::to_string (lo) : "-") + "," + (hh ? std::to_string (hi) : "-");
      rel_bounds (b, hl, lo, hh, hi);
      d.params["h"] = (hl ? std::to_string (lo) : "-") + "," + (hh ? std::to_string (hi) : "-");
    }
    if (has ("ASPECT")) {
      std::smatch m;
      std::regex_search (expr, m, re_aspect);
      std::string b = m.suffix ().str ();
      bool hl, hh; double lo, hi;
      rel_bounds (b, hl, lo, hh, hi);
      d.params["aspect"] = (hl ? std::to_string (lo) : "-") + "," + (hh ? std::to_string (hi) : "-");
    }
    if (d.operands.empty ()) {
      d.supported = false; d.reason = "rectangle without a layer";
    }
    return d;
  }

  //  HOLES / EXTENTS / MERGE (unary region ops) -- only fires when a layer exists
  for (const std::string &t : up) {
    auto it = unary_layer_ops ().find (t);
    if (it != unary_layer_ops ().end () && it->second != "passthrough") {
      if (!lt.empty ()) {
        d.kind = it->second;
        d.operands = {lt[0]};
        return d;
      }
    }
  }

  //  WITH EDGE: X WITH EDGE Y
  if (has ("WITH") && (has ("EDGE") || has ("EDGES"))) {
    d.kind = "with_edge";
    for (size_t i = 0; i < lt.size () && i < 2; ++i) { d.operands.push_back (lt[i]); }
    if (lt.size () < 2) {
      d.supported = false; d.reason = "WITH EDGE needs two layers";
    }
    return d;
  }

  //  EXPAND [EDGE] X [dir] BY N  (must precede the EDGE catch)
  if (has ("EXPAND")) {
    std::vector<double> nn = nums (toks);
    d.kind = "expand";
    if (!lt.empty ()) { d.operands = {lt[0]}; }
    if (!nn.empty ()) { d.has_value = true; d.value = nn[0]; }
    d.params["inside"] = has ("INSIDE") ? "1" : "0";
    d.params["outside"] = has ("OUTSIDE") ? "1" : "0";
    d.params["both"] = has ("BOTH") ? "1" : "0";
    if (d.operands.empty ()) {
      d.supported = false; d.reason = "expand without a layer";
    }
    return d;
  }

  //  Edge-typed derivations (result is an Edges layer)
  bool edge_cond = has ("EDGE") || has ("EDGES") || has ("COINCIDENT") || has ("COIN");
  if (!edge_cond) {
    for (const std::string &e : edge_ops ()) {
      if (U.count (e)) { edge_cond = true; break; }
    }
  }
  if (edge_cond) {
    bool coin = has ("COINCIDENT") || has ("COIN");
    bool inside = has ("INSIDE"), outside = has ("OUTSIDE"), touch = has ("TOUCH");
    bool binary = coin || inside || outside || touch;
    std::string dfm;
    for (const std::string &t : up) {
      if (edge_ops ().count (t) && t != "COIN" && t != "COINCIDENT") { dfm = t; break; }
    }
    d.kind = "edge";
    d.edge_typed = true;
    d.select_op = coin ? "COINCIDENT"
                  : inside ? "INSIDE"
                  : outside ? "OUTSIDE"
                  : touch ? "TOUCH"
                  : !dfm.empty () ? dfm
                  : "EDGE";
    d.params["coin"] = coin ? "1" : "0";
    d.params["inside"] = inside ? "1" : "0";
    d.params["outside"] = outside ? "1" : "0";
    d.params["touch"] = touch ? "1" : "0";
    d.params["inner"] = has ("INNER") ? "1" : "0";
    d.params["outer"] = has ("OUTER") ? "1" : "0";
    if (binary && lt.size () >= 2) {
      d.operands = {lt[0], lt[1]};
    } else if (!lt.empty ()) {
      d.operands = {lt[0]};
    }
    if (d.operands.empty ()) {
      d.supported = false; d.edge_typed = false; d.reason = "edge op without a layer";
    }
    return d;
  }

  //  region-returning selects, optionally NEGATED
  for (const std::string &t : up) {
    if (select_ops ().count (t)) {
      d.kind = "select";
      d.select_op = t;
      for (size_t i = 0; i < lt.size () && i < 2; ++i) { d.operands.push_back (lt[i]); }
      d.params["negate"] = has ("NOT") ? "1" : "0";
      //  SVRF interaction-count qualifier: `INTERACT A B ==N / >N / <N`.
      //  Dropping the count made every counted INTERACT behave uncounted —
      //  A commercial contact-orientation rule selects contacts interacting with EXACTLY 4
      //  rectilinear-edge strips, so the uncounted form mislabels every
      //  square contact in the design (15632/15632 false-flagged). Strict
      //  bounds fold to the integral count in the engine (>N -> N+1,
      //  <N -> N-1).
      {
        bool clo = false, chi = false; double cl = 0.0, ch = 0.0;
        rel_bounds (expr, clo, cl, chi, ch);
        if (clo || chi) {
          bool lo_strict = false, hi_strict = false;
          static const std::regex re_rel (
            R"((<=|<|>=|>|==)\s*(-?[0-9]*\.?[0-9]+))", std::regex::ECMAScript);
          for (auto it = std::sregex_iterator (expr.begin (), expr.end (), re_rel);
               it != std::sregex_iterator (); ++it) {
            const std::string rel = (*it)[1].str ();
            if (rel == ">") lo_strict = true;
            else if (rel == "<") hi_strict = true;
          }
          if (clo) {
            d.params["count_lo"] = std::to_string ((long long) cl);
            d.params["count_lo_strict"] = lo_strict ? "1" : "0";
          }
          if (chi) {
            d.params["count_hi"] = std::to_string ((long long) ch);
            d.params["count_hi_strict"] = hi_strict ? "1" : "0";
          }
        }
      }
      if (lt.empty ()) {
        d.supported = false; d.reason = "select without a layer";
      }
      return d;
    }
  }

  //  metric selection: AREA/PERIMETER -> Region ; LENGTH/ANGLE -> Edges
  for (const std::string &t : up) {
    if (metric_ops ().count (t)) {
      d.kind = "metric_select";
      d.metric = t;
      if (!lt.empty ()) { d.operands = {lt[0]}; }
      rel_bounds (expr, d.has_lo, d.lo, d.has_hi, d.hi);
      d.has_neq = find_neq (expr, d.neq);
      d.edge_typed = (t == "LENGTH" || t == "ANGLE");
      //  SVRF metric negation + bound strictness. `NOT ANGLE X >0 <90` is
      //  ONE construct: edges NOT strictly inside (0,90). Ignoring the NOT
      //  (and the strictness) turned it into "edges strictly inside (0,90)"
      //  — for a rectilinear layout that is the EMPTY set, so every
      //  derivation chained on it silently collapsed (a commercial contact-orientation rule).
      d.params["negate"] = has ("NOT") ? "1" : "0";
      {
        bool lo_strict = false, hi_strict = false;
        static const std::regex re_rel (
          R"((<=|<|>=|>|==)\s*(-?[0-9]*\.?[0-9]+))", std::regex::ECMAScript);
        for (auto it = std::sregex_iterator (expr.begin (), expr.end (), re_rel);
             it != std::sregex_iterator (); ++it) {
          const std::string rel = (*it)[1].str ();
          if (rel == ">") lo_strict = true;
          else if (rel == "<") hi_strict = true;
        }
        d.params["lo_strict"] = lo_strict ? "1" : "0";
        d.params["hi_strict"] = hi_strict ? "1" : "0";
      }
      if (d.operands.empty () || (!d.has_lo && !d.has_hi && !d.has_neq)) {
        d.supported = false; d.reason = t + " select without layer/bounds";
      }
      return d;
    }
  }

  //  nullary EXTENT: SVRF `X=EXTENT` (no operand) is the LAYOUT extent —
  //  the bbox of everything — NOT the per-shape EXTENTS op. Must precede
  //  the bare-identifier passthrough: `EXTENT` alone used to resolve as an
  //  undefined layer named EXTENT -> silently empty -> every BULK/LV
  //  context derived from it collapsed (commercial deck: SUB=EXTENT empty made
  //  POhv.S.5 fire 15420x on plain LV std cells).
  if (up.size () == 1 && (up[0] == "EXTENT" || up[0] == "EXTENTS")) {
    d.kind = "layout_extent";
    return d;
  }

  //  bare alias / passthrough
  if (toks.size () == 1 && is_identifier (toks[0])) {
    d.kind = "passthrough";
    d.operands = {toks[0]};
    return d;
  }

  d.supported = false; d.reason = "unrecognised derivation expression";
  return d;
}

// ── parse_deck ──────────────────────────────────────────────────────────────

SVRFDeck parse_deck (const std::string &text)
{
  static const std::regex block_open (
    R"(^\s*([A-Za-z0-9_.\-/]+)\s*\{\s*$)", std::regex::ECMAScript);
  static const std::regex assign_head (
    R"(^\s*([A-Za-z_][\w.$:]*)\s*=\s*(.+)$)", std::regex::ECMAScript);
  static const std::regex layer_re (
    R"(^\s*LAYER\s+([A-Za-z_][\w.$]*)\s+(\d+)(?:\s+(\d+))?\s*$)",
    std::regex::ECMAScript | std::regex::icase);
  static const std::regex copy_re (
    R"(\bCOPY\b\s+([A-Za-z_][\w.$]*))", std::regex::ECMAScript | std::regex::icase);

  std::string t = strip_comments (text);
  t = preprocess (t);

  SVRFDeck deck;
  deck.layers = parse_layers (t);
  deck.connects = parse_connects (t);

  std::vector<std::string> lines = splitlines (t);
  size_t i = 0, n = lines.size ();
  while (i < n) {
    const std::string &line = lines[i];

    std::smatch bo;
    if (std::regex_search (line, bo, block_open, std::regex_constants::match_continuous)) {
      std::string name = bo[1].str ();
      int depth = 1;
      size_t j = i + 1;
      std::vector<std::string> body;
      while (j < n && depth > 0) {
        depth += count_char (lines[j], '{') - count_char (lines[j], '}');
        if (depth > 0) { body.push_back (lines[j]); }
        ++j;
      }
      std::string joined = join (body, "\n");
      std::smatch mm;
      if (std::regex_search (joined, mm, meas_re ())) {
        SVRFRule r = make_rule (name, mm);
        size_t idx = deck.rules.size ();
        deck.rules.push_back (r);
        deck.statements.push_back ({SVRFStatement::Rule, idx});
      } else {
        std::smatch cp;
        if (std::regex_search (joined, cp, copy_re)) {
          SVRFRule r;
          r.name = name;
          r.op = "COPY";
          r.layer1 = cp[1].str ();
          r.layer2 = std::string ();
          r.cmp = std::string ();
          r.value = 0.0;
          {
            size_t a = joined.find_first_not_of (" \t\r\n\f\v");
            size_t b = joined.find_last_not_of (" \t\r\n\f\v");
            r.raw = (a == std::string::npos) ? std::string () : joined.substr (a, b - a + 1);
          }
          size_t idx = deck.rules.size ();
          deck.rules.push_back (r);
          deck.statements.push_back ({SVRFStatement::Rule, idx});
        }
      }
      i = j;
      continue;
    }

    std::smatch ah;
    bool is_assign = std::regex_search (line, ah, assign_head, std::regex_constants::match_continuous);
    if (is_assign && !std::regex_match (line, layer_re)) {
      std::string rhs = ah[2].str ();
      //  An assignment-form RULE leads with its measurement op; anchor the match.
      std::string rhs_stripped;
      {
        size_t a = rhs.find_first_not_of (" \t\r\n\f\v");
        size_t b = rhs.find_last_not_of (" \t\r\n\f\v");
        rhs_stripped = (a == std::string::npos) ? std::string () : rhs.substr (a, b - a + 1);
      }
      std::smatch mm;
      if (std::regex_search (rhs_stripped, mm, meas_re (), std::regex_constants::match_continuous)) {
        SVRFRule r = make_rule (ah[1].str (), mm);
        size_t idx = deck.rules.size ();
        deck.rules.push_back (r);
        deck.statements.push_back ({SVRFStatement::Rule, idx});
      } else {
        SVRFDerivation dv = parse_derivation (ah[1].str (), rhs);
        size_t idx = deck.derivations.size ();
        deck.derivations.push_back (dv);
        deck.statements.push_back ({SVRFStatement::Derivation, idx});
      }
    }
    ++i;
  }

  return deck;
}

}  // namespace db
