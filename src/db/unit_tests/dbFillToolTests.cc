
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


#include "dbFillTool.h"
#include "dbReader.h"
#include "dbRegion.h"
#include "dbTestSupport.h"
#include "dbRegion.h"
#include "tlUnitTest.h"
#include "tlStream.h"
#include "tlString.h"

#include <algorithm>
#include <string>
#include <vector>

TEST(1)
{
  db::Layout ly;
  {
    std::string fn (tl::testdata ());
    fn += "/algo/fill_tool1.gds";
    tl::InputStream stream (fn);
    db::Reader reader (stream);
    reader.read (ly);
  }

  db::cell_index_type fill_cell = ly.cell_by_name ("FILL_CELL").second;
  db::cell_index_type top_cell = ly.cell_by_name ("TOP").second;
  unsigned int fill_layer = ly.get_layer (db::LayerProperties (1, 0));

  db::Region fill_region (db::RecursiveShapeIterator (ly, ly.cell (top_cell), fill_layer));

  db::fill_region (&ly.cell (top_cell), fill_region, fill_cell, ly.cell (fill_cell).bbox (), db::Point (), false);

  CHECKPOINT();
  db::compare_layouts (_this, ly, tl::testdata () + "/algo/fill_tool_au1.gds");
}

TEST(2)
{
  db::Layout ly;
  {
    std::string fn (tl::testdata ());
    fn += "/algo/fill_tool2.gds";
    tl::InputStream stream (fn);
    db::Reader reader (stream);
    reader.read (ly);
  }

  db::cell_index_type fill_cell = ly.cell_by_name ("FILL_CELL").second;
  db::cell_index_type top_cell = ly.cell_by_name ("TOP").second;
  unsigned int fill_layer = ly.get_layer (db::LayerProperties (1, 0));

  db::Region fill_region (db::RecursiveShapeIterator (ly, ly.cell (top_cell), fill_layer));

  db::Region remaining_parts, remaining_polygons;

  db::fill_region (&ly.cell (top_cell), fill_region, fill_cell, ly.cell (fill_cell).bbox (), db::Point (), true, &remaining_parts, db::Vector (50, 100), &remaining_polygons);

  unsigned int l100 = ly.insert_layer (db::LayerProperties (100, 0));
  unsigned int l101 = ly.insert_layer (db::LayerProperties (101, 0));
  remaining_parts.insert_into (&ly, top_cell, l100);
  remaining_polygons.insert_into (&ly, top_cell, l101);

  CHECKPOINT();
  db::compare_layouts (_this, ly, tl::testdata () + "/algo/fill_tool_au2.gds");
}

TEST(3)
{
  db::Layout ly;
  {
    std::string fn (tl::testdata ());
    fn += "/algo/fill_tool3.gds";
    tl::InputStream stream (fn);
    db::Reader reader (stream);
    reader.read (ly);
  }

  db::cell_index_type fill_cell = ly.cell_by_name ("FILL_CELL").second;
  db::cell_index_type top_cell = ly.cell_by_name ("TOP").second;
  unsigned int fill_layer = ly.get_layer (db::LayerProperties (1, 0));

  db::Region fill_region (db::RecursiveShapeIterator (ly, ly.cell (top_cell), fill_layer));

  db::Region remaining_parts, remaining_polygons;

  db::Vector ko (-100, -130);
  db::Vector rs (230, 40);
  db::Vector cs (40, 230);
  db::Box fc_box (db::Point () + ko, db::Point (rs.x (), cs.y ()) + ko);
  db::fill_region (&ly.cell (top_cell), fill_region, fill_cell, fc_box, rs, cs, db::Point (), true, &remaining_parts, db::Vector (50, 100), &remaining_polygons);

  unsigned int l100 = ly.insert_layer (db::LayerProperties (100, 0));
  unsigned int l101 = ly.insert_layer (db::LayerProperties (101, 0));
  remaining_parts.insert_into (&ly, top_cell, l100);
  remaining_polygons.insert_into (&ly, top_cell, l101);

  CHECKPOINT();
  db::compare_layouts (_this, ly, tl::testdata () + "/algo/fill_tool_au3.gds");
}

TEST(3a)
{
  db::Layout ly;
  {
    std::string fn (tl::testdata ());
    fn += "/algo/fill_tool3.gds";
    tl::InputStream stream (fn);
    db::Reader reader (stream);
    reader.read (ly);
  }

  db::cell_index_type fill_cell = ly.cell_by_name ("FILL_CELL").second;
  db::cell_index_type top_cell = ly.cell_by_name ("TOP").second;
  unsigned int fill_layer = ly.get_layer (db::LayerProperties (1, 0));

  db::Region fill_region (db::RecursiveShapeIterator (ly, ly.cell (top_cell), fill_layer));

  db::Region remaining_parts, remaining_polygons;

  db::Vector ko (-100, -130);
  db::Vector rs (230, 40);
  db::Vector cs (-40, 230);
  db::Box fc_box (db::Point () + ko, db::Point (rs.x (), cs.y ()) + ko);
  db::fill_region (&ly.cell (top_cell), fill_region, fill_cell, fc_box, rs, cs, db::Point (), true, &remaining_parts, db::Vector (50, 100), &remaining_polygons);

  unsigned int l100 = ly.insert_layer (db::LayerProperties (100, 0));
  unsigned int l101 = ly.insert_layer (db::LayerProperties (101, 0));
  remaining_parts.insert_into (&ly, top_cell, l100);
  remaining_polygons.insert_into (&ly, top_cell, l101);

  CHECKPOINT();
  db::compare_layouts (_this, ly, tl::testdata () + "/algo/fill_tool_au3a.gds");
}

TEST(3b)
{
  db::Layout ly;
  {
    std::string fn (tl::testdata ());
    fn += "/algo/fill_tool3.gds";
    tl::InputStream stream (fn);
    db::Reader reader (stream);
    reader.read (ly);
  }

  db::cell_index_type fill_cell = ly.cell_by_name ("FILL_CELL").second;
  db::cell_index_type top_cell = ly.cell_by_name ("TOP").second;
  unsigned int fill_layer = ly.get_layer (db::LayerProperties (1, 0));

  db::Region fill_region (db::RecursiveShapeIterator (ly, ly.cell (top_cell), fill_layer));

  db::Region remaining_parts, remaining_polygons;

  db::Vector ko (-100, -130);
  db::Vector rs (230, -40);
  db::Vector cs (40, 230);
  db::Box fc_box (db::Point () + ko, db::Point (rs.x (), cs.y ()) + ko);
  db::fill_region (&ly.cell (top_cell), fill_region, fill_cell, fc_box, rs, cs, db::Point (), true, &remaining_parts, db::Vector (50, 100), &remaining_polygons);

  unsigned int l100 = ly.insert_layer (db::LayerProperties (100, 0));
  unsigned int l101 = ly.insert_layer (db::LayerProperties (101, 0));
  remaining_parts.insert_into (&ly, top_cell, l100);
  remaining_polygons.insert_into (&ly, top_cell, l101);

  CHECKPOINT();
  db::compare_layouts (_this, ly, tl::testdata () + "/algo/fill_tool_au3b.gds");
}

TEST(3c)
{
  db::Layout ly;
  {
    std::string fn (tl::testdata ());
    fn += "/algo/fill_tool3.gds";
    tl::InputStream stream (fn);
    db::Reader reader (stream);
    reader.read (ly);
  }

  db::cell_index_type fill_cell = ly.cell_by_name ("FILL_CELL").second;
  db::cell_index_type top_cell = ly.cell_by_name ("TOP").second;
  unsigned int fill_layer = ly.get_layer (db::LayerProperties (1, 0));

  db::Region fill_region (db::RecursiveShapeIterator (ly, ly.cell (top_cell), fill_layer));

  db::Region remaining_parts, remaining_polygons;

  db::Vector ko (-100, -130);
  db::Vector rs (230, -40);
  db::Vector cs (-40, 230);
  db::Box fc_box (db::Point () + ko, db::Point (rs.x (), cs.y ()) + ko);
  db::fill_region (&ly.cell (top_cell), fill_region, fill_cell, fc_box, rs, cs, db::Point (), true, &remaining_parts, db::Vector (50, 100), &remaining_polygons);

  unsigned int l100 = ly.insert_layer (db::LayerProperties (100, 0));
  unsigned int l101 = ly.insert_layer (db::LayerProperties (101, 0));
  remaining_parts.insert_into (&ly, top_cell, l100);
  remaining_polygons.insert_into (&ly, top_cell, l101);

  CHECKPOINT();
  db::compare_layouts (_this, ly, tl::testdata () + "/algo/fill_tool_au3c.gds");
}

TEST(4)
{
  db::Layout ly;
  {
    std::string fn (tl::testdata ());
    fn += "/algo/fill_tool4.gds";
    tl::InputStream stream (fn);
    db::Reader reader (stream);
    reader.read (ly);
  }

  db::cell_index_type fill_cell = ly.cell_by_name ("FILL_CELL").second;
  db::cell_index_type top_cell = ly.cell_by_name ("TOP").second;
  unsigned int fill_layer = ly.get_layer (db::LayerProperties (1, 0));

  db::Region fill_region (db::RecursiveShapeIterator (ly, ly.cell (top_cell), fill_layer));

  db::Region remaining_polygons;

  db::Vector ko (-100, -130);
  db::Vector rs (230, 0);
  db::Vector cs (0, 230);
  db::Box fc_box (db::Point () + ko, db::Point (rs.x (), cs.y ()) + ko);
  db::fill_region_repeat (&ly.cell (top_cell), fill_region, fill_cell, fc_box, rs, cs, db::Vector (50, 100), &remaining_polygons);

  unsigned int l101 = ly.insert_layer (db::LayerProperties (101, 0));
  remaining_polygons.insert_into (&ly, top_cell, l101);

  CHECKPOINT();
  db::compare_layouts (_this, ly, tl::testdata () + "/algo/fill_tool_au4.gds");
}

TEST(4b)
{
  db::Layout ly;
  {
    std::string fn (tl::testdata ());
    fn += "/algo/fill_tool4.gds";
    tl::InputStream stream (fn);
    db::Reader reader (stream);
    reader.read (ly);
  }

  db::cell_index_type fill_cell = ly.cell_by_name ("FILL_CELL").second;
  db::cell_index_type top_cell = ly.cell_by_name ("TOP").second;
  unsigned int fill_layer = ly.get_layer (db::LayerProperties (1, 0));

  db::Region fill_region (db::RecursiveShapeIterator (ly, ly.cell (top_cell), fill_layer));

  db::Region remaining_polygons;

  db::Vector ko (-100, -130);
  db::Vector rs (230, 0);
  db::Vector cs (0, 230);
  db::Box fc_box (db::Point () + ko, db::Point ());
  db::fill_region (&ly.cell (top_cell), fill_region, fill_cell, fc_box, rs, cs, db::Point (), true, &remaining_polygons);

  unsigned int l101 = ly.insert_layer (db::LayerProperties (101, 0));
  remaining_polygons.insert_into (&ly, top_cell, l101);

  CHECKPOINT();
  db::compare_layouts (_this, ly, tl::testdata () + "/algo/fill_tool_au4b.gds");
}

TEST(4c)
{
  db::Layout ly;
  {
    std::string fn (tl::testdata ());
    fn += "/algo/fill_tool4.gds";
    tl::InputStream stream (fn);
    db::Reader reader (stream);
    reader.read (ly);
  }

  db::cell_index_type fill_cell = ly.cell_by_name ("FILL_CELL").second;
  db::cell_index_type top_cell = ly.cell_by_name ("TOP").second;
  unsigned int fill_layer = ly.get_layer (db::LayerProperties (1, 0));

  db::Region fill_region (db::RecursiveShapeIterator (ly, ly.cell (top_cell), fill_layer));

  db::Region remaining_polygons;

  db::Vector ko (-100, -130);
  db::Vector rs (230, 0);
  db::Vector cs (0, 230);
  db::Box fc_box (db::Point () + ko, db::Point (rs.x (), cs.y ()) + ko);
  db::fill_region (&ly.cell (top_cell), fill_region, fill_cell, fc_box.enlarged (db::Vector (100, 100)), rs, cs, db::Point (), true, &remaining_polygons);

  unsigned int l101 = ly.insert_layer (db::LayerProperties (101, 0));
  remaining_polygons.insert_into (&ly, top_cell, l101);

  CHECKPOINT();
  db::compare_layouts (_this, ly, tl::testdata () + "/algo/fill_tool_au4c.gds");
}

//  issue #1309
TEST(5)
{
  db::Layout ly;
  {
    std::string fn (tl::testdata ());
    fn += "/algo/fill_tool5.gds";
    tl::InputStream stream (fn);
    db::Reader reader (stream);
    reader.read (ly);
  }

  db::cell_index_type fill_cell = ly.cell_by_name ("FILL_CELL").second;
  db::cell_index_type top_cell = ly.cell_by_name ("TOP").second;
  unsigned int fill_layer = ly.get_layer (db::LayerProperties (1, 0));

  db::Region fill_region (db::RecursiveShapeIterator (ly, ly.cell (top_cell), fill_layer));

  db::Region remaining_polygons;

  db::Vector rs (50, 0);
  db::Vector cs (0, 50);
  db::Box fc_box (db::Point (), db::Point (rs.x (), cs.y ()));
  db::fill_region (&ly.cell (top_cell), fill_region, fill_cell, fc_box, rs, cs, db::Point (), false, &remaining_polygons);

  unsigned int l100 = ly.insert_layer (db::LayerProperties (100, 0));
  remaining_polygons.insert_into (&ly, top_cell, l100);

  CHECKPOINT();
  db::compare_layouts (_this, ly, tl::testdata () + "/algo/fill_tool_au5.oas", db::WriteOAS);
}

//  issue #2087
TEST(6)
{
  db::Layout ly;
  {
    std::string fn (tl::testdata ());
    fn += "/algo/fill_tool6.gds";
    tl::InputStream stream (fn);
    db::Reader reader (stream);
    reader.read (ly);
  }

  db::cell_index_type fill_cell = ly.cell_by_name ("FILL_CELL").second;
  db::cell_index_type top_cell = ly.cell_by_name ("TOP").second;
  unsigned int fill_layer = ly.get_layer (db::LayerProperties (1, 0));

  db::Region fill_region (db::RecursiveShapeIterator (ly, ly.cell (top_cell), fill_layer));

  db::Region remaining_polygons;

  db::Vector rs (2500, 0);
  db::Vector cs (650, 2500);
  db::Box fc_box = ly.cell (fill_cell).bbox ();
  db::fill_region (&ly.cell (top_cell), fill_region, fill_cell, fc_box, rs, cs, db::Point (), false, &remaining_polygons);

  unsigned int l100 = ly.insert_layer (db::LayerProperties (100, 0));
  remaining_polygons.insert_into (&ly, top_cell, l100);

  CHECKPOINT();
  db::compare_layouts (_this, ly, tl::testdata () + "/algo/fill_tool_au6.oas", db::WriteOAS);
}

//  exclude_area
TEST(7)
{
  db::Layout ly;
  {
    std::string fn (tl::testdata ());
    fn += "/algo/fill_tool7.gds";
    tl::InputStream stream (fn);
    db::Reader reader (stream);
    reader.read (ly);
  }

  db::cell_index_type fill_cell = ly.cell_by_name ("FILL_CELL").second;
  db::cell_index_type top_cell = ly.cell_by_name ("TOP").second;

  unsigned int fill_layer = ly.get_layer (db::LayerProperties (1, 0));
  db::Region fill_region (db::RecursiveShapeIterator (ly, ly.cell (top_cell), fill_layer));

  unsigned int excl_layer = ly.get_layer (db::LayerProperties (2, 0));
  db::Region excl_region (db::RecursiveShapeIterator (ly, ly.cell (top_cell), excl_layer));

  db::Region remaining_polygons;

  db::Vector rs (2500, 0);
  db::Vector cs (650, 2500);
  db::Box fc_box = ly.cell (fill_cell).bbox ();
  db::fill_region (&ly.cell (top_cell), fill_region, fill_cell, fc_box, rs, cs, db::Point (), false, &remaining_polygons, db::Vector (), 0, db::Box (), excl_region);

  unsigned int l100 = ly.insert_layer (db::LayerProperties (100, 0));
  remaining_polygons.insert_into (&ly, top_cell, l100);

  CHECKPOINT();
  db::compare_layouts (_this, ly, tl::testdata () + "/algo/fill_tool_au7.oas", db::WriteOAS);
}

//  exclude_area
TEST(8)
{
  db::Layout ly;
  {
    std::string fn (tl::testdata ());
    fn += "/algo/fill_tool8.gds";
    tl::InputStream stream (fn);
    db::Reader reader (stream);
    reader.read (ly);
  }

  db::cell_index_type fill_cell = ly.cell_by_name ("FILL_CELL").second;
  db::cell_index_type top_cell = ly.cell_by_name ("TOP").second;

  unsigned int fill_layer = ly.get_layer (db::LayerProperties (1, 0));
  db::Region fill_region (db::RecursiveShapeIterator (ly, ly.cell (top_cell), fill_layer));

  unsigned int excl_layer = ly.get_layer (db::LayerProperties (2, 0));
  db::Region excl_region (db::RecursiveShapeIterator (ly, ly.cell (top_cell), excl_layer));

  db::Vector rs (2500, 0);
  db::Vector cs (650, 2500);
  db::Box fc_box = ly.cell (fill_cell).bbox ();
  db::fill_region (&ly.cell (top_cell), fill_region, fill_cell, fc_box, rs, cs, db::Point (), false, 0, db::Vector (), 0, db::Box (), excl_region);

  CHECKPOINT();
  db::compare_layouts (_this, ly, tl::testdata () + "/algo/fill_tool_au8.oas", db::WriteOAS);
}

//  exclude_area
TEST(9)
{
  db::Layout ly;
  {
    std::string fn (tl::testdata ());
    fn += "/algo/fill_tool9.gds";
    tl::InputStream stream (fn);
    db::Reader reader (stream);
    reader.read (ly);
  }

  db::cell_index_type fill_cell = ly.cell_by_name ("FILL_CELL").second;
  db::cell_index_type top_cell = ly.cell_by_name ("TOP").second;

  unsigned int fill_layer = ly.get_layer (db::LayerProperties (1, 0));
  db::Region fill_region (db::RecursiveShapeIterator (ly, ly.cell (top_cell), fill_layer));

  unsigned int excl_layer = ly.get_layer (db::LayerProperties (2, 0));
  db::Region excl_region (db::RecursiveShapeIterator (ly, ly.cell (top_cell), excl_layer));

  db::Vector rs (2500, 0);
  db::Vector cs (650, 2500);
  db::Box fc_box = ly.cell (fill_cell).bbox ();
  db::fill_region (&ly.cell (top_cell), fill_region, fill_cell, fc_box, rs, cs, db::Point (), true, 0, db::Vector (), 0, db::Box (), excl_region);

  CHECKPOINT();
  db::compare_layouts (_this, ly, tl::testdata () + "/algo/fill_tool_au9.oas", db::WriteOAS);
}

//  ---------------------------------------------------------------------------
//  fill_margin between locally-anchored (enhanced) fill arrays.
//
//  \Cell#fill_region_multi documents: "The fill_margin parameter is important as
//  it controls the distance between fill cells with a different origin and
//  therefore introduces a safety distance between pitch-incompatible arrays."
//
//  In enhanced mode every polygon of the fill region gets its OWN raster origin,
//  so two neighbouring polygons produce two arrays that are pitch-incompatible
//  with each other. The fill margin is the only thing that can keep those two
//  arrays apart, and the tests below assert that it does.
//  ---------------------------------------------------------------------------

namespace
{

//  Every fill cell placement in `cell`, in `cell`'s coordinates.
static std::vector<db::Box>
placed_fill_boxes (const db::Layout &ly, const db::Cell &cell, unsigned int layer)
{
  db::Region r ((db::RecursiveShapeIterator (ly, cell, layer)));
  r.set_merged_semantics (false);

  std::vector<db::Box> boxes;
  for (db::Region::const_iterator p = r.begin (); ! p.at_end (); ++p) {
    boxes.push_back (p->box ());
  }
  return boxes;
}

//  Pairs of fill cells that are separated (i.e. do not abut or overlap) but are
//  closer than `margin` in BOTH directions - which is exactly the condition
//  "one box enlarged by the margin still overlaps the other".
static std::string
fill_margin_violations (const std::vector<db::Box> &boxes, const db::Vector &margin)
{
  std::string res;

  for (size_t i = 0; i < boxes.size (); ++i) {
    for (size_t j = i + 1; j < boxes.size (); ++j) {

      const db::Box &a = boxes [i];
      const db::Box &b = boxes [j];

      db::Coord gx = std::max (db::Coord (0), std::max (a.left () - b.right (), b.left () - a.right ()));
      db::Coord gy = std::max (db::Coord (0), std::max (a.bottom () - b.top (), b.bottom () - a.top ()));

      if ((gx > 0 || gy > 0) && gx < margin.x () && gy < margin.y ()) {
        res += tl::sprintf ("%s vs. %s: gap=(%d,%d) < margin=(%d,%d)\n",
                            a.to_string (), b.to_string (),
                            int (gx), int (gy), int (margin.x ()), int (margin.y ()));
      }

    }
  }

  return res;
}

//  Two disjoint boxes 30 DBU apart, tiled by a 100x100 fill cell. In enhanced
//  mode the left box anchors its raster at x=0 and the right one at x=330, so
//  the two arrays are pitch-incompatible: nothing but the fill margin separates
//  the right edge of the left array from the left edge of the right one.
static void
build_two_anchored_polygons (db::Layout &ly, db::cell_index_type &fill_cell, db::cell_index_type &top_cell, unsigned int &l1, db::Region &fr)
{
  ly.dbu (0.001);

  l1 = ly.insert_layer (db::LayerProperties (1, 0));

  fill_cell = ly.add_cell ("FILL_CELL");
  ly.cell (fill_cell).shapes (l1).insert (db::Box (0, 0, 100, 100));

  top_cell = ly.add_cell ("TOP");

  fr.clear ();
  fr.insert (db::Box (0, 0, 300, 300));
  fr.insert (db::Box (330, 0, 630, 300));
}

}

//  fill_margin must separate arrays with different locally-optimized origins
TEST(fill_margin_separates_anchored_arrays)
{
  db::Layout ly;
  db::cell_index_type fill_cell = 0, top_cell = 0;
  unsigned int l1 = 0;
  db::Region fr;

  build_two_anchored_polygons (ly, fill_cell, top_cell, l1, fr);

  db::Vector fill_margin (50, 50);

  db::fill_region (&ly.cell (top_cell), fr, fill_cell, ly.cell (fill_cell).bbox (), db::Point (), true /*enhanced*/, 0, fill_margin, 0);

  std::vector<db::Box> boxes = placed_fill_boxes (ly, ly.cell (top_cell), l1);

  //  the test must not pass by placing (almost) nothing
  EXPECT_GE (int (boxes.size ()), 12);

  db::Box all;
  for (std::vector<db::Box>::const_iterator b = boxes.begin (); b != boxes.end (); ++b) {
    all += *b;
  }
  //  both polygons received fill
  EXPECT_LE (int (all.left ()), 0);
  EXPECT_GE (int (all.right ()), 550);

  CHECKPOINT();
  EXPECT_EQ (fill_margin_violations (boxes, fill_margin), "");
}

//  same, through fill_region_multi (fill_region_repeat) - the method whose
//  documentation carries the "safety distance between pitch-incompatible
//  arrays" promise
TEST(fill_margin_separates_anchored_arrays_multi)
{
  db::Layout ly;
  db::cell_index_type fill_cell = 0, top_cell = 0;
  unsigned int l1 = 0;
  db::Region fr;

  build_two_anchored_polygons (ly, fill_cell, top_cell, l1, fr);

  db::Vector fill_margin (50, 50);
  db::Box fc_box = ly.cell (fill_cell).bbox ();

  db::fill_region_repeat (&ly.cell (top_cell), fr, fill_cell, fc_box,
                          db::Vector (fc_box.width (), 0), db::Vector (0, fc_box.height ()),
                          fill_margin);

  std::vector<db::Box> boxes = placed_fill_boxes (ly, ly.cell (top_cell), l1);

  EXPECT_GE (int (boxes.size ()), 12);

  CHECKPOINT();
  EXPECT_EQ (fill_margin_violations (boxes, fill_margin), "");
}
