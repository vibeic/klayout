// Canonical parse-dump of a SVRF deck via the native C++ parser (dbSVRFDeck).
//
// Emits the SAME deterministic, line-oriented dump as tests/dump_parse.py so the
// two can be diffed byte-for-byte. Format (stable):
//
//   LAYER <name> <g>/<d>[,<g>/<d>...]                       (sorted by name)
//   CONNECT <a> <b> <via|->                                 (source order)
//   STMT <i> RULE  <name>|<op>|<l1>|<l2|->|<cmp>|<val>|m=..|ia=..|opp=..|whole=..|
//                  conn=..|win=..|step=..|sup=..
//   STMT <i> DERIV <name>|<kind>|ops=..|bool=..|sop=..|val=..|sel=..|metric=..|
//                  bounds=<lo,hi>|neq=..|edge=..|sup=..

#include "dbSVRFDeck.h"

#include <charconv>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

// Match dump_parse._f: integers without a trailing ".0", otherwise Python's
// repr() shortest round-trip (std::to_chars default == shortest round-trip).
static std::string fmt_double (double x)
{
  if (std::isfinite (x) && x == std::trunc (x)
      && x >= -9.2e18 && x <= 9.2e18) {
    return std::to_string ((long long) x);
  }
  char buf[64];
  auto res = std::to_chars (buf, buf + sizeof (buf), x);
  return std::string (buf, res.ptr);
}

static std::string f_opt (bool has, double v)
{
  return has ? fmt_double (v) : std::string ("-");
}

static std::string dash (const std::string &s)
{
  return s.empty () ? std::string ("-") : s;
}

static std::string conn_str (db::SVRFConnectivity c)
{
  switch (c) {
    case db::SVRFConnectivity::same: return "same";
    case db::SVRFConnectivity::different: return "different";
    default: return "none";
  }
}

static std::string bounds_str (bool has_lo, double lo, bool has_hi, double hi)
{
  return (has_lo ? fmt_double (lo) : std::string ("-")) + ","
       + (has_hi ? fmt_double (hi) : std::string ("-"));
}

int main (int argc, char **argv)
{
  if (argc < 2) {
    std::fprintf (stderr, "usage: %s <deck-file>\n", argv[0]);
    return 2;
  }

  std::ifstream in (argv[1], std::ios::binary);
  if (!in) {
    std::fprintf (stderr, "cannot open %s\n", argv[1]);
    return 2;
  }
  std::ostringstream ss;
  ss << in.rdbuf ();
  std::string text = ss.str ();

  db::SVRFDeck deck = db::parse_deck (text);

  std::vector<std::string> out;

  // layers, sorted by name (std::map iterates in key order)
  for (const auto &kv : deck.layers) {
    std::string purposes;
    for (size_t k = 0; k < kv.second.size (); ++k) {
      if (k) { purposes += ","; }
      purposes += std::to_string (kv.second[k].first) + "/"
                + std::to_string (kv.second[k].second);
    }
    out.push_back ("LAYER " + kv.first + " " + purposes);
  }

  // connects, source order
  for (const auto &c : deck.connects) {
    out.push_back ("CONNECT " + std::get<0> (c) + " " + std::get<1> (c) + " "
                   + dash (std::get<2> (c)));
  }

  // statements, source order
  for (size_t i = 0; i < deck.statements.size (); ++i) {
    const db::SVRFStatement &st = deck.statements[i];
    std::string idx = std::to_string (i);
    if (st.kind == db::SVRFStatement::Rule) {
      const db::SVRFRule &r = deck.rules[st.index];
      std::string line = "STMT " + idx + " RULE " + r.name + "|" + r.op + "|"
        + r.layer1 + "|" + dash (r.layer2) + "|"
        + r.cmp + "|" + fmt_double (r.value) + "|m=" + r.metrics
        + "|ia=" + f_opt (r.has_ignore_angle, r.ignore_angle)
        + "|opp=" + (r.opposite ? "1" : "0")
        + "|whole=" + (r.whole_edges ? "1" : "0")
        + "|conn=" + conn_str (r.connectivity)
        + "|win=" + f_opt (r.has_window, r.window)
        + "|step=" + f_opt (r.has_step, r.step)
        + "|sup=" + (r.supported ? "1" : "0");
      out.push_back (line);
    } else {
      const db::SVRFDerivation &d = deck.derivations[st.index];
      std::string ops;
      for (size_t k = 0; k < d.operands.size (); ++k) {
        if (k) { ops += ","; }
        ops += d.operands[k];
      }
      std::string line = "STMT " + idx + " DERIV " + d.name + "|" + d.kind
        + "|ops=" + ops
        + "|bool=" + dash (d.bool_sym)
        + "|sop=" + dash (d.size_op)
        + "|val=" + f_opt (d.has_value, d.value)
        + "|sel=" + dash (d.select_op)
        + "|metric=" + dash (d.metric)
        + "|bounds=" + bounds_str (d.has_lo, d.lo, d.has_hi, d.hi)
        + "|neq=" + f_opt (d.has_neq, d.neq)
        + "|edge=" + (d.edge_typed ? "1" : "0")
        + "|sup=" + (d.supported ? "1" : "0");
      out.push_back (line);
    }
  }

  std::string result;
  for (size_t i = 0; i < out.size (); ++i) {
    if (i) { result += "\n"; }
    result += out[i];
  }
  result += "\n";
  std::fwrite (result.data (), 1, result.size (), stdout);
  return 0;
}
