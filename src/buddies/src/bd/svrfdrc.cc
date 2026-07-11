/*

  KLayout Layout Viewer
  Copyright (C) 2006-2026 Matthias Koefferlein

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA

*/

//  svrfdrc -- native SVRF/Calibre DRC-deck execution buddy.
//
//  Parses a Calibre/SVRF `.rule` DRC deck (db::parse_deck) and executes every
//  statement DIRECTLY on KLayout's own db:: geometry engine (db::SVRFEngine) --
//  NO Python/Ruby interpreter, NO `-r` script, NO intermediate `.drc`. This is
//  the native replacement for `klayout -b -r run_svrf_drc.py`.
//
//    svrfdrc <deck.rule> <layout.gds> <report.txt> [--cell=TOP]
//
//  The report is byte-identical to the reference run_svrf_drc.py::main().

#include "bdCommon.h"

#include "dbSVRFDeck.h"
#include "dbSVRFEngine.h"

#include "tlCommandLineParser.h"
#include "tlLog.h"

#include <fstream>
#include <sstream>

#define SVRF_STR2(x) #x
#define SVRF_STR(x) SVRF_STR2(x)

BD_PUBLIC int svrfdrc (int argc, char *argv[])
{
  std::string deck_path, layout_path, report_path, cell;

  tl::CommandLineOptions cmd;
  cmd << tl::arg ("deck",   &deck_path,   "The SVRF/Calibre DRC rule deck (.rule)")
      << tl::arg ("layout", &layout_path, "The layout to check (GDS/OASIS, may be gzip compressed)")
      << tl::arg ("report", &report_path, "The report file to write (frozen SVRF-native format)")
      << tl::arg ("?--cell=name", &cell,  "Explicit top cell (required for multi-top layouts)");

  cmd.brief ("Native SVRF/Calibre DRC-deck execution on KLayout's db engine (no Python interpreter).");
  cmd.parse (argc, argv);

  //  KLayout version string, matching pya.Application.version() ("KLayout 0.30.9")
  //  without pulling in version.h (whose file-scope globals would multiply-define).
#if defined(KLAYOUT_VERSION)
  std::string ver = std::string ("KLayout ") + SVRF_STR (KLAYOUT_VERSION);
#else
  std::string ver = "KLayout";
#endif

  std::ifstream f (deck_path.c_str ());
  if (! f) {
    tl::error << "svrfdrc: cannot open deck " << deck_path;
    return 1;
  }
  std::stringstream ss;
  ss << f.rdbuf ();

  db::SVRFDeck deck = db::parse_deck (ss.str ());
  db::SVRFEngine eng (layout_path, deck, cell);
  eng.execute ();
  eng.write_report (report_path, deck_path, layout_path, ver);

  return 0;
}
