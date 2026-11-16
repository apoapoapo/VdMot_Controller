#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""
3D-printable enclosure for the VdMot_Controller PCB (C2/C3 sample, L-H0044-01-03/-04).

Parts:
  * base     - tray the PCB sits in (rests on a ledge along the board edges)
  * lid      - cover with hold-down pins that clamp the PCB onto the ledge
  * din_clip - optional TS35 DIN rail clip, screwed to the bottom of the base

All coordinates use the PCB frame of the KiCad STEP model (hardware/c3-sample_3D.rar):
  x = 0..155.6 along the long edge (valve jacks X1..X10 from left to right),
  y = 0..86.0 from the valve jack edge (front) to the terminal edge (back),
  z = 0 is the top copper surface, the PCB occupies z = -PCB_T..0.

Usage:
  pip install cadquery
  python3 enclosure.py [--out DIR]

Writes STL files in print orientation and STEP files in assembly orientation.
Requires CadQuery >= 2.4.
"""

import argparse
import math
import os

import cadquery as cq

# ---------------------------------------------------------------------------
# Parameters (mm)
# ---------------------------------------------------------------------------

PCB_W = 155.6  # board size in x
PCB_D = 86.0  # board size in y
PCB_T = 1.6  # board thickness

CLR = 0.3  # gap between PCB edge and wall
WIN_CLR = 0.4  # clearance around connector windows
WALL = 2.0  # wall thickness (all sides)
FLOOR = 2.0  # floor thickness of the base
ROOF = 2.0  # roof thickness of the lid
CORNER_R = 3.0  # outer vertical edge radius

UNDER_PCB = 4.4  # free space below the PCB (THT leads reach ~2 mm below)
TOP_CLEAR = 1.4  # space above the highest part (RJ45 of the WT32-ETH01)
MAX_PART_Z = 25.6  # highest component above the PCB top
SPLIT_Z = 5.0  # height of the base/lid parting plane above the PCB top

LEDGE_W = 1.8  # how far the support ledge reaches under the PCB edge
END_GAP = 6.8  # extra room at both short ends (screw towers live there)

# Screws: 4x M3 from the bottom through the base into the lid.
TOWER_D = 8.0
SCREW_HOLE_D = 3.5  # clearance hole for M3 (printed holes come out smaller)
SCREW_HEAD_D = 6.2  # counterbore for ISO 4762 / ISO 7380 heads
SCREW_HEAD_H = 3.2
# "insert": M3 heat-set insert (tip d ~4.0, knurl d ~4.6-5.0, length <= 6)
# "tap"   : self-tapping directly into the plastic
LID_HOLE_MODE = "insert"
INSERT_D = 4.3  # printed holes come out smaller than modelled
INSERT_DEPTH = 6.5
INSERT_CHAMFER = 0.5  # lead-in at the hole entry
TAP_D = 2.6
TAP_DEPTH = 10.0

# Magnet pockets in the bottom of both end blocks of the base (wall/plate mounting).
MAGNET = True
MAGNET_D = 6.0  # disc magnet diameter
MAGNET_H = 3.0  # disc magnet height
MAGNET_CLR = 0.3  # added to the diameter (press fit, glue recommended)
MAGNET_Y = [20.0, 43.0, 66.0]

# Screw tabs for wall/plate mounting (separate base variant "base_wallmount").
TAB_LEN = 12.0  # how far the tabs stick out at both ends
TAB_W = 26.0
TAB_T = 4.0
TAB_SLOT_D = 4.5  # screw slot width (screws up to 4 mm)
TAB_SLOT_L = 4.0  # slot length along x (tolerance for the hole spacing)

# Hold-down pins in the lid (positions on the PCB that are free of parts).
PIN_D = 4.0
PIN_GAP = 0.1  # pin tip ends this far above the PCB top
PIN_POS = [
    (1.8, 20.0),
    (1.8, 44.0),
    (1.8, 84.0),
    (49.5, 71.5),
    (59.0, 22.5),
    (101.5, 23.0),
    (119.2, 82.8),
    (137.5, 43.5),
    (152.5, 26.0),
    (152.5, 65.5),
]

# 4P4C modular jacks X1..X12 (MEBP 4-4S). The STEP model contains a smaller
# jack (11.5 mm high); height, width and flanges are taken from the datasheet,
# the position of the jack fronts was measured on a test print.
JACK_B = 11.18  # body width
JACK_A = 13.72  # width over the panel-stop flanges
JACK_H = 16.0  # height above the PCB
JACK_PEG_TO_REAR = 10.07  # split-foot pegs to the rear end of the jack (datasheet)
JACK_FLANGE = (1.2, 2.3)  # flange position behind the jack front (= panel thickness 1.2)
JACK_RECESS = 0.1  # jack front behind the outer wall face
JACK_Z0 = 0.8  # the jack body is raised above the PCB at its front (only the rear feet sit on it)
JACK_SKIN = 1.0  # outer wall skin in front of the flanges (datasheet panel: 1.2, minus tolerance)
# (x center, y of the pegs, +1 = opening towards -y / front, -1 = towards +y / back)
JACK_PITCH = (140.31 - 4.11) / 9.0
FRONT_JACKS = [(9.71 + i * JACK_PITCH, 6.10) for i in range(10)]  # X1..X10
BACK_JACKS = [(130.79, 79.90), (145.90, 79.90)]  # X12, X11
# Jack fronts (measured: X1..X10 flush with a wall face at y = -2.3,
# X11/X12 0.5 mm behind a wall face at y = 88.3).
FRONT_JACK_Y = -2.3
BACK_JACK_Y = 87.8
FRONT_LABELS = [str(i + 1) for i in range(10)]

# Other windows in the back wall, from the STEP model: (x_min, x_max, z_min, z_max).
TERMINAL_H = 10.1  # room for the latch arms of the plugs (CTB9209) above the header (7.82 mm)
BACK_WINDOWS = [
    (11.38, 27.42, 12.05, 25.55),  # RJ45 of the WT32-ETH01
    (34.86, 51.34, 0.0, TERMINAL_H),  # X18 1-Wire
    (54.66, 71.14, 0.0, TERMINAL_H),  # X17 RS485
    (74.26, 90.74, 0.0, TERMINAL_H),  # X19 CAN
    (102.00, 113.40, 0.0, TERMINAL_H),  # X13 power
]
BACK_LABELS = ["LAN", "1-Wire", "RS485", "CAN", "PWR"]
BACK_JACK_LABELS = ["12", "11"]

# Ventilation slots in the lid roof: rows of slots along y.
VENT_W = 2.0
VENT_PITCH = 5.0
VENT_Y = (30.0, 58.0)
VENT_SPAN = 140.0  # slots spread symmetrically around the enclosure center (also above the ESP)

# Text engraving
TEXT = True
TEXT_DEPTH = 0.6
FONT = "DejaVu Sans"
TITLE = "VdMot Controller"

# DIN rail clip (TS35 x 7.5)
DIN_SCREW_DX = 20.5  # screws at x = center +- DIN_SCREW_DX
DIN_SCREW_DY = 7.0  # y offset of the screws from the clip center (keeps them clear of the B1 header leads)
DIN_PLATE = (50.0, 50.0, 3.0)  # x, y, thickness
DIN_RAIL_W = 35.0
DIN_FLANGE_T = 1.0
DIN_HOOK_GAP = 1.3  # room for the 1.0 mm rail flange

# ---------------------------------------------------------------------------
# Derived dimensions
# ---------------------------------------------------------------------------

IN_X0 = -CLR - END_GAP
IN_X1 = PCB_W + CLR + END_GAP
OUT_X0, OUT_X1 = IN_X0 - WALL, IN_X1 + WALL
# Front and back walls are placed so the jack fronts end up flush with the outside.
# The back jacks stick out less than WALL + CLR over the PCB edge, so the back
# wall gets thinner instead of pushing the jacks behind the outer face.
OUT_Y0 = FRONT_JACK_Y - JACK_RECESS
OUT_Y1 = BACK_JACK_Y + JACK_RECESS
IN_Y0 = min(OUT_Y0 + WALL, -CLR)
IN_Y1 = max(OUT_Y1 - WALL, PCB_D + CLR)
WALL_FRONT, WALL_BACK = IN_Y0 - OUT_Y0, OUT_Y1 - IN_Y1

PCB_BOT = -PCB_T
FLOOR_TOP = PCB_BOT - UNDER_PCB
BASE_BOT = FLOOR_TOP - FLOOR
ROOF_BOT = MAX_PART_Z + TOP_CLEAR
LID_TOP = ROOF_BOT + ROOF

TOWER_X = [-CLR - TOWER_D / 2, PCB_W + CLR + TOWER_D / 2]  # towers also locate the PCB in x
TOWER_Y = [IN_Y0 + 8.0, IN_Y1 - 8.0]
# Centers of the solid end blocks, moved 0.6 mm inwards to keep the outer wall thicker.
MAGNET_X = [(OUT_X0 + LEDGE_W) / 2 + 0.6, (PCB_W - LEDGE_W + OUT_X1) / 2 - 0.6]
TOWERS = [(x, y) for x in TOWER_X for y in TOWER_Y]

DIN_CENTER = (PCB_W / 2, PCB_D / 2)
DIN_SCREWS = [(DIN_CENTER[0] + dx, DIN_CENTER[1] + DIN_SCREW_DY) for dx in (-DIN_SCREW_DX, DIN_SCREW_DX)]


def box(x0, x1, y0, y1, z0, z1):
    return cq.Workplane("XY").box(x1 - x0, y1 - y0, z1 - z0, centered=False).translate((x0, y0, z0))


def rounded_block(x0, x1, y0, y1, z0, z1, r):
    return (
        cq.Workplane("XY")
        .workplane(offset=z0)
        .center((x0 + x1) / 2, (y0 + y1) / 2)
        .rect(x1 - x0, y1 - y0)
        .extrude(z1 - z0)
        .edges("|Z")
        .fillet(r)
    )


def cyl(x, y, z0, z1, d):
    return cq.Workplane("XY").workplane(offset=z0).center(x, y).circle(d / 2).extrude(z1 - z0)


def engrave(body, origin, xdir, normal, text, size):
    """Cut text into a planar face. origin lies on the face, normal points out of the body."""
    if not TEXT:
        return body
    plane = cq.Plane(origin=origin, xDir=xdir, normal=normal)
    try:
        txt = cq.Workplane(plane).text(
            text, size, -TEXT_DEPTH, combine=False, font=FONT, kind="bold", halign="center", valign="center"
        )
        return body.cut(txt)
    except Exception as exc:  # missing font etc. - the enclosure is still usable without text
        print(f"warning: text '{text}' skipped ({exc})")
        return body


def wall_box(side, x0, x1, d0, d1, z0, z1):
    """Box in the front/back wall; d0..d1 is the depth measured inwards from the outer face."""
    if side == "front":
        return box(x0, x1, OUT_Y0 + d0, OUT_Y0 + d1, z0, z1)
    return box(x0, x1, OUT_Y1 - d1, OUT_Y1 - d0, z0, z1)


def wall_cuts():
    """All connector cutouts in the front/back wall: (side, x0, x1, z0, z1, d0, d1), clearances included."""
    cuts = []
    through = (-1.0, WALL + 1.0)
    jack_z = (JACK_Z0 - 0.3, JACK_H + WIN_CLR)
    flange_d = (JACK_RECESS + JACK_SKIN, WALL + 1.0)
    for side, jacks in (("front", FRONT_JACKS), ("back", BACK_JACKS)):
        for xc, _ in jacks:
            # Opening for the jack body in the outer skin, wider relief behind it for the flanges.
            hb, ha = JACK_B / 2 + WIN_CLR, JACK_A / 2 + 0.3
            cuts.append((side, xc - hb, xc + hb, *jack_z, *through))
            cuts.append((side, xc - ha, xc + ha, *jack_z, *flange_d))
    for x0, x1, z0, z1 in BACK_WINDOWS:
        z0 -= WIN_CLR if z0 > 0 else 0.2
        cuts.append(("back", x0 - WIN_CLR, x1 + WIN_CLR, z0, z1 + WIN_CLR, *through))
    return cuts


def windows(which):
    """Cutters for the base (open towards the top) or the lid (open towards the bottom)."""
    solids = []
    for side, x0, x1, z0, z1, d0, d1 in wall_cuts():
        if which == "base" and z0 < SPLIT_Z:
            solids.append(wall_box(side, x0, x1, d0, d1, z0, SPLIT_Z + 1))
        if which == "lid" and z1 > SPLIT_Z:
            solids.append(wall_box(side, x0, x1, d0, d1, SPLIT_Z - 1, z1))
    return solids


def tongues():
    """Base wall raised below windows that start above the parting plane (RJ45),
    so the gap under the connector is closed. The lid notch slides over it."""
    solids = []
    for side, x0, x1, z0, _, d0, d1 in wall_cuts():
        if z0 >= SPLIT_Z + 1.0 and d0 < 0:
            t = WALL_FRONT if side == "front" else WALL_BACK
            solids.append(wall_box(side, x0 + 0.3, x1 - 0.3, 0, t, SPLIT_Z - 0.1, z0))
    return solids


# ---------------------------------------------------------------------------
# Base
# ---------------------------------------------------------------------------


def make_base():
    body = rounded_block(OUT_X0, OUT_X1, OUT_Y0, OUT_Y1, BASE_BOT, SPLIT_Z, CORNER_R)
    body = body.cut(rounded_block(IN_X0, IN_X1, IN_Y0, IN_Y1, FLOOR_TOP, SPLIT_Z + 1, 1.0))

    # Support ledges under the PCB edges (top = PCB bottom).
    ledges = [
        box(IN_X0, LEDGE_W, IN_Y0, IN_Y1, FLOOR_TOP - 0.1, PCB_BOT),
        box(PCB_W - LEDGE_W, IN_X1, IN_Y0, IN_Y1, FLOOR_TOP - 0.1, PCB_BOT),
        box(IN_X0, IN_X1, IN_Y0, LEDGE_W, FLOOR_TOP - 0.1, PCB_BOT),
        box(IN_X0, IN_X1, PCB_D - LEDGE_W, IN_Y1, FLOOR_TOP - 0.1, PCB_BOT),
    ]
    # The jacks stick out over the front/back PCB edge, so the walls are further
    # out than the board. These rims locate the board edges (top stays below the
    # jack bodies, which start at the PCB top).
    if -CLR - IN_Y0 > 0.4:
        ledges.append(box(IN_X0, IN_X1, IN_Y0, -CLR, PCB_BOT, -0.3))
    if IN_Y1 - PCB_D - CLR > 0.4:
        ledges.append(box(IN_X0, IN_X1, PCB_D + CLR, IN_Y1, PCB_BOT, -0.3))
    for ledge in ledges:
        body = body.union(ledge)

    # Screw towers with counterbore from below.
    for x, y in TOWERS:
        body = body.union(cyl(x, y, FLOOR_TOP - 0.1, SPLIT_Z, TOWER_D))
        body = body.cut(cyl(x, y, BASE_BOT - 1, SPLIT_Z + 1, SCREW_HOLE_D))
        body = body.cut(cyl(x, y, BASE_BOT - 1, BASE_BOT + SCREW_HEAD_H, SCREW_HEAD_D))

    # Connector windows (open to the top) and the tongue below the RJ45.
    for t in tongues():
        body = body.union(t)
    for w in windows("base"):
        body = body.cut(w)

    # Magnet pockets, open to the bottom.
    if MAGNET:
        for x in MAGNET_X:
            for y in MAGNET_Y:
                body = body.cut(cyl(x, y, BASE_BOT - 1, BASE_BOT + MAGNET_H + 0.2, MAGNET_D + MAGNET_CLR))

    # Holes for the DIN rail clip / wall mounting.
    for x, y in DIN_SCREWS:
        body = body.cut(cyl(x, y, BASE_BOT - 1, FLOOR_TOP + 1, SCREW_HOLE_D))

    # Ventilation slots in the floor, left and right of the DIN clip.
    clip_half = DIN_PLATE[0] / 2 + 4.0
    for side in (-1, 1):
        for i in range(6):
            x = DIN_CENTER[0] + side * (clip_half + i * VENT_PITCH * 1.4)
            for y0, y1 in ((20.0, 38.0), (48.0, 66.0)):
                body = body.cut(box(x - VENT_W / 2, x + VENT_W / 2, y0, y1, BASE_BOT - 1, FLOOR_TOP + 1))

    # Small chamfer on the bed edge against elephant foot.
    body = body.faces("<Z").edges("not %CIRCLE").chamfer(0.5)
    return body


def add_wall_tabs(base):
    """Base variant with screw tabs at both short ends for wall/plate mounting."""
    yc = PCB_D / 2
    for side in (-1, 1):
        x_wall = OUT_X0 if side < 0 else OUT_X1
        x_out = x_wall + side * TAB_LEN
        tab = (
            cq.Workplane("XY")
            .workplane(offset=BASE_BOT)
            .center((x_wall - side * 1.0 + x_out) / 2, yc)
            .rect(abs(x_out - x_wall) + 1.0, TAB_W)
            .extrude(TAB_T)
            .edges("|Z")
            .fillet(4.0)
        )
        xs = x_wall + side * TAB_LEN / 2
        slot = (
            cq.Workplane("XY")
            .workplane(offset=BASE_BOT - 1)
            .center(xs, yc)
            .slot2D(TAB_SLOT_L + TAB_SLOT_D, TAB_SLOT_D, angle=0)
            .extrude(TAB_T + 2)
        )
        base = base.union(tab).cut(slot)
    return base


# ---------------------------------------------------------------------------
# Lid
# ---------------------------------------------------------------------------


def make_lid():
    body = rounded_block(OUT_X0, OUT_X1, OUT_Y0, OUT_Y1, SPLIT_Z, LID_TOP, CORNER_R)
    body = body.cut(rounded_block(IN_X0, IN_X1, IN_Y0, IN_Y1, SPLIT_Z - 1, ROOF_BOT, 1.0))

    # Screw towers.
    for x, y in TOWERS:
        body = body.union(cyl(x, y, SPLIT_Z, ROOF_BOT + 0.1, TOWER_D))
        if LID_HOLE_MODE == "insert":
            body = body.cut(cyl(x, y, SPLIT_Z - 1, SPLIT_Z + INSERT_DEPTH, INSERT_D))
            r = INSERT_D / 2
            lead_in = cq.Solid.makeCone(r + INSERT_CHAMFER + 1, r, INSERT_CHAMFER + 1, cq.Vector(x, y, SPLIT_Z - 1))
            body = body.cut(cq.Workplane("XY").add(lead_in))
        else:
            body = body.cut(cyl(x, y, SPLIT_Z - 1, SPLIT_Z + TAP_DEPTH, TAP_D))

    # Hold-down pins.
    for x, y in PIN_POS:
        pin = cyl(x, y, PIN_GAP, ROOF_BOT + 0.1, PIN_D).faces("<Z").edges().chamfer(0.4)
        body = body.union(pin)

    # Connector windows (open to the bottom).
    for w in windows("lid"):
        body = body.cut(w)

    # Ventilation slots in the roof, keeping away from the pins.
    k = int(VENT_SPAN / 2 / VENT_PITCH)
    for i in range(-k, k + 1):
        x = (OUT_X0 + OUT_X1) / 2 + i * VENT_PITCH
        y0, y1 = VENT_Y
        if any(abs(px - x) < PIN_D / 2 + VENT_W / 2 + 1.0 and y0 - 3 < py < y1 + 3 for px, py in PIN_POS):
            continue
        slot = (
            cq.Workplane("XY")
            .workplane(offset=ROOF_BOT - 1)
            .center(x, (y0 + y1) / 2)
            .slot2D(y1 - y0, VENT_W, angle=90)
            .extrude(ROOF + 2)
        )
        body = body.cut(slot)

    # Vent slots in both end walls (above the screw towers' height range of the base).
    for x_face in (OUT_X0, OUT_X1):
        for j in range(6):
            yc = 27.0 + j * 6.0
            slot = (
                cq.Workplane("YZ")
                .workplane(offset=x_face - 3)
                .center(yc, (SPLIT_Z + ROOF_BOT) / 2 + 2)
                .slot2D(12.0, VENT_W, angle=90)
                .extrude(6)
            )
            body = body.cut(slot)

    # Labels.
    label_z = (JACK_H + WIN_CLR + ROOF_BOT) / 2 + 0.5
    for (xc, _), txt in zip(FRONT_JACKS, FRONT_LABELS):
        body = engrave(body, (xc, OUT_Y0, label_z), (1, 0, 0), (0, -1, 0), txt, 5.0)
    for (xc, _), txt in zip(BACK_JACKS, BACK_JACK_LABELS):
        body = engrave(body, (xc, OUT_Y1, label_z), (-1, 0, 0), (0, 1, 0), txt, 4.0)
    for (x0, x1, _, z1), txt in zip(BACK_WINDOWS, BACK_LABELS):
        if z1 > 20:  # RJ45 window reaches the roof, no room above it
            continue
        size = 4.0 if len(txt) <= 3 else 3.2
        body = engrave(body, ((x0 + x1) / 2, OUT_Y1, z1 + 5.5), (-1, 0, 0), (0, 1, 0), txt, size)
    body = engrave(body, (PCB_W / 2, 12.0, LID_TOP), (1, 0, 0), (0, 0, 1), TITLE, 7.0)
    return body


# ---------------------------------------------------------------------------
# DIN rail clip (TS35). Separate part, screwed to the base floor.
# Local frame: plate top (contact face to the base) at z = 0, rail below,
# rail runs along x, centered on y = 0.
# ---------------------------------------------------------------------------


def make_din_clip():
    px, py, pt = DIN_PLATE
    half = DIN_RAIL_W / 2
    hook_z0 = -pt - DIN_HOOK_GAP - 1.6  # bottom of the hook lips
    lip_top = -pt - DIN_HOOK_GAP  # rail flange sits between plate and lip

    clip = box(-px / 2, px / 2, -py / 2, py / 2, -pt, 0)

    # Fixed hooks on the +y side.
    for x0, x1 in ((-px / 2, -px / 2 + 16), (px / 2 - 16, px / 2)):
        clip = clip.union(box(x0, x1, half + 0.2, half + 2.4, hook_z0, -pt))
        lip = (
            cq.Workplane("YZ")
            .polyline([(half + 0.3, hook_z0), (half - 1.8, lip_top - 0.6), (half - 1.8, lip_top), (half + 0.3, lip_top)])
            .close()
            .extrude(x1 - x0)
            .translate((x0, 0, 0))
        )
        clip = clip.union(lip)

    # Spring latch on the -y side: a cantilever beam along x, cut free from the plate.
    beam_y0, beam_y1 = -half - 2.6, -half - 0.6  # beam (2.0 mm thick in y)
    root_x, tip_x = -px / 2 + 4, px / 2 - 4
    clip = clip.cut(box(root_x + 2, px / 2 + 1, -py / 2 - 1, beam_y0 + 0.0, -pt - 1, 1))  # outer slot
    clip = clip.cut(box(root_x + 2, px / 2 + 1, beam_y1, beam_y1 + 1.4, -pt - 1, 1))  # inner slot
    clip = clip.union(box(root_x, tip_x, beam_y0, beam_y1, -pt, 0))
    # Hook at the free end with a ramp so the clip snaps onto the rail.
    hx0, hx1 = tip_x - 14, tip_x
    clip = clip.union(box(hx0, hx1, beam_y0, beam_y1, hook_z0, -pt))
    lip = (
        cq.Workplane("YZ")
        .polyline([(beam_y1, hook_z0), (-half + 1.8, lip_top - 0.6), (-half + 1.8, lip_top), (beam_y1, lip_top)])
        .close()
        .extrude(hx1 - hx0)
        .translate((hx0, 0, 0))
    )
    clip = clip.union(lip)
    # Pull tab to release the latch with a screwdriver.
    clip = clip.union(box(tip_x - 6, tip_x, beam_y0 - 4.0, beam_y0 + 0.01, -pt, 0))
    clip = clip.cut(box(tip_x - 4.5, tip_x - 1.5, beam_y0 - 3.0, beam_y0 - 1.5, -pt - 1, 1))

    # Screw bosses with hex nut traps (M3, 5.5 mm AF) in the free space between the rail flanges.
    dy = DIN_SCREW_DY
    for dx in (-DIN_SCREW_DX, DIN_SCREW_DX):
        clip = clip.union(cyl(dx, dy, -pt - 3.0, -pt + 0.1, 8.0))
        clip = clip.cut(cyl(dx, dy, -pt - 4, 1, SCREW_HOLE_D))
        nut = cq.Workplane("XY").workplane(offset=-pt - 3.1).center(dx, dy).polygon(6, 5.5 / math.cos(math.pi / 6) + 0.3).extrude(2.6)
        clip = clip.cut(nut)
    return clip


def jack_envelopes():
    """Datasheet envelope (body + panel-stop flanges) of the 12 modular jacks, for check_fit.py."""
    solids = []
    for jacks, front, d in ((FRONT_JACKS, FRONT_JACK_Y, 1), (BACK_JACKS, BACK_JACK_Y, -1)):
        for xc, peg in jacks:
            # Front part (wall region) is raised by JACK_Z0, the rear part sits on the PCB.
            ys_front = sorted((front, front + d * 3.0))
            ys_rear = sorted((front + d * 3.0, peg + d * JACK_PEG_TO_REAR))
            fs = sorted((front + d * JACK_FLANGE[0], front + d * JACK_FLANGE[1]))
            solids.append(box(xc - JACK_B / 2, xc + JACK_B / 2, *ys_front, JACK_Z0, JACK_H).val())
            solids.append(box(xc - JACK_B / 2, xc + JACK_B / 2, *ys_rear, 0, JACK_H).val())
            solids.append(box(xc - JACK_A / 2, xc + JACK_A / 2, *fs, JACK_Z0, JACK_H).val())
    return solids


def din_clip_in_assembly(clip):
    return clip.translate((DIN_CENTER[0], DIN_CENTER[1], BASE_BOT))


# ---------------------------------------------------------------------------


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "models"))
    ap.add_argument("--no-text", action="store_true", help="skip label engraving")
    args = ap.parse_args()
    global TEXT
    if args.no_text:
        TEXT = False
    os.makedirs(args.out, exist_ok=True)

    base = make_base()
    base_wall = add_wall_tabs(base)
    lid = make_lid()
    clip = make_din_clip()

    # STEP files in assembly position (PCB frame).
    cq.exporters.export(base, os.path.join(args.out, "vdmot_enclosure_base.step"))
    cq.exporters.export(base_wall, os.path.join(args.out, "vdmot_enclosure_base_wallmount.step"))
    cq.exporters.export(lid, os.path.join(args.out, "vdmot_enclosure_lid.step"))
    cq.exporters.export(din_clip_in_assembly(clip), os.path.join(args.out, "vdmot_enclosure_din_clip.step"))

    # STL files in print orientation, resting on z = 0.
    base_p = base.translate((0, 0, -BASE_BOT))
    base_wall_p = base_wall.translate((0, 0, -BASE_BOT))
    lid_p = lid.rotate((0, 0, 0), (1, 0, 0), 180).translate((0, 0, LID_TOP))
    clip_p = clip.rotate((0, 0, 0), (1, 0, 0), 180)
    parts = (("base", base_p), ("base_wallmount", base_wall_p), ("lid", lid_p), ("din_clip", clip_p))
    for name, part in parts:
        cq.exporters.export(part, os.path.join(args.out, f"vdmot_enclosure_{name}.stl"), tolerance=0.02, angularTolerance=0.1)

    bb = base.union(lid).val().BoundingBox()
    print(f"outer size: {bb.xlen:.1f} x {bb.ylen:.1f} x {bb.zlen:.1f} mm")
    print(f"written to {args.out}")


if __name__ == "__main__":
    main()
