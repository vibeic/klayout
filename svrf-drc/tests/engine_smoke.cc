/*
  engine_smoke.cc -- standalone driver to run the native db::SVRFEngine on a
  deck + layout and emit the frozen report. Used to cross-check the native C++
  engine against the reference run_svrf_drc.py byte-for-byte.

    engine_smoke <deck.rule> <layout.gds> <report.txt> \
                 [klayout_version] [display_deck] [display_layout] [cell]

  display_deck/display_layout override the "# deck=.. layout=.." header strings
  so a host run can reproduce a container run's paths exactly. NO vendor data.
*/

#include "dbSVRFDeck.h"
#include "dbSVRFEngine.h"

#include <fstream>
#include <sstream>
#include <iostream>

int main (int argc, char **argv)
{
  if (argc < 4) {
    std::cerr << "usage: engine_smoke <deck> <layout> <report> [ver] [disp_deck] [disp_layout] [cell]\n";
    return 2;
  }
  std::ifstream f (argv[1]);
  std::stringstream ss;
  ss << f.rdbuf ();
  db::SVRFDeck deck = db::parse_deck (ss.str ());

  std::string ver        = argc > 4 ? argv[4] : "";
  std::string disp_deck  = argc > 5 ? argv[5] : argv[1];
  std::string disp_lay   = argc > 6 ? argv[6] : argv[2];
  std::string cell       = argc > 7 ? argv[7] : "";

  try {
    db::SVRFEngine eng (argv[2], deck, cell);
    eng.execute ();
    eng.write_report (argv[3], disp_deck, disp_lay, ver);
  } catch (std::exception &e) {
    std::cerr << "ERROR: " << e.what () << "\n";
    return 1;
  }
  return 0;
}
