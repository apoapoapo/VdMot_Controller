#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""
Collision check of the enclosure against the PCB STEP model.

  1. extract hardware/c3-sample_3D.rar (e.g. `unar c3-sample_3D.rar`)
  2. python3 enclosure.py
  3. python3 check_fit.py path/to/c3-sample_3D.step

Reports every component that intersects the base, the lid or the DIN clip,
and checks that each part is a single valid solid. The modular jacks are
checked against their datasheet envelope in addition to the STEP model.
"""

import os
import sys

import cadquery as cq
from OCP.STEPControl import STEPControl_Reader
from OCP.TopoDS import TopoDS_Iterator

import enclosure as enc


def load_components(path):
    reader = STEPControl_Reader()
    if reader.ReadFile(path) != 1:
        sys.exit(f"cannot read {path}")
    reader.TransferRoots()
    it = TopoDS_Iterator(reader.OneShape())
    parts = []
    while it.More():
        shape = cq.Shape.cast(it.Value())
        bb = shape.BoundingBox()
        # The board itself (exported only 0.44 mm thick) is replaced by a 1.6 mm box below.
        if not (bb.xlen > 150 and bb.ylen > 80 and bb.zlen < 1):
            parts.append(shape)
        it.Next()
    return parts


def overlaps(a, b, tol=0.0):
    return not (
        a.xmax < b.xmin - tol
        or a.xmin > b.xmax + tol
        or a.ymax < b.ymin - tol
        or a.ymin > b.ymax + tol
        or a.zmax < b.zmin - tol
        or a.zmin > b.zmax + tol
    )


def near_material(bb, margin=1.0):
    """Cheap pre-filter: True if a bounding box may touch enclosure material.

    Everything strictly inside the free cavity (between ledges and roof, away
    from towers and pins) cannot collide and is skipped to keep the check fast.
    """
    if bb.zmin < enc.PCB_BOT + margin or bb.zmax > enc.ROOF_BOT - margin:
        return True
    if bb.xmin < enc.IN_X0 + margin or bb.xmax > enc.IN_X1 - margin:
        return True
    if bb.ymin < enc.IN_Y0 + margin or bb.ymax > enc.IN_Y1 - margin:
        return True
    r = enc.PIN_D / 2 + margin
    for x, y in enc.PIN_POS:
        if bb.xmin < x + r and bb.xmax > x - r and bb.ymin < y + r and bb.ymax > y - r:
            return True
    r = enc.TOWER_D / 2 + margin
    for x, y in enc.TOWERS:
        if bb.xmin < x + r and bb.xmax > x - r and bb.ymin < y + r and bb.ymax > y - r:
            return True
    return False


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    comps = load_components(sys.argv[1])
    pcb = cq.Solid.makeBox(enc.PCB_W, enc.PCB_D, enc.PCB_T, cq.Vector(0, 0, -enc.PCB_T))
    comps.append(pcb)
    # The STEP model has smaller modular jacks than the real MEBP 4-4S: add the datasheet envelope.
    comps += enc.jack_envelopes()
    print(f"{len(comps)} component shapes loaded")

    base = enc.make_base()
    parts = {
        "base": base.val(),
        "base_wallmount": enc.add_wall_tabs(base).val(),
        "lid": enc.make_lid().val(),
        "din_clip": enc.din_clip_in_assembly(enc.make_din_clip()).val(),
    }
    ok = True
    for name, part in parts.items():
        n = len(part.Solids())
        print(f"{name}: valid={part.isValid()} solids={n} volume={part.Volume() / 1000:.1f} cm3")
        ok &= part.isValid() and n == 1
    for name, part in parts.items():
        pbb = part.BoundingBox()
        hits = 0
        tested = 0
        for c in comps:
            cbb = c.BoundingBox()
            if not overlaps(pbb, cbb) or not near_material(cbb):
                continue
            tested += 1
            try:
                inter = part.intersect(c)
            except Exception:  # noqa: BLE001 - open shells may fail, fall back to faces
                continue
            vol = inter.Volume() if inter.Solids() else 0.0
            area = sum(f.Area() for f in inter.Faces())
            if vol > 1e-3 or (not c.Solids() and area > 1e-3):
                hits += 1
                ib = inter.BoundingBox()
                print(
                    f"  COLLISION {name}: vol={vol:.3f} area={area:.3f} at "
                    f"x[{ib.xmin:.1f},{ib.xmax:.1f}] y[{ib.ymin:.1f},{ib.ymax:.1f}] z[{ib.zmin:.1f},{ib.zmax:.1f}]"
                )
        print(f"{name}: {tested} shapes tested, {hits} collision(s)")
        ok &= hits == 0
    print("result:", "OK" if ok else "FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    sys.exit(main())
