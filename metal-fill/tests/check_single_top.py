#!/usr/bin/env python3
"""check_single_top.py — assert the streamed GDS has exactly ONE top cell.

A fill cell that was created but never instanced is, in GDS, simply another root.
A sign-off deck (gf180mcu's, for one) refuses a multi-top layout before a single rule
executes, so the failure mode is an ABSENT DRC verdict rather than a red one. Run under
KLayout (`strmrun` / `klayout -b -r`).

    DUMMY_GDS=<filled.gds> strmrun check_single_top.py

Prints `TOP-OK` on success.
"""
import os
import sys


def main():
    import pya
    ly = pya.Layout()
    ly.read(os.environ["DUMMY_GDS"])
    tops = [c.name for c in ly.each_cell() if c.parent_cells() == 0]
    orphan_fill = [n for n in tops if n.startswith("FILL_")]
    if orphan_fill:
        print(f"FAIL [6] un-instanced fill cells left as extra top cells: {orphan_fill} "
              f"(all tops: {tops})")
        return 1
    if len(tops) != 1:
        print(f"FAIL [6] {len(tops)} top cells in the stream: {tops}")
        return 1
    print(f"  [6] streamed GDS has exactly one top cell: {tops[0]} OK")
    print("TOP-OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
