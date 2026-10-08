#!/usr/bin/env python3
"""Stage Wuhu Island for the native g3d: Wii Sports Resort's island -> DATA/wuhu/island.ng3d (NG3D 2).

    tools/wuhu/stage_wuhu.py DATA [--wsr ROOT] [--bake TOOL | --no-bake] [--pvs TOOL | --no-pvs]
                             [--keep-raw] [--name NAME] [--json OUT] [--no-prelit] [--no-pos16]

ROOT is your Wii Sports Resort extraction (default ../wii-sports-extract/wii-sports-resort next to
this repository; files/ under it is the disc's root). DATA is the port's data directory (the one
tools/stage_data.py makes); the package goes to DATA/wuhu/NAME.ng3d (NAME default island), a summary
of what went into it to DATA/wuhu/NAME.json. Nothing of the disc is copied anywhere else, and nothing
it makes belongs in the repository.

What goes in (docs/wuhu.md has the reasons and the numbers; docs/wuhu_design.md sections 0.3, 8):
  - one static world cut into cells of CELL units on x/z (triangles by centroid, a placed object
    whole by its position), each with three detail levels (LOD_DISTANCES): level 0 WS2_common_island
    as Island Flyover (mode 13) shows it, Wedge Island included, with WS2_common_island_V, the
    placed objects of PlnEnv0.carc/WS2_pln_island_tag0.pmp group 1 (what the Flyover's 1-player mode
    spawns: exclude bit 2 skipped) that stand still -- trees (full models), boats at anchor, the
    fountain, the venues' gates, and as detail (BATCH_DETAIL, culled beyond DETAIL_DISTANCE)
    parasols, tents, kayaks, flags at rest, poles, tees, buoys and the venues' small
    props -- and the sea by the shores and inland (lake, pool, falls, shore foam, waves, ripples);
    level 1 the far island (WS2_common_island_R, WSR's own low-detail copy, with a few of the
    island's nodes it lacks), the waters, the trees' medium models, the boats and gates; level 2 the
    far island and its waters with the trees' low models. One batch a (cell, material, detail) in a level's list; slivers of a
    material cut off at a cell's edge go to the neighbour with the rest of it.
  - the moving ones as models placed by instances with their own node animation: windmills,
    balloons, the airship, moving cars, cruisers and the liner on their routes, whales, gulls and
    hawks on circles; small ones culled by distance (INSTANCE_CULL).
  - the open sea (WS2_common_sea's plane away from the shores; sea2_dummy -> sea2_day) and
    WS2_common_vr (the sky, with the sun in it) as models; the island's, sea's, sky's, sun's and
    fountain's texture and colour animations.
  - the ground-level day environment (Bic F0's lights and light sets; the fog table, but no material
    fogged), k = 10, markers (spawn, landmarks, camera presets, the exploration menu's warps, each
    checked on the collision) and the island's collision (WS2_common_island1..5.kcl, tree trunks as
    cylinders).
  - for speed (NG3D version 2): positions as s16 steps of 1/12 unit about each buffer's origin
    (VA_POS16), static lighting baked into the vertex colours of every lit material whose colours no
    animation drives, but the windmills' (MAT_PRELIT, no normals where nothing else needs them;
    LIVE_LIT_PLACED), the cells' visibility rows (CVIS, from the host tool ng3d-pvs: TOOL, default
    build-host/ng3d-pvs).
The package is baked: after writing it, TOOL UNBAKED OUT runs (the renderer's ng3d-bake, default
build-host/ng3d-bake, which converts the GX texels to PICA ones offline), with UNBAKED =
DATA/wuhu/NAME.unbaked.ng3d (kept with --keep-raw: tools/g3d/preview.py reads GX texels) and OUT =
DATA/wuhu/NAME.ng3d. Without the tool it stops: the 3DS refuses an unbaked package. --no-bake
writes one anyway (the host converts GX texels as it loads). --no-prelit and --no-pos16 make
packages for comparisons.
"""
import argparse
import json
import math
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import time
from collections import Counter, OrderedDict

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, "tools"))
sys.path.insert(0, os.path.join(ROOT, "tools/g3d"))
import brres  # noqa: E402
import convert  # noqa: E402
import ng3d  # noqa: E402
import nintendo_formats as nf  # noqa: E402
import shadows  # noqa: E402
from ng3d import NONE16  # noqa: E402

DEFAULT_WSR = os.path.join(os.path.dirname(ROOT), "wii-sports-extract/wii-sports-resort")
# the renderer's offline converter of GX texels to PICA ones (tools/ng3d_bake.cpp, the host build):
# the 3DS loads only baked packages (converting at load time would race the GX path's texture
# uploads, which share pica_tex.h's tables, on the GPU thread)
DEFAULT_BAKE = os.path.join(ROOT, "build-host", "ng3d-bake")
# the cells' visibility rows (CVIS) come from tools/ng3d_pvs.cpp (the host build's ng3d-pvs)
DEFAULT_PVS = os.path.join(ROOT, "build-host", "ng3d-pvs")

# ----------------------------------------------------------------------------------- choices
# Island Flyover's node visibility (main.dol's table at 0x8066A37C, column 13 = Pln day; research
# wsrdol.md section 2): every node of the island and sea models visible but these. Island_22 (a
# villa) is hidden in every mode; Light1-4 are the night lights (island_L, not staged).
HIDDEN_MODE13 = {"Island_16b_Model", "Island_17_Model", "Island_17b_Model", "Island_19_Model",
                 "Island_19c_Model", "Island_20_Model", "Island_20c_Model", "Island_22_Model",
                 "Water16b", "Light1", "Light2", "Light3", "Light4"}

# x/z cells of the static world, units (research gfx.md section 4: 39 non-empty cells, 53-264 draws
# a frame, the largest cell 16k vertices -- within one u16 index space)
CELL = 4000.0
# Detail levels of a cell, by the eye's distance to its box (docs/wuhu.md "Cells and detail levels"
# has the measures behind the numbers):
#   0 = everything: the island at full detail with its edge-blend strips, the trees' full models,
#       every prop, the sea's shore foam, waves and ripples (the strips and the small props are
#       BATCH_DETAIL as well: gone beyond DETAIL_DISTANCE even in a level-0 cell);
#   1 (from LOD1) = the far island: WS2_common_island_R's geometry (WSR's own low-detail copy of
#       the island, drawn mirrored as its water reflection: the same frame, 15k triangles for the
#       whole archipelago, one TEV stage a material, Wedge Island's light model in it), the lake,
#       the falls and the sea by the shores, the trees' medium models (WS2_common_tree_M,
#       tree2_M), the boats and the venues' gates;
#   2 (from LOD2) = the far island and its waters, plus the trees' low models (tree_L, tree2_L):
#       a 230-unit conifer is 4 pixels tall at
#       15,000 units on the 3DS's 240 lines at 45 degrees, a boat 2.
# (Three levels are the format's; their thresholds are distances in the package's world units.)
LOD1, LOD2 = 5000.0, 15000.0
LOD_DISTANCES = [0.0, LOD1, LOD2]
# the tags: which levels a piece is in
TAG_ALL, TAG_PROP, TAG_NEAR, TAG_MID, TAG_FAR, TAG_MID_FAR = 0, 1, 2, 3, 4, 5
TAG_LEVELS = {TAG_ALL: (0, 1, 2), TAG_PROP: (0, 1), TAG_NEAR: (0,), TAG_MID: (1,), TAG_FAR: (2,),
              TAG_MID_FAR: (1, 2)}
# the far island (levels 1-2) is island_R's but for these nodes of the island model, which R lacks
# (the round stage of Swordplay on the water, the court beside the lighthouse) or has without their
# grass for no saving (three islets: 22-28 triangles against 31-59): the island's own (its non-detail
# draws) at every level, R's left out
FAR_FROM_ISLAND = {"Island_21_Model", "Island_3b_Model", "Island_12_Model", "Island_13_Model", "Island_14_Model"}
R_LEFT_OUT = {"Island_12_Model", "Island_13_Model", "Island_14_Model"}
# The cells' visibility rows (CVIS): from each cell, the cells some triangle of which shows from an eye
# over the cell's ground (tools/ng3d_pvs.cpp: cube maps, the cells' opaque triangles occluding as the
# farthest of the detail levels they may be drawn at). Eyes: a column every PVS_SPACING units over the
# cell, at every floor the collision has there (ground or water, not the invisible fences), at heights
# PVS_HEIGHTS (fractions of PVS_MAX_HEIGHT; 2.5 units at the least) over it and under what is above
# it. PVS_MAX_HEIGHT: three heights of the island's Mii (158 Wii no Ma units at k / 1.7, the size the
# island draws it: engine/wuhu/island.cpp MII_SCALE), where the follow camera sits on flat ground and
# a little over; a camera higher over the ground under it (zoomed out, raised, over a drop) doesn't
# use the rows
PVS_MAX_HEIGHT = 80.0
PVS_SPACING = 400.0
PVS_HEIGHTS = (0.0, 0.5, 1.0)
PVS_FACE = 128
# a cell's material with fewer triangles than this (a sliver of a material crossing the cell's edge)
# goes to the neighbouring cell that draws the most of it
SLIVER, SLIVER_MARGIN = 32, 16.0
# BATCH_DETAIL batches (Env::detail_distance culls them by their own box): the edge-blend strips
# laid over the terrain where grass, cliff, sand and earth meet, the sea's shore foam, waves and
# ripples, the decals, and the small props (PMP classes staged "small")
DETAIL_DISTANCE = 3000.0
DETAIL_MATERIALS = {"mt_gake_blend", "mt_kusa_blend", "mt_suna_blend", "mt_tuti_blend", "mt_stage_logo",
                    "png_net_b", "mt_zd_w_kiwa", "mt_zd_w_kiwa_off", "mt_ze_w_nami", "mt_ze_w_nami2",
                    "mt_zb_w_hamon"}
# the sea model keeps the open sea: the sea plane's triangles wider than this (or outside the cells'
# grid); the rest of the sea -- the plane near the shores, the lake, the pool, the falls and
# flows, the shore foam, waves and ripples -- goes into the cells by triangle centroid
SEA_CELL_MAX = CELL / 2

# where the Mii appears (research placement.md section 5): Wuhu Town's plaza south of the coin-toss
# fountain, facing it (-Z); checked on the collision below
SPAWN = (-2029.0, 69.0, 2330.0)
SPAWN_DIR = (0.0, 0.0, -1.0)

# k: Wii no Ma's Mii units per island unit (docs/wuhu_design.md section 8; research k.md). A Mii
# matched to a Mii: both games hang the same RFL head (RFL_Res.dat, byte for byte the same in both)
# from a body node, its origin at the chin. WSR's player body (Common/Kokeshi/common.carc
# G3D/WS2_avaA1.brres, every Kokeshi body alike) has head_set 9.70 above the feet, and the head is
# drawn at scale 0.08 (WSR main.dol's KokeshiManager, fn_80246F70); Wii no Ma's (g3d/Mii/Model.lex)
# has its Head node at 108.86 and the head at scale 0.69 (Wii no Ma's mii_ModelRes_ctor,
# 0x80191368), at height scale 1. With the typical hairstyle's top 69.2 RFL units above the chin, a
# Mii is 9.70 + 0.08 * 69.2 = 15.24 island units and 108.86 + 0.69 * 69.2 = 156.6 Wii no Ma units
# tall: 10.3. Feature by feature k is 9.6-11 (the neck 10.1-11.2, the shoulders 10.0-11.1, the
# channel's default Mii 9.55), and the island's own measures agree (a doubles tennis court is 236.3
# units long, 23.77 m: 10.1 cm a unit, taking a Wii no Ma unit as a centimetre). Rounded: 10. (Stage
# 1's 9.2 came from the spectators' body, whose head_set sits at 9.86 times their placement scale
# 1.2.)
MII_UNITS_PER_ISLAND_UNIT = 10.0
NOMA_MII_HEIGHT = 158.0          # Wii no Ma units, height scale 1 (engine/wuhu/player.h)
NOMA_HEAD_Y, NOMA_HEAD_SCALE = 108.86, 0.69
WSR_HEAD_SCALE, RFL_HAIR_TOP = 0.08, 69.2

# The day's environment as WSR runs it at ground level (research env_choice/env.md: cycling, mode
# 11, the island's own ground-level day): Bic F0's lights, Bic F0's fog table -- whose fog 0 is off,
# as every day .pfog has it: WSR never fogs the terrain, the props or the sky by day -- with
# ws2_common_day.pfobj written into fog 1 and switched on by the island (fn_80275B38 points the island
# at the FogManager's second fog, fn_802757F0 loads the .pfobj there and sets its enable bit), which
# only the open sea's materials (mt_za_water and its _off twin) use.
BIC = "Common/BicScene/common.carc"
ENV_LIGHTS = (BIC, "Env/WS2_bic_F0/WS2_bic_F0_Light.plight")
ENV_FOGS = (BIC, "Env/WS2_bic_F0/WS2_bic_F0_fog.pfog")
ENV_PENV = (BIC, "Env/WS2_bic_F0/WS2_bic_F0_Env.penv")
SEA_FOG = ("Stage/Static/StageArc.carc", "Binary/WS2_common_binary/ws2_common_day.pfobj")
SEA_FOG_INDEX = 1
# whether materials keep their fog (MAT_FOG): not since the speed round (stage_env says why)
KEEP_FOG = False
# EGG's type-0 lights are positions in the world: the sun's (Map_Light, Obj_Light) are 82,000-103,000
# units out. The ones within a few hundred units of the origin (Flag_Light at (-10, 0, 50),
# PlayerObj_Light, Sea_Light) light props thousands of units away: they are directions written as
# positions (env.md section 4), staged as directional lights from there toward the origin.
POINT_LIGHT_MIN_DISTANCE = 10000.0

# The sun (CmnObj class 1, 太陽: Sun/G3D/WS2_common_sun_model, a glow disc and a fan of rays, both
# STD billboards). Its update (main.dol fn_802A8D80) puts it at the scene's .penv position with x and
# z taken relative to the camera -- it stays in one direction in the sky as the eye moves, like the
# sky dome -- and scales it each frame by size * |view z| / (0.5 * 2.39 * 10000 * 456): a constant
# size on the screen (the .penv's size 200: 10,000 model units cover 200 of 456 lines at the
# reference lens), i.e. a constant angle. So it is staged as a part of the sky model, in the .penv's
# direction (Bic F0: (-34,200, 25,000, 93,964), 14 degrees up, 20 degrees west of +z: the visible sun
# is lower than Map_Light, which lights from 35.6 degrees up; WSR draws it at the .penv position, so
# that is where it goes), at SUN_DISTANCE (inside the dome, 62,000-70,000 out, and the camera's far
# plane), turned to face the eye and scaled for that distance. WS2_common_sunshine (a glare over the
# screen) and WS2_common_sun_lensfx (flares along it) are left out: their alpha and place come from
# where the sun is on the screen and a read of the depth buffer (fn_802A89C4, GXPeekZ).
SUN_DISTANCE = 60000.0
SUN_SCREEN = 0.5 * 2.39 * 10000.0 * 456.0

# The sea that moves (docs/wuhu.md "The sea"; engine/g3d/sea.h). WSR's island sea is a flat plane at
# y 0 whose only motion is an indirect warp of jog_waterB by the scrolling jog_waterA (research
# sea/report.md section 1); the engine has no dependent texture reads, so mt_za_water keeps its three
# other layers -- the glare spot projected from the camera, jog_waterB, sea2_day for the placeholder
# sea2_dummy as WSR swaps it -- as three texture units, its texture matrices 2 and 3 (SRT0 sea_P's
# crossing scrolls and the colour mottle) moved to the package's 1 and 2 (SEA_WATER_MATRIX: the
# engine keeps a material's first three), scrolling smoothly every frame. Over it near the eye a grid
# of vertices that follows the camera draws waves (the stager's: WSR's island has none; its jet-ski
# sea's are a CPU height field on a hex grid, research section 2): SEA_WAVES, travelling waves of the
# deep water's speed (sqrt(g wavelength / 2 pi), g = SEA_G units a second squared: 9.81 m at 9 cm a
# unit), a calm bay's swell from one side (within 45 degrees of one wind), each faded out from 1.5 to
# 3 of its wavelengths from the grid's centre (beyond that the grid's vertices, 0.13 of their distance
# apart, are too few for it) and flattened toward the shores by an amplitude map from the collision
# (SEA_AMP: none within `calm` of land, whole from `whole` on; land the triangles that aren't water or
# a fence reaching above y -3). The grid (SEA_GRID): `segments` around and rings from `inner` out to
# `radius` each `segments / 2 pi` further (log-polar: its vertices as far apart along it as across,
# denser near the eye as the screen is), centred under the eye on a lattice of `snap`, drawn while the
# eye is no higher than `height` over the sea. Its material is mt_za_water's (the flat sea's look at
# the rim exactly) times the shade of its vertices (sea.h: the sun's side of a swell lighter, the
# sky's Fresnel reflection) plus SEA_SKY times their brightening (that reflection's gain, the sun's
# glint); SEA_SHADE are those terms' weights (diffuse, sky, glint, glint exponent), toward the
# visible sun (the .penv's direction, 14 degrees up). The flat sea at its level -- a triangle with a
# corner there (SEA_LEVEL_TOL) and none higher than SEA_SHORE: the ramps the water rises up the beaches
# by too -- gets a material of its own, MAT_SEA: not drawn where the grid drew (the stencil buffer), so
# a trough never shows the flat plane through it and the rim has no seam; the inland waters that share
# mt_za_water (the lake, the pool, the streams, Wedge Island's rim) keep the plain one.
SEA_LEVEL, SEA_LEVEL_TOL, SEA_SHORE = 0.0, 0.5, 20.0
SEA_WATER = "mt_za_water"
SEA_WATER_MATRIX = {2: 1, 3: 2}
SEA_G = 109.0
SEA_WAVES = (  # (direction it travels, degrees from +z toward +x; wavelength; amplitude; phase)
    (35.0, 300.0, 2.6, 0.0), (65.0, 190.0, 1.6, 1.7), (10.0, 120.0, 0.95, 3.1), (80.0, 75.0, 0.5, 4.4))
SEA_FADE = (1.5, 3.0)
SEA_GRID = dict(radius=1200.0, inner=3.0, segments=48, snap=8.0, height=600.0)
SEA_SHADE = (2.5, 2.2, 1.5, 40.0)
SEA_SKY = (200, 228, 255)
SEA_AMP = dict(texel=128.0, raster=64.0, calm=40.0, whole=400.0, margin=1500.0)
# Wakes (research sea/report.md section 3). WSR's own: the island's cruiser object draws
# Cruiser/G3D/WS2_common_cruiser_ef (two bow-wave skirts and a trailing foam trapezoid, 32 triangles,
# in the cruiser's space) under each moving cruiser, the liner Liner/G3D/WS2_common_move_liner_ef,
# with their SRT0s (the foam scrolling back) and CLR0 frame 0 (the day's colours, baked into the
# registers); staged as parts of the moving cruisers' and the liner's models (WAKES), and as an
# instance of its own (WAKE_CLASS, hidden) that the island puts under the boat the player drives,
# faded by its speed (MAT_RUNTIME: its C0 alpha, which the foam's alpha is multiplied by). And ours:
# a trail behind every boat that moves (INST_TRAIL on TRAIL_CLASSES; world.h), a ribbon along its
# path that curves as it turns and fades over `life` seconds, widening by `spread` a second, of
# ws2f_waterStream's streaks (the wake's foam texture, its alpha times the ribbon's: TRAIL), riding
# the waves; the nearest `boats` of them in one draw.
WAKES = {33: ("Cruiser/G3D/WS2_common_cruiser_ef.brres", "WS2_common_cruiser_ef"),
         40: ("Liner/G3D/WS2_common_move_liner_ef.brres", "WS2_common_move_liner_ef")}
WAKE_CLASS = 0xFFFD
TRAIL_CLASSES = (7, 8, 33, 40)
TRAIL = dict(boats=4, samples=24, spacing=14.0, life=5.0, spread=7.0, speed=70.0)

# Wedge Island (Island_16) is in the cells like the rest of the island: its full model and sea rim
# (Water16) at levels 0-1, island_R's light model of it (Island_16b_model, as WSR's ground-level
# modes show it: research env.md, the two can't be told apart from the main island) at level 2.
# Water16b (16b's own rim: its pond and waves) is left out: the pond is Water16's, the waves detail.
WEDGE_FAR_NODES = ("Island_16b_Model", "Water16b")

# The exploration menu's places (docs/wuhu_design.md section 8): markers of kind MK_WARP in the order
# the menu lists them -- the town, the beaches and the coasts, the mountain, the castle and the
# ruins, then the islands. The spots are the research's (reach/warps.json: its walk graph over the
# island's collision at 50-degree slopes and 8-unit steps): each on walkable ground with 10 units of
# it all round, facing its landmark when that is more than 40 units off, else the volcano or the
# main island. The 27 places you can't walk to (islets, Wedge Island, the villa cove, the summit
# rim, the fenced courts, ships' decks, sea caves, cliff-ringed plateaus), and 13 of the landmarks
# you can, a handful a region. Entries: (LandMarkData index, whose name the menu shows, or None and
# a name of our own; "+..." appends to the landmark's name), feet (x, y, z), yaw (degrees: x += sin,
# z += cos). Each is checked on the collision when staged (warp_check). Two are moved from the
# research's spots to the nearest that pass: the lone cedar's, which stood in its trunk, 17.5 units
# south (-z is north), and the summit rim's, 5 units from its edge, 52.5 units along the rim.
WARPS = [
    # the town
    (28, None, (-2127.5, 69.0, 1972.5), 45.4),        # the coin-toss fountain, the plaza
    (3, None, (-1237.5, 161.3, 3857.5), 207.4),       # the liner's deck, no gangway
    (25, None, (-4297.5, 200.0, 2202.5), 134.9),      # the Swordplay stage, over water
    (25, "+の観客席", (-4617.5, 192.0, 2177.5), 131.3),  # its stands
    (18, None, (-4382.5, 504.0, 2.5), 122.2),
    # the beaches and the coasts
    (17, None, (6002.5, 64.3, 757.5), 221.8),
    (1, None, (4657.5, 181.4, -742.5), 221.8),        # the hotel
    (54, None, (4877.5, 179.2, -1892.5), 236.5),      # the tennis court, fenced
    (53, None, (8202.5, 54.5, 5022.5), 236.8),
    (52, None, (7082.5, 31.5, 9562.5), 217.4),
    (51, None, (6862.5, 725.0, 8077.5), 220.9),       # the sandy hill, ringed by cliffs
    (6, None, (-6942.5, 989.0, -117.5), 296.7),       # the lighthouse's cape
    (40, None, (-7417.5, 68.6, 232.5), 114.1),        # the sea cave under it
    (2, None, (-3392.5, 15.8, -5427.5), 73.8),        # the villa cove, under the sea cliffs
    (43, None, (882.5, 40.6, -6947.5), 312.1),        # a sea cave's ledge
    # the mountain
    (0, None, (737.5, 333.0, 1627.5), 168.8),
    (9, None, (-1332.5, 336.7, -1452.5), 127.8),      # the suspension bridge
    (8, None, (-4897.5, 800.3, -1737.5), 107.9),
    (49, None, (2772.5, 2195.0, -4980.0), 319.1),     # by the lone cedar (the highest ground you walk to)
    (65, None, (1852.5, 2123.7, -3957.5), 334.7),     # the rock in the lava
    (38, "+の山頂", (1641.2, 4579.1, -3715.0), 328.5),  # the summit rim
    # the castle and the ruins
    (5, None, (-242.5, 1369.0, -3237.5), 108.0),
    (7, None, (952.5, 829.1, -7502.5), 13.7),
    (58, None, (1137.5, 1633.8, -8452.5), 8.7),       # the ruins' heights, above 58-degree rock
    # the islands
    (56, None, (4247.5, 117.9, -5907.5), 113.4),
    (56, "+の北の小島", (3972.5, 245.0, -6482.5), 317.8),
    (60, None, (-7412.5, 373.8, -4747.5), 59.7),
    (61, None, (-10382.5, 524.3, -7667.5), 54.6),
    (62, None, (-6227.5, 424.1, -10747.5), 28.2),
    (16, None, (6502.5, 423.9, -14442.5), 330.8),
    (None, "北東の小島", (8337.5, 280.7, -13447.5), 323.1),
    (None, "北東の小島（南）", (8792.5, 292.6, -11837.5), 317.9),
    (50, None, (-4827.5, 418.5, 12217.5), 163.9),
    (4, None, (-29787.5, 228.8, 16697.5), 224.8),     # Wedge Island
    (71, None, (-27162.5, 12.6, 17327.5), 333.6),
    (32, None, (-28762.5, 169.2, 13807.5), 336.8),
    (11, None, (-31312.5, 160.0, 16332.5), 119.8),
    (None, "ゴルフ島の北の小島", (-27327.5, 117.1, 12397.5), 117.0),
    (33, None, (-29812.5, 212.6, 20262.5), 126.4),
    (34, None, (-33107.5, 17.0, 14777.5), 116.2),
]
# the body a warp spot must have room for (engine/wuhu/player.cpp PlayerParams::make's fractions of
# the Mii's height at k; reach.md: 20 units of headroom, ground no steeper than 50 degrees)
WARP_HEADROOM = 20.0
WARP_WALKABLE_NY = math.cos(math.radians(50.0))

# the reference views (eye, target, vertical fov): research render_island.py's cameras, and
# env_choice's quay_wedge, from the plaza's sea front at the height of a walking eye toward Hotel
# Nineteen on Wedge Island, 27,000 units off (where Wedge Island's far model shows)
VIEWS = OrderedDict([
    ("town", ((-2350.0, 115.0, 2350.0), (-1500.0, 170.0, 1250.0), 55.0)),
    ("coast", ((8300.0, 105.0, 3000.0), (6000.0, 150.0, -1500.0), 55.0)),
    ("beach", ((6300.0, 115.0, -200.0), (8700.0, 40.0, 5200.0), 55.0)),
    ("interior", ((1450.0, 2260.0, -3500.0), (2350.0, 2140.0, -4350.0), 65.0)),
    ("distant", ((-7700.0, 1100.0, -300.0), (-29600.0, 300.0, 16100.0), 40.0)),
    ("overview", ((-9000.0, 3500.0, 12000.0), (0.0, 400.0, -2000.0), 50.0)),
    ("wedge", ((-2800.0, 114.0, 3300.0), (-29858.0, 350.0, 16626.0), 50.0)),
])

# CmnObjManager classes (main.dol 0x80669518) by PMP (group 1) type (0x80669858), and how each
# is staged: "static" baked into the cells for levels 0-1 (TAG_PROP; NEAR_STATIC level 0), "small"
# baked for level 0 only (TAG_NEAR) as detail, "tree" baked at up to three detail levels,
# "instance" a model + instance (optionally animated),
# "out" left out (the reason in docs/wuhu.md)
PMP_CLASS = {0: 2, 1: 3, 2: 4, 3: 6, 4: 8, 5: 41, 6: 42, 7: 5, 9: 7, 10: 9, 11: 10, 12: 10, 18: 10, 19: 10,
             13: 11, 15: 11, 24: 11, 25: 11, 35: 12, 33: 13, 14: 14, 16: 15, 20: 16, 22: 16, 23: 16, 21: 17,
             26: 18, 27: 19, 28: 19, 29: 20, 30: 21, 31: 22, 32: 23, 34: 24, 36: 25, 37: 26, 39: 27, 40: 29,
             41: 28, 42: 30, 43: 31, 45: 32, 46: 33, 48: 34, 49: 35, 50: 36, 51: 37, 52: 38, 53: 39, 54: 40}
SA = "Stage/Static/StageArc.carc"
CLASSES = {
    2: ("conifer", "tree", "Tree/G3D/WS2_common_tree.brres", ("WS2_common_tree_H", "WS2_common_tree_M", "WS2_common_tree_L")),
    3: ("broadleaf tree", "tree", "Tree/G3D/WS2_common_tree2.brres", ("WS2_common_tree2_H", "WS2_common_tree2_M", "WS2_common_tree2_L")),
    4: ("palm", "tree", "Tree/G3D/WS2_common_tree3.brres", ("WS2_common_tree3", None)),
    5: ("seagull", "instance", "Bird/G3D/WS2_common_bird.brres", ("WS2_common_bird",)),
    6: ("airship", "instance", "Airship/G3D/WS2_common_airship.brres", ("WS2_common_airship",)),
    7: ("cruiser (at anchor)", "instance", "Cruiser/G3D/WS2_common_cruiser.brres", ("WS2_common_cruiser",)),
    8: ("yacht", "instance", "Yacht/G3D/WS2_common_yacht.brres", ("WS2_common_yacht",)),
    9: ("parasol", "small", "Parasol/G3D/WS2_common_parasol.brres", ("WS2_common_parasol",)),
    10: ("flag A", "small", "Flag/G3D/WS2_common_FlagA1.brres", ("WS2_common_FlagA1",)),
    11: ("flag B", "small", "Flag/G3D/WS2_common_FlagB1.brres", ("WS2_common_FlagB1",)),
    12: ("flag C", "small", "Flag/G3D/WS2_common_FlagC1.brres", ("WS2_common_FlagC1",)),
    13: ("golf flag", "small", "Flag/G3D/WS2_common_glf_flag.brres", ("WS2_common_glf_flag",)),
    14: ("tent", "small", "Tent/G3D/WS2_common_tent.brres", ("WS2_common_tent",)),
    15: ("parked car", "instance", "Car/G3D/WS2_common_car.brres", ("WS2_common_car",)),
    16: ("balloon", "instance", "Balloon/G3D/WS2_common_balloon.brres", ("WS2_common_balloon",)),
    17: ("hermit crab", "out", None, ()),
    18: ("windmill", "instance", "WindMill/G3D/WS2_common_windmill.brres", ("WS2_common_windmill",)),
    19: ("kayak", "small", "Kayak/G3D/WS2_common_kayak.brres", ("WS2_common_kayak",)),
    20: ("pole", "small", "Pole/G3D/WS2_common_pole.brres", ("WS2_common_pole",)),
    21: ("fountain", "static", "Fountain/G3D/WS2_common_fountain.brres", ("WS2_common_fountain",)),
    22: ("waterfall (a particle effect)", "out", None, ()),
    23: ("hawk", "instance", "Hawk/G3D/WS2_common_hawk.brres", ("WS2_common_hawk",)),
    24: ("golf tee", "small", "GlfTee/G3D/WS2_common_tee.brres", ("WS2_common_tee",)),
    25: ("fireworks (night)", "out", None, ()),
    26: ("moving car", "instance", "Car/G3D/WS2_common_car.brres", ("WS2_common_car",)),
    27: ("campfire (night)", "out", None, ()),
    28: ("torch (night)", "out", None, ()),
    29: ("seaplane", "out", None, ()),
    30: ("shooting star (an effect)", "out", None, ()),
    31: ("sea no-entry buoys", "small", "SeaGuard/G3D/WS2_common_seaguard.brres", ("WS2_common_seaguard",)),
    32: ("whale", "instance", "Whale/G3D/WS2_common_whale.brres", ("WS2_common_whale",)),
    33: ("moving cruiser", "instance", "Cruiser/G3D/WS2_common_cruiser.brres", ("WS2_common_cruiser",)),
    34: ("frisbee-dog plate", "small", "IslandLowObj/G3D/WS2_common_fld_plate.brres", ("WS2_common_fld_plate",)),
    35: ("archery target", "small", "IslandLowObj/G3D/WS2_common_arc_target.brres", ("WS2_common_arc_target",)),
    36: ("jet-ski start gate", "static", "IslandLowObj/G3D/WS2_common_jsk_start.brres", ("WS2_common_jsk_start",)),
    37: ("wakeboard goal gate", "static", "IslandLowObj/G3D/WS2_common_wkb_goal.brres", ("WS2_common_wkb_goal",)),
    38: ("table-tennis table", "small", "IslandLowObj/G3D/WS2_common_png_table.brres", ("WS2_common_png_table",)),
    39: ("cycling gate", "small", "IslandLowObj/G3D/WS2_common_bic_gate.brres", ("WS2_common_bic_gate",)),
    40: ("moving liner", "instance", "Liner/G3D/WS2_common_move_liner.brres", ("WS2_common_move_liner",)),
    41: ("spectator Mii", "out", None, ()),
    42: ("spectator Mii (hands)", "out", None, ()),
}
# the moving ones' speeds, from WSR's update functions where they are known: the liner advances its
# 29-key route by 0.00067 frame a frame (fn_802A5694); a moving cruiser by 20 / the route's length
# (fn_802A4A8C), i.e. 20/29 units a frame along it; the rest play their animation at one frame a
# frame. A gull or hawk circles at 1.9 units a frame, its steering's cap (fn_80281030).
LINER_RATE = 0.00067
CRUISER_SPEED = 20.0 / 29.0
BIRD_SPEED = 1.9
WINDMILL_DESYNC = 37.0         # frames between windmills' phases (they turn together in WSR's data)

# distances beyond which instances aren't drawn (0 = always), from the near side of their
# bounding sphere (World): a 20-unit gull or hawk is 3 pixels across at 2,000 units on the 3DS's 240
# lines at 45 degrees, a 55-unit car 5 at 3,000, a 600-unit whale 22 at 8,000, a 160-unit cruiser 6
# at 8,000; the big ones (airship, liner, balloons, windmills) always. (The cars' and cruisers'
# spheres hold their whole routes: a renderer culling them by where they are now gains most.)
# Parked cars stay available around town; small moving props disappear at their visual thresholds.
INSTANCE_CULL = {5: 2000.0, 7: 15000.0, 8: 15000.0, 15: 12000.0,
                 23: 2000.0, 26: 3000.0, 32: 8000.0, 33: 8000.0}
# The car's four wheels (WS2_common_car is one node, one shape: each wheel is 24 of its 385
# triangles, an outer and an inner face and a tread of 10 quads) cut out onto nodes of their own,
# wheel_fl / fr / rl / rr (front +z, left +x), about their hubs, so that the island turns them (a
# car driven: rolling by the distance, the front pair steered). A wheel: its triangles' corners all
# within 6.05 of the hub in y and z and 9.7..14.7 out from the middle on its side.
WHEEL_HUBS = OrderedDict([("wheel_fl", (12.214, 6.036, 14.791)), ("wheel_fr", (-12.214, 6.036, 14.791)),
                          ("wheel_rl", (12.214, 6.036, -16.174)), ("wheel_rr", (-12.214, 6.036, -16.174))])
WHEEL_RADIUS = 6.0


def wheel_pick(hub):
    def pick(T):
        x, y, z = T[..., 0], T[..., 1], T[..., 2]
        side = np.sign(hub[0])
        inside = ((np.abs(y - hub[1]) <= 6.05) & (np.abs(z - hub[2]) <= 6.05) &
                  (side * x >= 9.7) & (side * x <= 14.7))
        return inside.all(axis=1)
    return pick


CAR_WHEELS = [(n, hub, wheel_pick(hub)) for n, hub in WHEEL_HUBS.items()]
# the "static" props at level 0 only (small next to the boats and the gates)
NEAR_STATIC = {21}


# ----------------------------------------------------------------------------------- sources


class Wsr:
    """The extraction's archives (Yaz0 U8 .carc), unpacked in memory on first use."""

    def __init__(self, root):
        self.root = root
        self.files = os.path.join(root, "files")
        if not os.path.isdir(self.files):
            raise SystemExit("%s: no files/ here (give --wsr the extraction's root)" % root)
        self._arcs = {}
        self._brres = {}

    def arc(self, rel):
        if rel not in self._arcs:
            data = open(os.path.join(self.files, rel), "rb").read()
            data, _ = nf.maybe_decompress(data)
            self._arcs[rel] = {p: data[o:o + s] for p, o, s in nf.u8_list(data) if o is not None}
        return self._arcs[rel]

    def get(self, rel, member):
        return self.arc(rel)[member]

    def brres(self, rel, member):
        key = (rel, member)
        if key not in self._brres:
            self._brres[key] = brres.BRRES(self.get(rel, member), "%s/%s" % (rel, member))
        return self._brres[key]


# ----------------------------------------------------------------------------------- KCL


def kcl_triangles(d):
    """A KCL file's triangles ((n, 3, 3) float64) and attributes (research placement.md section 3:
    prisms of height h on a vertex, a face normal and three edge normals)."""
    o_pos, o_nrm, o_prism, o_oct = struct.unpack_from(">4I", d, 0)
    pos = np.frombuffer(d, ">f4", (o_nrm - o_pos) // 4, o_pos).reshape(-1, 3).astype(np.float64)
    nrm = np.frombuffer(d, ">f4", (o_prism + 0x10 - o_nrm) // 4, o_nrm).reshape(-1, 3).astype(np.float64)
    n = (o_oct - (o_prism + 0x10)) // 0x10
    pr = np.frombuffer(d, np.dtype([("h", ">f4"), ("p", ">u2"), ("f", ">u2"), ("a", ">u2"), ("b", ">u2"),
                                    ("c", ">u2"), ("attr", ">u2")]), n, o_prism + 0x10)
    v1 = pos[pr["p"]]
    fn, na, nb, nc = nrm[pr["f"]], nrm[pr["a"]], nrm[pr["b"]], nrm[pr["c"]]
    h = pr["h"].astype(np.float64)[:, None]
    ca, cb = np.cross(na, fn), np.cross(nb, fn)
    with np.errstate(divide="ignore", invalid="ignore"):
        v2 = v1 + cb * (h / np.einsum("ij,ij->i", cb, nc)[:, None])
        v3 = v1 + ca * (h / np.einsum("ij,ij->i", ca, nc)[:, None])
    return np.stack([v1, v2, v3], 1), pr["attr"].astype(np.int64), fn


def kcl_ground(tris, attrs, normals, x, z, y_from):
    """The highest triangle under (x, z) at or below y_from: (y, attribute, normal) or None."""
    a, b, c = tris[:, 0], tris[:, 1], tris[:, 2]

    def edge(p, q):
        return (q[:, 0] - p[:, 0]) * (z - p[:, 2]) - (q[:, 2] - p[:, 2]) * (x - p[:, 0])
    e0, e1, e2 = edge(a, b), edge(b, c), edge(c, a)
    with np.errstate(invalid="ignore", divide="ignore"):
        inside = ((e0 >= 0) & (e1 >= 0) & (e2 >= 0)) | ((e0 <= 0) & (e1 <= 0) & (e2 <= 0))
        n = np.cross(b - a, c - a)
        y = a[:, 1] - (n[:, 0] * (x - a[:, 0]) + n[:, 2] * (z - a[:, 2])) / n[:, 1]
    ok = inside & np.isfinite(y) & (y <= y_from + 1e-3) & (np.abs(n[:, 1]) > 1e-9)
    if not ok.any():
        return None
    i = np.nonzero(ok)[0][np.argmax(y[ok])]
    return float(y[i]), int(attrs[i]), normals[i]


def kcl_surfaces(tris, attrs, normals, x, z):
    """Every triangle over or under (x, z): (y there, attribute, normal y), highest first. A point
    a hundredth of a unit outside a triangle's footprint counts as under it (two prisms rebuild a
    shared edge apart: engine/collision/kcl.cpp ground() does the same)."""
    a, b, c = tris[:, 0], tris[:, 1], tris[:, 2]
    with np.errstate(invalid="ignore", divide="ignore"):
        n = np.cross(b - a, c - a)

        def edge(p, q):   # signed distance on x/z from the edge's line
            e = (q[:, 0] - p[:, 0]) * (z - p[:, 2]) - (q[:, 2] - p[:, 2]) * (x - p[:, 0])
            return e / np.maximum(np.hypot(q[:, 0] - p[:, 0], q[:, 2] - p[:, 2]), 1e-9)
        e0, e1, e2 = edge(a, b), edge(b, c), edge(c, a)
        tol = 0.01
        inside = ((e0 >= -tol) & (e1 >= -tol) & (e2 >= -tol)) | ((e0 <= tol) & (e1 <= tol) & (e2 <= tol))
        y = a[:, 1] - (n[:, 0] * (x - a[:, 0]) + n[:, 2] * (z - a[:, 2])) / n[:, 1]
    ok = inside & np.isfinite(y) & (np.abs(n[:, 1]) > 1e-9)
    i = np.nonzero(ok)[0]
    i = i[np.argsort(-y[i])]
    return y[i], attrs[i], normals[i, 1]


def triangle_distances(p, tris):
    """Distances from point p to each triangle ((n, 3, 3)): to the plane where p's foot falls inside
    the triangle, else to the nearest edge."""
    a, b, c = tris[:, 0], tris[:, 1], tris[:, 2]
    with np.errstate(invalid="ignore", divide="ignore"):
        n = np.cross(b - a, c - a)
        n /= np.linalg.norm(n, axis=1, keepdims=True)
        h = ((p - a) * n).sum(1)
        f = p - h[:, None] * n
        inside = np.ones(len(tris), bool)
        for u, v in ((a, b), (b, c), (c, a)):
            inside &= (np.cross(v - u, f - u) * n).sum(1) >= 0

        def seg(u, v):
            d = v - u
            t = np.clip(((p - u) * d).sum(1) / np.maximum((d * d).sum(1), 1e-12), 0, 1)
            return np.linalg.norm(p - (u + t[:, None] * d), axis=1)
        out = np.where(inside, np.abs(h), np.minimum(np.minimum(seg(a, b), seg(b, c)), seg(c, a)))
    return np.where(np.isfinite(out), out, np.inf)


def warp_body(k):
    """The Mii a warp spot must have room for, in island units at k: engine/wuhu/player.cpp
    PlayerParams::make's measures (radius 0.2 of the height, a step 0.25, the knee sphere a radius
    above the step, the chest one at 0.75)."""
    h = NOMA_MII_HEIGHT / k
    return dict(height=round(h, 3), radius=round(0.2 * h, 3), step=round(0.25 * h, 3),
                knee=round(0.25 * h + 0.2 * h, 3), chest=round(0.75 * h, 3))


def warp_check(kcl, cylinders, pos, body, wall_ny=0.3, ring=5.0):
    """Does the Mii fit at `pos` (feet)? The ground under it (the highest dry surface at most 2 units
    above the feet) within 2 units of them, walkable (no steeper than 50 degrees; not lava, water or
    a fence), and the same within a step all round at `ring` units; no water over it, nothing solid
    between a step and WARP_HEADROOM above it; its two spheres (body: knee, chest, radius) clear of
    walls (fences, and faces steeper than wall_ny: the player's own rule) and of tree trunks.
    -> dict(ok, why, ground, attribute, normal_y, clearance)."""
    tris, attrs, normals = kcl
    x, y, z = pos
    R = max(body["radius"], ring) + 10.0
    lo, hi = tris.min(1), tris.max(1)
    near = ((lo[:, 0] <= x + R) & (hi[:, 0] >= x - R) & (lo[:, 2] <= z + R) & (hi[:, 2] >= z - R) &
            (lo[:, 1] <= y + WARP_HEADROOM + R) & (hi[:, 1] >= y - R - 40))
    T, A, N = tris[near], attrs[near], normals[near]

    def ground_at(px, pz, top):
        ys, a_, ny = kcl_surfaces(T, A, N, px, pz)
        for k in range(len(ys)):
            if ys[k] <= top and a_[k] != 0x1106:
                return float(ys[k]), int(a_[k]), float(ny[k]), ys, a_, ny
        return None

    def walkable(a, ny):
        return ny >= WARP_WALKABLE_NY and a != 0x110B and (a >> 8) not in (0x00, 0x08)

    out = dict(ok=False, why="", ground=None, attribute=None, normal_y=None, clearance=None)
    g = ground_at(x, z, y + 2.0)
    if g is None or abs(g[0] - y) > 2.0:
        out["why"] = "no ground within 2 units of the feet (%s)" % (None if g is None else round(g[0], 2))
        return out
    gy, ga, gny, ys, a_, ny = g
    out.update(ground=round(gy, 2), attribute="%04x" % ga, normal_y=round(gny, 4))
    if not walkable(ga, gny):
        out["why"] = "ground %04x, normal y %.3f: not walkable" % (ga, gny)
        return out
    above = ys > gy + 1e-3
    wet = above & (a_ == 0x1106) & (ys <= gy + body["height"])
    if wet.any():
        out["why"] = "water %.1f above the feet" % float(ys[wet].min() - gy)
        return out
    roof = above & (a_ != 0x1106) & (ys > gy + body["step"]) & (ys < gy + WARP_HEADROOM)
    if roof.any():
        out["why"] = "a ceiling %.1f above the feet" % float(ys[roof].min() - gy)
        return out
    solid = (((A >> 8) == 0x00) | ((A >> 8) == 0x08) | (N[:, 1] < wall_ny)) & (A != 0x1106)
    clear = np.inf
    for hgt in (body["knee"], body["chest"]):
        d = triangle_distances(np.array([x, gy + hgt, z]), T[solid])
        clear = min(clear, float(d.min()) if len(d) else np.inf)
    out["clearance"] = round(clear, 2) if np.isfinite(clear) else None
    if clear < body["radius"]:
        out["why"] = "in a wall (%.2f from one, the body's radius %.2f)" % (clear, body["radius"])
        return out
    for cx, cz, y0, y1, cr in cylinders:
        if y0 < gy + body["height"] and y1 > gy and math.hypot(x - cx, z - cz) < cr + body["radius"]:
            out["why"] = "in a tree trunk at (%.0f, %.0f)" % (cx, cz)
            return out
    for k in range(8):
        th = math.pi * k / 4
        r = ground_at(x + ring * math.sin(th), z + ring * math.cos(th), gy + body["step"])
        if r is None or abs(r[0] - gy) > body["step"] or not walkable(r[1], r[2]):
            out["why"] = "the ground %g units off at %d degrees is %s" % (
                ring, 45 * k, "missing" if r is None else "%.1f off, %04x, normal y %.2f" % (r[0] - gy, r[1], r[2]))
            return out
    out.update(ok=True, why="room (%s)" % ("%.2f from the nearest wall" % clear if np.isfinite(clear) else
                                         "no wall within %.0f" % R))
    return out


def mat_name(mdl, d):
    """The name of a draw's material."""
    return next(m.name for m in mdl.materials if m.index == d.mat)


def landmark_name(l):
    """A LandMarkData name without its number ("28_コイン・トスの噴水" -> "コイン・トスの噴水")."""
    return l["name"].split("_", 1)[1] if "_" in l["name"] else l["name"]


# ----------------------------------------------------------------------------------- env files


def rgba(b):
    return list(b[:4])


def read_plight(d):
    """LGHT v2 (EGG::LightManager): 16 LightObjects of 0x50 at 0x28, then 16 ambient colours (8
    bytes apart). Per light: type 0x12 (0 world point, 1 world directional, ...), flags 0x16,
    position 0x18, aim 0x24, intensity 0x30 (multiplied into the RGB), colour 0x34. A light is on in
    g3d only with flag bits 0 and 0x40 both set (EGG's LightObject, fn_801BC6E0): Abatar_Light
    (0x7A1) is off although bit 0 is set."""
    n, na = struct.unpack_from(">HH", d, 0x10)
    lights = []
    for i in range(n):
        o = 0x28 + 0x50 * i
        typ = d[o + 0x12]
        idx, flags = struct.unpack_from(">HH", d, o + 0x14)
        pos = struct.unpack_from(">3f", d, o + 0x18)
        aim = struct.unpack_from(">3f", d, o + 0x24)
        inten = struct.unpack_from(">f", d, o + 0x30)[0]
        col = d[o + 0x34:o + 0x38]
        eff = [min(255, int(c * inten)) for c in col[:3]] + [col[3]]
        lights.append(dict(index=idx, type=typ, flags=flags, on=bool(flags & 1 and flags & 0x40), pos=pos,
                           aim=aim, color=eff))
    ab = 0x28 + 0x50 * n
    amb = [rgba(d[ab + 8 * i:ab + 8 * i + 4]) for i in range(na)]
    return lights, amb


def read_fogd(d, o=0):
    """FOGD (EGG::Fog): start 0x10, end 0x14, RGBA 0x20, GX fog type 0x24, flags 0x25 (bit 0 on)."""
    st, en = struct.unpack_from(">2f", d, o + 0x10)
    return dict(start=st, end=en, color=rgba(d[o + 0x20:o + 0x24]), type=d[o + 0x24], on=bool(d[o + 0x25] & 1))


def read_penv(d):
    """PENV: the sun object's position (x, y, z at 0x14) and size (0x20), which the scene hands the
    sun (CmnObj class 1) when it makes it."""
    return dict(pos=struct.unpack_from(">3f", d, 0x14), size=struct.unpack_from(">f", d, 0x20)[0])


def read_landmarks(d):
    """LandMarkData.exbin: 118 records of 0xBC from 0x10: radius, kind (0 fixed, 3 riding a vehicle:
    position relative to it, 6 unused), message id, Shift-JIS name at 0x10, position at 0x9C."""
    out = []
    for i in range((len(d) - 0x10) // 0xBC):
        o = 0x10 + i * 0xBC
        kind = struct.unpack_from(">I", d, o + 4)[0]
        name = d[o + 0x10:o + 0x90].split(b"\0")[0].decode("shift_jis", "replace")
        out.append(dict(index=i, kind=kind, name=name, pos=struct.unpack_from(">3f", d, o + 0x9C)))
    return out


def read_campar(d):
    """WS2_seq1_IslandCamParam.exbin: 15 x {eye xyz, target xyz} at 0x10."""
    return [(struct.unpack_from(">3f", d, 0x10 + 24 * i), struct.unpack_from(">3f", d, 0x1C + 24 * i))
            for i in range(15)]


def read_pmp(d):
    """PMPF objects: (group, type, position, scale, Mtx34 as WSR builds it [X*sx | Y*sy | Z*sz | pos],
    include bit, exclude bit, sub id). Research placement.md section 2.1."""
    n = struct.unpack_from(">H", d, 0x10)[0]
    out = []
    for i in range(n):
        b = 0x80 + 0x58 * i
        g, t = struct.unpack_from(">HH", d, b)
        pos = struct.unpack_from(">3f", d, b + 8)
        sc = struct.unpack_from(">3f", d, b + 0x14)
        zx, yx, xx = (struct.unpack_from(">3f", d, b + o) for o in (0x20, 0x2C, 0x38))
        m = np.eye(4)
        for r in range(3):
            m[r, :] = [xx[r] * sc[0], yx[r] * sc[1], zx[r] * sc[2], pos[r]]
        inc, exc, sub = struct.unpack_from(">hhh", d, b + 0x48)
        out.append(dict(index=i, group=g, type=t, filter=d[b + 4], pos=np.array(pos), scale=sc, mtx=m,
                        axes=(np.array(xx), np.array(yx), np.array(zx)), include=inc, exclude=exc, sub=sub))
    return out


def spawned(objs, exclude_mask):
    """What CmnObjManager makes of a PMP's group-1 records (fn_8026D230 / fn_8026D514): filter 0,
    a known type, include bit 0 (the Flyover passes include mask 0), exclude bit not in the mask."""
    out, dropped = [], Counter()
    for o in objs:
        if o["group"] != 1 or o["filter"] != 0 or o["type"] not in PMP_CLASS:
            continue
        cls = PMP_CLASS[o["type"]]
        if o["include"] > 0:
            dropped[cls] += 1
            continue
        if o["exclude"] > 0 and exclude_mask & (1 << (o["exclude"] - 1)):
            dropped[cls] += 1
            continue
        out.append(dict(o, cls=cls))
    return out, dropped


# ----------------------------------------------------------------------------------- staging


def env_lights(w):
    """The day's lights and light sets as the package records them (stage_env adds them in this
    order): ([dict(name, kind, rgba, position, direction, info)], [dict(id, name, lights, ambient)])
    -- Bic F0's lights in the sets of SCN0 WS2_Scene (a material's light set number is the set's
    index)."""
    lights, ambient = read_plight(w.get(*ENV_LIGHTS))
    scn = w.brres("Common/Static/common.carc", "G3D/WS2_Scene.brres")
    scene = next(iter(scn.scenes.values()))
    by_id = {l["index"]: l for l in lights}
    light_index, out = {}, []
    for ls in sorted(scene.light_sets, key=lambda s: s["id"]):
        for nm in ls["lights"]:
            lid = scene.lights[nm]
            l = by_id.get(lid)
            if l is None or not l["on"] or lid in light_index:
                continue
            # EGG type 0: a light at a position in the world (the sun's 48,000-100,000 units off,
            # unattenuated; or a direction written as a position: POINT_LIGHT_MIN_DISTANCE); type 1:
            # directional, from the position toward the aim point. Every light carries that
            # direction (what a directional-only renderer uses)
            pos = np.array(l["pos"], np.float64)
            d = np.array(l["aim"], np.float64) - pos
            d /= max(np.linalg.norm(d), 1e-9)
            point = l["type"] == 0 and np.linalg.norm(pos) >= POINT_LIGHT_MIN_DISTANCE
            light_index[lid] = len(out)
            out.append(dict(name=nm, kind=ng3d.LK_POINT if point else ng3d.LK_DIRECTIONAL, rgba=l["color"],
                            position=[float(x) for x in pos], direction=[float(x) for x in d],
                            info=dict(name=nm, id=lid, egg_type=l["type"], flags="%04x" % l["flags"],
                                      kind="point" if point else "directional", pos=l["pos"],
                                      direction=[round(float(x), 4) for x in d], color=l["color"])))
    sets = []
    for ls in sorted(scene.light_sets, key=lambda s: s["id"]):
        idx = [light_index[scene.lights[nm]] for nm in ls["lights"] if scene.lights[nm] in light_index]
        amb = ambient[scene.ambients[ls["ambient"]]] if ls["ambient"] in scene.ambients else [0, 0, 0, 255]
        sets.append(dict(id=ls["id"], name=ls["name"], lights=idx, ambient=amb))
    return out, sets


# the colour animations staged (stage_anims), by the model whose materials they drive: a material
# whose material or ambient colour one drives keeps its lighting live (not MAT_PRELIT)
CLR0_STAGED = (("WS2_common_island", "WS2_common_island"), ("WS2_common_island_V", "WS2_common_island_V"),
               ("WS2_common_sea", "WS2_common_sea_nami"))


# models placed turned that keep their lighting live (PMP class, model): a model's lighting is baked
# once, at its rest orientation, so every instance would carry that one. The eight windmills stand
# 6-87 degrees apart about y (and their blades turn): baked, six of them were lit from the wrong
# side, 19-42 colour levels off on average and up to 122 (the windmill is 96 vertices). The models
# moving along world routes (cars, cruisers, the liner, the airship, birds, whales) and the
# balloons (3-4 levels off) are baked: their light turns with them (docs/wuhu.md).
LIVE_LIT_PLACED = ((18, "WS2_common_windmill"),)


def live_lit_materials(isl, wsr=None):
    """(model, material) whose channel colours (material or ambient, CLR0 targets 0-3) a staged
    CLR0 animates, and (given the extraction) every material of LIVE_LIT_PLACED's models."""
    out = set()
    for model, anim in CLR0_STAGED:
        for mname, targets in isl.anims["CLR0"][anim].materials.items():
            if any(t in (0, 1, 2, 3) for t in targets):
                out.add((model, mname))
    for cls, model in LIVE_LIT_PLACED if wsr is not None else ():
        out |= {(model, m.name) for m in wsr.brres(SA, CLASSES[cls][2]).models[model].materials}
    return out


class Stager:
    def __init__(self, wsr, prelit=True, pos16=True):
        self.wsr = wsr
        self.pkg = ng3d.Package()
        # Group 5/type 3 records are WSR's shadow-area volumes. Their PMP matrix places a
        # unit box in island space; the bake uses these to fade ambient light in caves and
        # other enclosed areas, while leaving the direct sun untouched.
        tag0 = read_pmp(wsr.get("Stage/PlnScene/PlnEnv0.carc", "WS2_pln_island_tag0.pmp"))
        self.shadow_areas = []
        for obj in tag0:
            if obj["group"] != 5 or obj["type"] != 3:
                continue
            try:
                inv = np.linalg.inv(obj["mtx"])
            except np.linalg.LinAlgError:
                continue
            half_extent = np.linalg.norm(obj["mtx"][:3, :3], axis=0)
            if np.all(half_extent > 1e-6):
                self.shadow_areas.append((inv, half_extent))
        # the lights first: MAT_PRELIT materials take them into their vertex colours as the
        # geometry is converted
        self.lights, self.light_sets = env_lights(wsr)
        self.island = wsr.brres(SA, "Island/G3D/WS2_common_island.brres")
        lighting = convert.Lighting([(l["kind"], l["position"], l["direction"], l["rgba"]) for l in self.lights],
                                    {ls["id"]: (ls["ambient"], ls["lights"]) for ls in self.light_sets},
                                    self.shadow_areas) \
            if prelit else None
        self.keep_lit = live_lit_materials(self.island, wsr)
        self.cv = convert.Converter(self.pkg, lighting, self.keep_lit, pos16)
        self.cells = convert.MeshSet(self.cv)
        self.inv = OrderedDict()           # the inventory written next to the package
        self.cell_groups = {}              # cell -> Counter(node name -> triangles)
        self.mat_by_name = {}              # (model name, material name) -> package material
        self.log = []

    def say(self, *a):
        s = " ".join(str(x) for x in a)
        self.log.append(s)
        print(s, flush=True)

    def cell_of(self, x, z):
        return (int(math.floor(x / CELL)), int(math.floor(z / CELL)))

    # -- materials by name, for animations
    def material(self, r, mdl, mat, tex_override=None, prio=0, variant=None):
        mi = self.cv.material(r, mdl, mat, tex_override, prio, variant=variant)
        if not variant:
            self.mat_by_name[(mdl.name, mat.name)] = mi
        return mi

    def texmtx_of(self, model):
        def f(mname, j):
            mi = self.mat_by_name.get((model, mname))
            if mi is None:
                return None
            m = self.pkg.materials[mi]
            return m.tex_mtx_first + j if j < m.n_tex_mtx else None
        return f

    def material_of(self, model):
        return lambda mname: self.mat_by_name.get((model, mname))

    # -- static geometry into the cells
    def bake_model(self, r, mdl, M, tag, hidden=(), tex_override=None, by_centroid=False, cell=None,
                   group_name=None, tag_of=None, detail=False, select=None, variant=None):
        """Every visible draw of `mdl`, moved by M (4x4), into the cells: by triangle centroid
        (terrain) or all into `cell`. tag_of(node name, material name) -> its tag, or None to leave
        the draw out (default: `tag`); a draw whose material is in DETAIL_MATERIALS, or any if
        `detail`, makes BATCH_DETAIL batches; select(node name, material name, triangles (n, 3, 3))
        -> the triangles to keep; variant: the draws' materials of their own (Converter.material).
        Returns the triangles baked."""
        mats = {m.index: m for m in mdl.materials}
        shapes = {s.index: s for s in mdl.shapes}
        tris = 0
        for d in mdl.draws("DrawOpa") + mdl.draws("DrawXlu"):
            vis = mdl.nodes[d.node].name if 0 <= d.node < len(mdl.nodes) else None
            if vis in hidden:
                continue
            mname = mats[d.mat].name
            dtag = tag if tag_of is None else tag_of(vis, mname)
            if dtag is None:
                continue
            t = mdl.shape_triangles(shapes[d.shape], world=True)
            if not t or "pos" not in t:
                continue
            c = convert.corners_from(t, mdl, shapes[d.shape])
            if M is not None:
                c = convert.transform_corners(c, M)
            if select is not None:
                keep = np.repeat(np.asarray(select(vis, mname, c["pos"].reshape(-1, 3, 3)), bool), 3)
                if not keep.any():
                    continue
                c = {k: (v[keep] if k != "uvfrac" else v) for k, v in c.items()}
            mi = self.material(r, mdl, mats[d.mat], tex_override, d.prio, variant)
            det = detail or mname in DETAIL_MATERIALS
            n = len(c["pos"]) // 3
            tris += n
            if by_centroid:
                cen = c["pos"].reshape(n, 3, 3).mean(1)
                cx = np.floor(cen[:, 0] / CELL).astype(np.int64)
                cz = np.floor(cen[:, 2] / CELL).astype(np.int64)
                for key in set(zip(cx.tolist(), cz.tolist())):
                    sel = np.repeat((cx == key[0]) & (cz == key[1]), 3)
                    self.cells.add(key, mi, {k: (v[sel] if k != "uvfrac" else v) for k, v in c.items()},
                                   TAG_LEVELS[dtag], det)
                    if group_name:
                        self.cell_groups.setdefault(key, Counter())[vis or group_name] += int(sel.sum() // 3)
            else:
                self.cells.add(cell, mi, c, TAG_LEVELS[dtag], det)
        return tris

    # ------------------------------------------------------------------------------- island
    def stage_island(self):
        w = self.wsr
        isl = self.island
        self.seatex = sea = w.brres(SA, "Island/G3D/WS2_common_seatex.brres")
        self.sea_override = {"sea2_dummy": (sea, "sea2_day")}
        island, island_r = isl.models["WS2_common_island"], isl.models["WS2_common_island_R"]
        sea_m = isl.models["WS2_common_sea"]
        three_unit_sea(sea_m)
        # the island (Wedge Island with it) at level 0, its edge-blend strips detail; a few nodes at
        # every level (FAR_FROM_ISLAND)
        def island_tag(node, mat):
            return TAG_ALL if node in FAR_FROM_ISLAND and mat not in DETAIL_MATERIALS else TAG_NEAR
        n = self.bake_model(isl, island, None, TAG_NEAR, HIDDEN_MODE13, by_centroid=True, group_name="island",
                            tag_of=island_tag)
        nv = self.bake_model(isl, isl.models["WS2_common_island_V"], None, TAG_NEAR, HIDDEN_MODE13,
                             by_centroid=True, group_name="island_V")
        # levels 1-2: island_R, the whole of it (its own nodes: one version each of the mountain and
        # the north coast, Wedge Island's light model)
        nr = self.bake_model(isl, island_r, None, TAG_MID_FAR, R_LEFT_OUT, by_centroid=True)
        # the sea near the shores and inland, into the cells: the open sea stays a model
        x0, z0, nx, nz = self.grid_hint = self.island_grid(island)

        def open_sea(node, mat, tris):
            cen = tris.mean(1)
            ext = tris.max(1) - tris.min(1)
            inside = (cen[:, 0] >= x0) & (cen[:, 0] < x0 + nx * CELL) & (cen[:, 2] >= z0) & (cen[:, 2] < z0 + nz * CELL)
            return ~inside | (np.maximum(ext[:, 0], ext[:, 2]) > SEA_CELL_MAX) if (node, mat) == (
                "Water_Sea", "mt_za_water") else np.zeros(len(tris), bool)

        def sea_tag(node, mat):     # (the cave's water, _off, at level 0: R has no cave)
            return TAG_NEAR if mat in DETAIL_MATERIALS or mat.endswith("_off") else TAG_ALL

        def at_level(node, mat, tris):  # (the flat sea's surface: MAT_SEA, under the wave grid)
            y = tris[:, :, 1] - SEA_LEVEL
            return (np.abs(y).min(1) < SEA_LEVEL_TOL) & (y.max(1) < SEA_SHORE) if mat == SEA_WATER else \
                np.zeros(len(tris), bool)
        ns = self.bake_model(isl, sea_m, None, TAG_ALL, {WEDGE_FAR_NODES[1]}, self.sea_override, by_centroid=True,
                             tag_of=sea_tag, select=lambda node, mat, tris: ~open_sea(node, mat, tris) &
                             ~at_level(node, mat, tris))
        nsl = self.bake_model(isl, sea_m, None, TAG_ALL, {WEDGE_FAR_NODES[1]}, self.sea_override, by_centroid=True,
                              tag_of=sea_tag, select=lambda node, mat, tris: ~open_sea(node, mat, tris) &
                              at_level(node, mat, tris), variant="sea")
        ns += nsl
        self.say("island: %d triangles in the cells (mode 13 with Wedge Island), island_V %d, island_R %d "
                 "(level 2), the sea near the shores and inland %d" % (n, nv, nr, ns))
        self.inv["island"] = dict(source="%s Island/G3D/WS2_common_island.brres" % SA, cell_triangles=n,
                                  island_V_triangles=nv, island_R_triangles=nr, sea_in_cells_triangles=ns,
                                  hidden_nodes=sorted(HIDDEN_MODE13), detail_materials=sorted(DETAIL_MATERIALS),
                                  far_from_island=sorted(FAR_FROM_ISLAND), island_R_left_out=sorted(R_LEFT_OUT))
        # the open sea and the sky: models of their own; the sun a part of the sky. The open sea lies
        # at the sea's level whole: its water is MAT_SEA's
        def open_level(m, d, tris):
            keep = open_sea(m.nodes[d.node].name, mat_name(m, d), tris)
            off = keep & ~at_level(m.nodes[d.node].name, mat_name(m, d), tris)
            if off.any():
                raise SystemExit("the open sea has %d triangles off its level" % int(off.sum()))
            return keep
        self.sea_model, _ = self.cv.model(isl, sea_m, hidden={nd.name for nd in sea_m.nodes if nd.name != "Water_Sea"},
                                          tex_override=self.sea_override, select=open_level, variant="sea")
        self.register_materials(isl, sea_m, self.sea_override)
        # the sea's water as the package has it: the inland waters' (plain), the flat sea's (MAT_SEA),
        # and the wave grid's (stage_sea gives it the grid's shading): each animated by sea_P
        water = next(m for m in sea_m.materials if m.name == SEA_WATER)
        self.sea_waters = {v: self.cv.material(isl, sea_m, water, self.sea_override, variant=v)
                           for v in (None, "sea", "grid")}
        self.pkg.materials[self.sea_waters["sea"]].flags |= ng3d.MAT_SEA
        grid_material(self.pkg.materials[self.sea_waters["grid"]])
        self.say("the sea: %d triangles of the flat sea at its level in the cells (MAT_SEA), the open sea's %d" % (
            nsl, sum(self.pkg.batches[b].n_indices // 3 for b in range(
                self.pkg.models[self.sea_model].first_batch,
                self.pkg.models[self.sea_model].first_batch + self.pkg.models[self.sea_model].n_batches))))
        self.sun = sunr = w.brres(SA, "Sun/G3D/WS2_common_sun_model.brres")
        penv = read_penv(w.get(*ENV_PENV))
        M, info = sun_matrix(penv)
        self.sky_model, nm = self.cv.model(isl, isl.models["WS2_common_vr"], parts=[
            convert.Part("sun", sunr, sunr.models["WS2_common_sun_model"], M)])
        self.register_materials(isl, isl.models["WS2_common_vr"])
        self.register_materials(sunr, sunr.models["WS2_common_sun_model"])
        sky = self.pkg.models[self.sky_model]
        self.say("sky: WS2_common_vr with the sun %s (.penv %s, size %g), %d batches in its draw order: %s" % (
            info, [round(x, 1) for x in penv["pos"]], penv["size"], sky.n_batches, ", ".join(
                self.pkg.string(self.pkg.materials[self.pkg.batches[b].material].name)
                for b in range(sky.first_batch, sky.first_batch + sky.n_batches))))
        self.inv["sky"] = dict(model="WS2_common_vr", sun=dict(source="%s Sun/G3D/WS2_common_sun_model.brres" % SA,
                                                               penv="%s %s" % ENV_PENV, penv_position=penv["pos"],
                                                               penv_size=penv["size"], **info))

    def register_materials(self, r, mdl, override=None):
        """The package materials a model's conversion made, by (model, material) name (for its
        animations)."""
        ov = tuple(sorted((a, id(b[0]), b[1]) for a, b in (override or {}).items()))
        for m in mdl.materials:
            k = (id(r), mdl.name, m.index, ov)
            if k in self.cv.mat:
                self.mat_by_name[(mdl.name, m.name)] = self.cv.mat[k]

    def island_grid(self, island):
        """The cells' grid as the island's triangles span it (Wedge Island included): (x0, z0, nx,
        nz) in cells of CELL from the corner (x0, z0)."""
        shapes = {sh.index: sh for sh in island.shapes}
        lo, hi = np.full(2, np.inf), np.full(2, -np.inf)
        for d in island.draws("DrawOpa") + island.draws("DrawXlu"):
            if island.nodes[d.node].name in HIDDEN_MODE13:
                continue
            t = island.shape_triangles(shapes[d.shape], world=True)
            if not t or "pos" not in t:
                continue
            cen = np.asarray(t["pos"], np.float64).reshape(-1, 3, 3).mean(1)[:, [0, 2]]
            lo, hi = np.minimum(lo, cen.min(0)), np.maximum(hi, cen.max(0))
        c0 = np.floor(lo / CELL).astype(int)
        c1 = np.floor(hi / CELL).astype(int)
        return float(c0[0] * CELL), float(c0[1] * CELL), int(c1[0] - c0[0] + 1), int(c1[1] - c0[1] + 1)

    # ------------------------------------------------------------------------------- props
    def stage_props(self):
        w = self.wsr
        tag0 = read_pmp(w.get("Stage/PlnScene/PlnEnv0.carc", "WS2_pln_island_tag0.pmp"))
        objs, dropped = spawned(tag0, exclude_mask=2)
        self.objs = objs
        by_class = Counter(o["cls"] for o in objs)
        self.say("tag0: %d records, %d group-1 objects spawned (exclude mask 2 drops %s)" % (
            len(tag0), len(objs), dict(dropped)))
        self.say("lighting: %d shadow-area boxes (group 5/type 3), ambient fades by up to 45%% over 5 units" %
                 len(self.shadow_areas))
        counts = OrderedDict()
        handled = Counter()
        for cls in sorted(by_class):
            name, how, path, models = CLASSES[cls]
            counts[cls] = dict(name=name, placed=by_class[cls], handling=how, brres=path, models=list(models))
        for cls, n in sorted(dropped.items()):
            counts.setdefault(cls, dict(name=CLASSES[cls][0], placed=0, handling="out", brres=CLASSES[cls][2],
                                        models=list(CLASSES[cls][3])))
            counts[cls]["dropped_by_exclude_bit"] = n
        # static props and trees: H/M/L tree models in levels 0/1/2 (the palm has one model at 0-1);
        # the boats and gates at levels 0-1, the fountain at 0; the
        # small props at level 0, BATCH_DETAIL
        for o in objs:
            name, how, path, models = CLASSES[o["cls"]]
            if how not in ("static", "small", "tree"):
                continue
            r = w.brres(SA, path)
            cell = self.cell_of(o["pos"][0], o["pos"][2])
            if how == "tree":
                near, mid = models[:2]
                far = models[2] if len(models) > 2 else None
                if mid:
                    self.bake_model(r, r.models[near], o["mtx"], TAG_NEAR, cell=cell)
                    self.bake_model(r, r.models[mid], o["mtx"], TAG_MID, cell=cell)
                    if far:
                        self.bake_model(r, r.models[far], o["mtx"], TAG_FAR, cell=cell)
                else:
                    self.bake_model(r, r.models[near], o["mtx"], TAG_PROP, cell=cell)
            elif how == "static":
                self.bake_model(r, r.models[models[0]], o["mtx"], TAG_NEAR if o["cls"] in NEAR_STATIC else TAG_PROP,
                                cell=cell)
            else:
                self.bake_model(r, r.models[models[0]], o["mtx"], TAG_NEAR, cell=cell, detail=True)
            handled[o["cls"]] += 1
        self.inv["props"] = counts
        self.say("baked: %s" % ", ".join("%s %d" % (CLASSES[c][0], n) for c, n in sorted(handled.items())))

    # ------------------------------------------------------------------------------- moving props
    def stage_instances(self):
        w, pkg = self.wsr, self.pkg
        objs = [o for o in self.objs if CLASSES[o["cls"]][1] == "instance"]
        made = Counter()
        models = {}

        def model_for(cls, extra_root=None):
            if (cls, extra_root) not in models:
                path, mname = CLASSES[cls][2], CLASSES[cls][3][0]
                r = w.brres(SA, path)
                # (a moving boat with WSR's wake under it, as a part: WAKES)
                parts = [convert.Part("wake", self.wake_brres(cls), self.wake_brres(cls).models[WAKES[cls][1]])] \
                    if cls in WAKES else ()
                mi, nm = self.cv.model(r, r.models[mname], name="%s#%d" % (mname, cls) if extra_root or parts else mname,
                                       extra_root=extra_root, split=CAR_WHEELS if mname == "WS2_common_car" else (),
                                       parts=parts)
                models[(cls, extra_root)] = (r, r.models[mname], mi, nm)
            return models[(cls, extra_root)]

        def place(cls, model, anim, mtx, offset, flags=ng3d.INST_ANIMATED):
            # (a boat leaves a trail as it moves: TRAIL_CLASSES)
            flags |= ng3d.INST_TRAIL if cls in TRAIL_CLASSES else 0
            pkg.add("instances", model=model, flags=flags, anim=anim, class_id=cls,
                    mtx=[list(map(float, row)) for row in np.asarray(mtx)[:3]], anim_offset=float(offset),
                    cull_distance=INSTANCE_CULL.get(cls, 0.0))
            made[cls] += 1

        def chr0(r, name):
            return r.anims["CHR0"][name]

        ident = np.eye(4)
        notes = OrderedDict()
        # parked cars are runtime instances so the player can take the nearest one and drive it;
        # their original transforms and class_id are retained, but no route animation is attached.
        parked = [o for o in objs if o["cls"] == 15]
        if parked:
            r, mdl, mi, nm = model_for(15)
            for o in parked:
                place(15, mi, ng3d.NONE16, o["mtx"], 0, flags=ng3d.INST_STATIC)
            notes[15] = "parked; runtime instance, enter with 1 and drive with the circle pad"
        for cls in (7, 8):
            anchored = [o for o in objs if o["cls"] == cls]
            if anchored:
                r, mdl, mi, nm = model_for(cls)
                for o in anchored:
                    place(cls, mi, ng3d.NONE16, o["mtx"], 0, flags=ng3d.INST_STATIC)
                notes[cls] = "anchored boat; runtime instance, enter with 1 and steer with the circle pad"
        # windmills: the blades turn about the hub (CHR0 WS2_common_windmill, 299 frames), on the
        # placed matrix; they turn in step in WSR, staggered here
        ws = [o for o in objs if o["cls"] == 18]
        if ws:
            r, mdl, mi, nm = model_for(18)
            ab = convert.AnimBuilder(pkg, "WS2_common_windmill", chr0(r, "WS2_common_windmill").frames)
            convert.add_chr0(ab, chr0(r, "WS2_common_windmill"), mi, nm, mdl)
            ai = ab.end()
            for k, o in enumerate(ws):
                place(18, mi, ai, o["mtx"], (k * WINDMILL_DESYNC) % 299)
            notes[18] = "CHR0 WS2_common_windmill (the flier node), on the placed matrix"
        # balloons: WS2_common_balloon_flight (7,999 frames, a ~2,000-unit loop) relative to the
        # placed matrix; all three variants use it here (the others are basketball / canoe scenes')
        bs = [o for o in objs if o["cls"] == 16]
        if bs:
            r, mdl, mi, nm = model_for(16)
            a = chr0(r, "WS2_common_balloon_flight")
            ab = convert.AnimBuilder(pkg, a.name, a.frames)
            convert.add_chr0(ab, a, mi, nm, mdl)
            ai = ab.end()
            for k, o in enumerate(bs):
                place(16, mi, ai, o["mtx"], k * a.frames / len(bs))
            notes[16] = "CHR0 WS2_common_balloon_flight relative to the placed matrix, phases spread"
        # the airship: the title sequence's flight (a world path 2,400-2,800 units up over the main
        # island). Its default 'flight' is a 2,500-unit circle at sea level round the origin that
        # the Flyover must place by code not traced here.
        for o in [o for o in objs if o["cls"] == 6]:
            r, mdl, mi, nm = model_for(6)
            a = chr0(r, "WS2_common_airship_flight_title")
            ab = convert.AnimBuilder(pkg, a.name, a.frames)
            convert.add_chr0(ab, a, mi, nm, mdl)
            place(6, mi, ab.end(), ident, 0)
            notes[6] = "CHR0 WS2_common_airship_flight_title (world path)"
        # moving cars: WS2_common_car_drive is one world route along the roads (12,000 frames);
        # each car starts where the route passes nearest its placement
        cs = [o for o in objs if o["cls"] == 26]
        if cs:
            r, mdl, mi, nm = model_for(26)
            a = chr0(r, "WS2_common_car_drive")
            ab = convert.AnimBuilder(pkg, a.name, a.frames)
            convert.add_chr0(ab, a, mi, nm, mdl)
            ai = ab.end()
            path = sample_path(a, "WS2_common_car", 1.0)
            for o in cs:
                place(26, mi, ai, ident, nearest_frame(path, o["pos"]))
            notes[26] = "CHR0 WS2_common_car_drive (world route), each starting nearest its placement"
        # moving cruisers: eight routes (WS2_common_cruiser_revolve1..8, 29 keys each, world
        # coordinates); each cruiser takes the route passing nearest it, at CRUISER_SPEED, with its
        # placed scale
        ms = [o for o in objs if o["cls"] == 33]
        if ms:
            r, mdl, mi, nm = model_for(33)
            routes = {}
            for k in range(1, 9):
                a = chr0(r, "WS2_common_cruiser_revolve%d" % k)
                p1 = sample_path(a, a.targets[0], 1.0)
                length = path_length(p1)
                routes[k] = (a, length, p1)
            per_route = {}
            for o in ms:
                best = min(routes, key=lambda k: np.min(np.linalg.norm(routes[k][2][:, 1:4] - o["pos"], axis=1)))
                per_route.setdefault(best, []).append(o)
            for k, lst in sorted(per_route.items()):
                a, length, p1 = routes[k]
                ts = (length / CRUISER_SPEED) / a.frames          # game frames per anim frame
                ab = convert.AnimBuilder(pkg, "%s@%.0f" % (a.name, ts * a.frames), a.frames * ts)
                sc = lst[0]["scale"][0]
                convert.add_chr0(ab, a, mi, nm, mdl, rename={a.targets[0]: mdl.nodes[0].name}, time_scale=ts,
                                 override={(mdl.nodes[0].name, ng3d.TT_NODE_SCALE_X + i): sc for i in range(3)})
                ai = ab.end()
                for o in lst:
                    place(33, mi, ai, ident, nearest_frame(p1, o["pos"]) * ts)
            notes[33] = "CHR0 WS2_common_cruiser_revolve1..8 (world routes) at %.2f units a frame" % CRUISER_SPEED
        # the liner: WS2_common_move_liner_revolve1 at LINER_RATE
        for o in [o for o in objs if o["cls"] == 40]:
            r, mdl, mi, nm = model_for(40)
            a = chr0(r, "WS2_common_move_liner_revolve1")
            ts = 1.0 / LINER_RATE
            ab = convert.AnimBuilder(pkg, "%s@%.0f" % (a.name, a.frames * ts), a.frames * ts)
            convert.add_chr0(ab, a, mi, nm, mdl, time_scale=ts)
            place(40, mi, ab.end(), ident, 0)
            notes[40] = "CHR0 WS2_common_move_liner_revolve1 (world route) at %g frame a frame" % LINER_RATE
        # whales: surfacing, blowing and diving where they are placed (CHR0 _surface_gush_dive, 1,319
        # frames); WSR also moves them along its revolve routes (not staged)
        wh = [o for o in objs if o["cls"] == 32]
        if wh:
            r, mdl, mi, nm = model_for(32)
            a = chr0(r, "WS2_common_whale_surface_gush_dive")
            ab = convert.AnimBuilder(pkg, a.name, a.frames)
            convert.add_chr0(ab, a, mi, nm, mdl)
            ai = ab.end()
            for k, o in enumerate(wh):
                place(32, mi, ai, o["mtx"], (k * 0.37 % 1.0) * a.frames)
            notes[32] = "CHR0 WS2_common_whale_surface_gush_dive in place"
        # gulls and hawks: a circle of the placed scale's radius (WSR's PMP scale is the radius) at
        # BIRD_SPEED, flapping (fly_a); an 'orbit' node above the model's root carries the circle
        for cls, flap in ((5, "WS2_common_bird_fly_a"), (23, "WS2_common_hawk_fly_a")):
            lst = [o for o in objs if o["cls"] == cls]
            if not lst:
                continue
            r, mdl, mi, nm = model_for(cls, extra_root="orbit")
            a = chr0(r, flap)
            by_radius = {}
            for o in lst:
                by_radius.setdefault(round(o["scale"][0], 1), []).append(o)
            for radius, group in sorted(by_radius.items()):
                laps = max(1, int(round(2 * math.pi * radius / BIRD_SPEED / a.frames)))
                period = laps * a.frames
                ab = convert.AnimBuilder(pkg, "%s@r%.0f" % (flap, radius), period)
                convert.add_chr0(ab, a, mi, nm, mdl, period=a.frames)
                add_orbit(ab, mi, nm["orbit"], radius, period)
                ai = ab.end()
                for k, o in enumerate(group):
                    M = np.eye(4)
                    M[:3, 3] = o["pos"]
                    place(cls, mi, ai, M, (k * 0.618 % 1.0) * period)
            notes[cls] = "%s flapping on a circle of the placed scale's radius at %.1f units a frame" % (
                flap, BIRD_SPEED)
        # The Skydiving parachute's idle/open loop from OmkScene. The island attaches this single
        # animated instance to the Mii during an intentional run off a ledge; its reserved class id
        # keeps it out of the island's parked vehicle and prop handling.
        parachute = w.brres("Common/OmkScene/common.carc", "G3D/WS2_omk_parachute.brres")
        parachute_model = parachute.models["WS2_omk_parachute_01"]
        parachute_index, parachute_nodes = self.cv.model(parachute, parachute_model)
        parachute_anim = parachute.anims["CHR0"]["WS2_omk_parachute_01"]
        parachute_builder = convert.AnimBuilder(pkg, parachute_anim.name, parachute_anim.frames)
        convert.add_chr0(parachute_builder, parachute_anim, parachute_index, parachute_nodes, parachute_model)
        pkg.add("instances", model=parachute_index, flags=ng3d.INST_ANIMATED, anim=parachute_builder.end(),
                class_id=0xFFFE, mtx=np.eye(4)[:3].tolist(), anim_offset=0.0, cull_distance=0.0)
        self.inv["falling"] = dict(source="Common/OmkScene/common.carc:G3D/WS2_omk_parachute.brres",
                                   model="WS2_omk_parachute_01", animation="WS2_omk_parachute_01 (30-frame CHR0)")
        self.stage_wakes()
        self.say("instances: %s" % ", ".join("%s %d" % (CLASSES[c][0], n) for c, n in sorted(made.items())))
        for cls, n in made.items():
            self.inv["props"][cls]["instances"] = n
            self.inv["props"][cls]["animation"] = notes.get(cls)
        # the moving models' boxes over their animations: the cars, boats, liner and airship run
        # world routes on an identity matrix, so their rest boxes sit at the origin; a renderer
        # culling an instance by its matrix and its model's box must see where they go
        grown = convert.widen_model_bounds(pkg)
        self.say("model bounds over their animations: %s" % ", ".join(
            "%s %.0f -> %.0f" % (pkg.string(pkg.models[mi].name), r0, r1) for mi, (r0, r1) in sorted(grown.items())))
        self.inv["model_radius_over_animation"] = {pkg.string(pkg.models[mi].name): [r0, r1]
                                                   for mi, (r0, r1) in sorted(grown.items())}

    # ------------------------------------------------------------------------------- wakes
    def wake_brres(self, cls):
        return self.wsr.brres(SA, WAKES[cls][0])

    def stage_wakes(self):
        """WSR's wakes (WAKES): the moving cruisers' and the liner's, parts of their models (model_for),
        their materials given the day's colours (CLR0 frame 0); the driven boat's, an instance of its
        own (WAKE_CLASS, hidden: the island shows it, placed and faded) with a material of its own
        (MAT_RUNTIME); the boats' trails' material and batch (TRAIL, made from the cruiser's wake: its
        streaks and colours)."""
        pkg = self.pkg
        self.wake_mats = []    # (package material, the wake's brres, its model): stage_anims scrolls them
        for cls, (path, mname) in sorted(WAKES.items()):
            r = self.wake_brres(cls)
            self.register_materials(r, r.models[mname])
            for m in r.models[mname].materials:
                mi = self.mat_by_name.get((mname, m.name))
                if mi is not None:
                    wake_registers(pkg.materials[mi], r.anims["CLR0"][mname], m.name)
                    self.wake_mats.append((mi, r, mname))
        r = self.wake_brres(33)
        ef = r.models["WS2_common_cruiser_ef"]
        mi, _ = self.cv.model(r, ef, name="WS2_common_cruiser_ef#driven", variant="driven")
        for b in range(pkg.models[mi].first_batch, pkg.models[mi].first_batch + pkg.models[mi].n_batches):
            M = pkg.materials[pkg.batches[b].material]
            M.flags |= ng3d.MAT_RUNTIME
            wake_registers(M, r.anims["CLR0"]["WS2_common_cruiser_ef"], "m_nami_ef")
            self.wake_mats.append((pkg.batches[b].material, r, "WS2_common_cruiser_ef"))
        pkg.add("instances", model=mi, flags=0, anim=ng3d.NONE16, class_id=WAKE_CLASS, mtx=np.eye(4)[:3].tolist(),
                anim_offset=0.0, cull_distance=0.0)
        # the trails' material: the wake's, its alpha the ribbons' (trail_material); its batch (stage_sea)
        self.trail_material = self.cv.material(r, ef, ef.materials[0], variant="trail")
        M = pkg.materials[self.trail_material]
        trail_material(M)
        wake_registers(M, r.anims["CLR0"]["WS2_common_cruiser_ef"], "m_nami_ef")
        self.inv["wakes"] = dict(parts={CLASSES[c][0]: "%s %s" % (SA, p) for c, (p, _) in WAKES.items()},
                                 driven="WS2_common_cruiser_ef#driven (class 0x%04X, MAT_RUNTIME)" % WAKE_CLASS,
                                 trails=dict(TRAIL, classes=[CLASSES[c][0] for c in TRAIL_CLASSES]))
        self.say("wakes: WSR's under the moving cruisers and the liner (parts), the driven boat's (an instance of "
                 "its own, faded at run time); trails behind %s" % ", ".join(CLASSES[c][0] for c in TRAIL_CLASSES))

    # ------------------------------------------------------------------------------- the cells
    def build_cells(self):
        pkg = self.pkg
        # the slivers a centroid cut leaves where a material crosses a cell's edge go to the
        # neighbour that draws the most of it (a draw fewer each)
        self.cells.absorb_small(SLIVER, lambda g: [(g[0] + dx, g[1] + dz) for dx in (-1, 0, 1) for dz in (-1, 0, 1)
                                                   if dx or dz], SLIVER_MARGIN)
        built = self.cells.build()
        keys = list(built)
        xs = [k[0] for k in keys]
        zs = [k[1] for k in keys]
        x0, x1, z0, z1 = min(xs), max(xs), min(zs), max(zs)
        nx, nz = x1 - x0 + 1, z1 - z0 + 1
        world_lo, world_hi = np.full(3, np.inf), np.full(3, -np.inf)
        stats = Counter()
        for iz in range(z0, z1 + 1):
            for ix in range(x0, x1 + 1):
                lst = built.get((ix, iz), [])
                lo, hi = np.full(3, np.inf), np.full(3, -np.inf)
                for b, _, _ in lst:
                    bb = pkg.batches[b].bbox
                    lo, hi = np.minimum(lo, bb[:3]), np.maximum(hi, bb[3:])
                if not lst:
                    lo = np.array([ix * CELL, 0.0, iz * CELL])
                    hi = np.array([(ix + 1) * CELL, 0.0, (iz + 1) * CELL])
                else:
                    world_lo, world_hi = np.minimum(world_lo, lo), np.maximum(world_hi, hi)

                def level(l):
                    sel = [(b, m) for b, m, lv in lst if l in lv]
                    sel.sort(key=lambda bm: (pkg.materials[bm[1]].flags & ng3d.MAT_XLU,
                                             pkg.materials[bm[1]].priority, bm[0]))
                    return [b for b, _ in sel]
                levels = [level(l) for l in range(3)]
                n_lods = 3 if any(lv != (0, 1, 2) for _, _, lv in lst) else 1
                first, count = [0, 0, 0], [0, 0, 0]
                for l in range(n_lods):
                    first[l] = len(pkg.cell_batches)
                    count[l] = len(levels[l])
                    pkg.cell_batches.extend(levels[l])
                    stats["batches level %d" % l] += len(levels[l])
                grp = self.cell_groups.get((ix, iz))
                group = grp.most_common(1)[0][0] if grp else ""
                pkg.add("cells", bbox=[float(v) for v in list(lo) + list(hi)], first=first, count=count,
                        n_lods=n_lods, lod_distance=LOD_DISTANCES[:n_lods] + [0.0] * (3 - n_lods),
                        group=pkg.str(group))
                stats["cells"] += 1
                stats["cells with geometry"] += bool(lst)
        self.grid = (x0 * CELL, z0 * CELL, nx, nz)
        self.world_bbox = [float(v) for v in list(world_lo) + list(world_hi)]
        self.say("cells: %d x %d of %.0f units from (%.0f, %.0f), %d with geometry; batches by level %s" % (
            nx, nz, CELL, x0 * CELL, z0 * CELL, stats["cells with geometry"],
            [stats["batches level %d" % l] for l in range(3)]))
        self.inv["cells"] = dict(cell=CELL, grid=[nx, nz], origin=[x0 * CELL, z0 * CELL],
                                 with_geometry=stats["cells with geometry"], lod_distances=LOD_DISTANCES,
                                 batches_per_level=[stats["batches level %d" % l] for l in range(3)])

    # ------------------------------------------------------------------------------- animations
    def stage_anims(self):
        pkg, isl = self.pkg, self.island
        fountain = self.wsr.brres(SA, CLASSES[21][2])
        out = OrderedDict()
        # world: lava (island SRT0, 2,400 frames) and the fountain's water (300, repeated 8 times);
        # colours: lava and the volcano's inside (island CLR0, 200) and island_V's blend (200)
        ab = convert.AnimBuilder(pkg, "world_srt", 2400)
        n = convert.add_srt0(ab, isl.anims["SRT0"]["WS2_common_island"], self.texmtx_of("WS2_common_island"))
        n += convert.add_srt0(ab, fountain.anims["SRT0"]["WS2_common_fountain"], self.texmtx_of("WS2_common_fountain"),
                              period=300)
        self.world_srt = ab.end()
        out["world_srt"] = dict(frames=2400, tracks=n, sources=["SRT0 WS2_common_island", "SRT0 WS2_common_fountain"])
        ab = convert.AnimBuilder(pkg, "world_clr", 200)
        n = convert.add_clr0(ab, isl.anims["CLR0"]["WS2_common_island"], self.material_of("WS2_common_island"))
        n += convert.add_clr0(ab, isl.anims["CLR0"]["WS2_common_island_V"], self.material_of("WS2_common_island_V"))
        self.world_clr = ab.end()
        out["world_clr"] = dict(frames=200, tracks=n, sources=["CLR0 WS2_common_island", "CLR0 WS2_common_island_V"])
        # the sea: the Flyover's set (main.dol 0x806BE698: sea_P with sea_nami); 20,000 frames. sea_P's
        # mt_za_water tracks go to each of its materials (the inland waters', the flat sea's, the wave
        # grid's), its matrices 2 and 3 as the package's 1 and 2 (three_unit_sea; 1, jog_waterA's, has
        # nothing left to move). And the wakes' foam scrolling back (their SRT0s: the cruiser's 120 frames
        # 167 times, 0.2 % slower to fit; the liner's 400, 50 times)
        ab = convert.AnimBuilder(pkg, "sea_srt", 20000)
        sea_p = isl.anims["SRT0"]["WS2_common_sea_P"]
        n = convert.add_srt0(ab, sea_p, self.texmtx_of("WS2_common_sea"), skip={(SEA_WATER, j) for j in range(8)})

        def matrix_in(mi, mname, remap=None):
            m = pkg.materials[mi]

            def f(name, j):
                k = j if remap is None else remap.get(j)
                return m.tex_mtx_first + k if name == mname and k is not None and k < m.n_tex_mtx else None
            return f
        for mi in self.sea_waters.values():
            n += convert.add_srt0(ab, sea_p, matrix_in(mi, SEA_WATER, SEA_WATER_MATRIX))
        n += convert.add_srt0(ab, isl.anims["SRT0"]["WS2_common_sea_nami"], self.texmtx_of("WS2_common_sea"),
                              period=1000)
        wakes = 0
        for mi, r, mname in self.wake_mats:
            a = r.anims["SRT0"][mname]
            laps = max(1, round(20000 / a.frames))
            for mat in r.models[mname].materials:
                wakes += convert.add_srt0(ab, a, matrix_in(mi, mat.name), period=20000 / laps,
                                          time_scale=20000 / (laps * a.frames))
        n += wakes
        self.sea_srt = ab.end()
        out["sea_srt"] = dict(frames=20000, tracks=n, sources=["SRT0 WS2_common_sea_P", "SRT0 WS2_common_sea_nami",
                                                               "SRT0 WS2_common_cruiser_ef, _move_liner_ef"],
                              water_materials=len(self.sea_waters), wake_tracks=wakes)
        ab = convert.AnimBuilder(pkg, "sea_clr", 1000)
        n = convert.add_clr0(ab, isl.anims["CLR0"]["WS2_common_sea_nami"], self.material_of("WS2_common_sea"))
        self.sea_clr = ab.end()
        out["sea_clr"] = dict(frames=1000, tracks=n, sources=["CLR0 WS2_common_sea_nami"])
        # the sky: the clouds' scroll (9,999 frames) and the sun's rays turning (4,000): four of the
        # one, ten of the other, stretched by 0.01 % to fit
        a = isl.anims["SRT0"]["WS2_common_vr"]
        sun = self.sun.anims["SRT0"]["WS2_common_sun_model"]
        frames = 4 * a.frames
        laps = round(frames / sun.frames)
        ab = convert.AnimBuilder(pkg, "sky_srt", frames)
        n = convert.add_srt0(ab, a, self.texmtx_of("WS2_common_vr"), period=a.frames)
        n += convert.add_srt0(ab, sun, self.texmtx_of("WS2_common_sun_model"), period=frames / laps,
                              time_scale=frames / (laps * sun.frames))
        self.sky_srt = ab.end()
        out["sky_srt"] = dict(frames=frames, tracks=n, sources=["SRT0 WS2_common_vr (x4)",
                                                                "SRT0 WS2_common_sun_model (x%d)" % laps])
        self.inv["anims"] = out
        self.say("animations: %d, %d tracks, %d keys" % (len(pkg.anims), len(pkg.tracks), len(pkg.keys)))

    # ------------------------------------------------------------------------------- environment
    def stage_env(self):
        w, pkg = self.wsr, self.pkg
        # the lights and light sets (env_lights: the ones the prelit materials were baked with)
        for l in self.lights:
            pkg.add("lights", name=pkg.str(l["name"]), kind=l["kind"], rgba=l["rgba"], position=l["position"],
                    direction=l["direction"])
        env_lights = [l["info"] for l in self.lights]
        sets = []
        for ls in self.light_sets:
            while len(pkg.light_sets) < ls["id"]:
                pkg.add("light_sets", lights=[NONE16] * 8)
            idx, amb = ls["lights"], ls["ambient"]
            pkg.add("light_sets", name=pkg.str(ls["name"]), lights=(idx + [NONE16] * 8)[:8], ambient=amb)
            sets.append(dict(id=ls["id"], name=ls["name"], lights=[pkg.string(pkg.lights[i].name) for i in idx],
                             ambient="#%02x%02x%02x" % tuple(amb[:3])))
        # the fogs: Bic F0's table (fog 0 off), the island's .pfobj in fog 1, switched on
        table = w.get(*ENV_FOGS)
        fogs = [read_fogd(table, 0x14 + 0x30 * i) for i in range(SEA_FOG_INDEX + 1)]
        fogs[SEA_FOG_INDEX] = dict(read_fogd(w.get(*SEA_FOG)), on=True)
        for f in fogs:
            pkg.add("fogs", type=ng3d.FOG_LINEAR if f["on"] and f["type"] == 2 else ng3d.FOG_NONE, rgba=f["color"],
                    start=f["start"], end=f["end"])
        # materials asking for a fog the table doesn't have get none; the sky (the dome and the sun)
        # none whatever its index says: WSR's sky is fog 0, off by day, and a fog 0 turned on (a
        # night's) must not wash it out at 62,000-70,000 units
        users = {}
        sky = pkg.models[self.sky_model]
        sky_mats = {pkg.batches[b].material for b in range(sky.first_batch, sky.first_batch + sky.n_batches)}
        for i, m in enumerate(pkg.materials):
            if m.flags & ng3d.MAT_FOG and (m.fog >= len(pkg.fogs) or i in sky_mats):
                m.flags &= ~ng3d.MAT_FOG
                self.cv.stats["materials unfogged (the sky, or a fog index past the table)"] += 1
            if m.flags & ng3d.MAT_FOG:
                users.setdefault(m.fog, []).append(pkg.string(m.name))
        fog_users = {i: (len(u) if i != SEA_FOG_INDEX else u) for i, u in sorted(users.items())}
        # ... and then no fog at all (MAT_FOG cleared; the table stays, and the scene's clear colour
        # is still fog 1's): fog 0 is off, and fog 1 starts 27,978 units out, where from the ground
        # the open sea is a few lines at the horizon (research env.md); the PICA backend draws a
        # fogged item a second time for its fog
        unfogged = Counter()
        if not KEEP_FOG:
            for m in pkg.materials:
                if m.flags & ng3d.MAT_FOG:
                    m.flags &= ~ng3d.MAT_FOG
                    unfogged["fog %d (%s)" % (m.fog, "off" if pkg.fogs[m.fog].type == ng3d.FOG_NONE else "on")] += 1
            self.cv.stats["materials unfogged (v2: no fog pass)"] = sum(unfogged.values())
        # k (MII_UNITS_PER_ISLAND_UNIT), and the WSR Mii it rests on, measured: the player body's
        # head_set over its feet, and the RFL head on it
        body = w.brres("Common/Kokeshi/common.carc", "G3D/WS2_avaA1.brres").models["WS2_avaA1"]
        head_set = float([n for n in body.nodes if n.name == "head_set"][0].mtx[1, 3])
        wsr_mii = head_set + WSR_HEAD_SCALE * RFL_HAIR_TOP
        noma_mii = NOMA_HEAD_Y + NOMA_HEAD_SCALE * RFL_HAIR_TOP
        k = self.k = MII_UNITS_PER_ISLAND_UNIT
        pkg.env = ng3d.Rec("Env", sky_model=self.sky_model, sky_anim=self.sky_srt, sea_model=self.sea_model,
                           sea_anim_srt=self.sea_srt, sea_anim_clr=self.sea_clr, world_anim_srt=self.world_srt,
                           world_anim_clr=self.world_clr, world_bbox=self.world_bbox, mii_units_per_unit=k,
                           cell_size=CELL, n_cells_x=self.grid[2], n_cells_z=self.grid[3],
                           grid_origin=[self.grid[0], self.grid[1]], detail_distance=DETAIL_DISTANCE)
        self.say("env: %d lights (%s), %d light sets, fogs %s, fogged materials by fog %s; k = %.1f (WSR's Mii: "
                 "head_set %.2f + %.2f x %.1f = %.2f island units; Wii no Ma's %.1f units: %.2f; a %.0f-tall Mii "
                 "is %.1f island units)" % (
                     len(pkg.lights), ", ".join("%s %s" % (l["name"], l["kind"]) for l in env_lights),
                     len(pkg.light_sets), [(f["start"], f["end"], f["on"]) for f in fogs], fog_users, k, head_set,
                     WSR_HEAD_SCALE, RFL_HAIR_TOP, wsr_mii, noma_mii, noma_mii / wsr_mii, NOMA_MII_HEIGHT,
                     NOMA_MII_HEIGHT / k))
        self.inv["env"] = dict(source=dict(lights="%s %s" % ENV_LIGHTS, fogs="%s %s" % ENV_FOGS,
                                           sea_fog="%s %s" % SEA_FOG),
                               lights=env_lights, light_sets=sets, fogs=fogs, fogged_materials_in_wsr=fog_users,
                               unfogged_materials=dict(unfogged),
                               clear_colour="#%02x%02x%02x" % tuple(fogs[SEA_FOG_INDEX]["color"][:3]),
                               k=dict(value=k, wsr_head_set_y=head_set, wsr_mii_height=wsr_mii,
                                      noma_mii_height=noma_mii, ratio=noma_mii / wsr_mii,
                                      mii_height_island_units=NOMA_MII_HEIGHT / k),
                               world_bbox=self.world_bbox)

    # ------------------------------------------------------------------------------- markers
    def stage_markers(self, kcl):
        pkg, w = self.pkg, self.wsr
        tris, attrs, normals = kcl
        g = kcl_ground(tris, attrs, normals, SPAWN[0], SPAWN[2], SPAWN[1] + 60)
        # flat ground around it: the ground on a 9 x 9 grid 200 units across
        ys = []
        for dx in np.linspace(-100, 100, 9):
            for dz in np.linspace(-100, 100, 9):
                h = kcl_ground(tris, attrs, normals, SPAWN[0] + dx, SPAWN[2] + dz, SPAWN[1] + 60)
                ys.append((h[0], h[1], float(h[2][1])) if h else (None, None, None))
        hs = [y for y, _, _ in ys if y is not None]
        spawn_check = dict(ground_y=g[0], attribute="%04x" % g[1], normal_y=float(g[2][1]),
                           grid_200=dict(points=len(ys), on_ground=len(hs), y_min=min(hs), y_max=max(hs),
                                         attributes=sorted({"%04x" % a for _, a, _ in ys if a is not None}),
                                         min_normal_y=min(n for _, _, n in ys if n is not None)))
        self.say("spawn %s: ground %.2f attr %s normal.y %.4f; 200x200 around: y %.2f..%.2f, attrs %s" % (
            SPAWN, g[0], spawn_check["attribute"], g[2][1], min(hs), max(hs), spawn_check["grid_200"]["attributes"]))
        pkg.add("markers", name=pkg.str("spawn"), kind=0, position=[SPAWN[0], g[0], SPAWN[2]], dir=list(SPAWN_DIR))
        lms = read_landmarks(w.get("Stage/Shared/Activity.carc", "LandMarkData/LandMarkData.exbin"))
        used = [l for l in lms if l["kind"] != 6]
        fixed = [l for l in used if l["kind"] == 0]
        for l in fixed:
            pkg.add("markers", name=pkg.str(landmark_name(l) or "landmark_%d" % l["index"]), kind=1,
                    position=list(map(float, l["pos"])))
        for i, (eye, tgt) in enumerate(read_campar(w.get("Stage/Shared/SeqData.carc",
                                                         "Map/WS2_seq1_IslandCamParam.exbin"))):
            pkg.add("markers", name=pkg.str("seqcam_%d" % i), kind=2, position=list(map(float, eye)),
                    dir=list(map(float, tgt)))
        for name, (eye, tgt, fov) in VIEWS.items():
            pkg.add("markers", name=pkg.str("view_" + name), kind=2, position=list(eye), dir=list(tgt))
        # the exploration menu's places, each checked on the collision (the spawn too, for the record)
        body = warp_body(self.k)
        names = {l["index"]: landmark_name(l) for l in lms}
        spawn_room = warp_check(kcl, self.cylinders, (SPAWN[0], g[0], SPAWN[2]), body)
        warps, bad = [], []
        for lm, own, pos, yaw in WARPS:
            name = own if lm is None else names[lm] + own[1:] if own else names[lm]
            r = warp_check(kcl, self.cylinders, pos, body)
            if not r["ok"]:
                bad.append("%s %s: %s" % (name, pos, r["why"]))
            d = [math.sin(math.radians(yaw)), 0.0, math.cos(math.radians(yaw))]
            feet = [float(pos[0]), r["ground"] if r["ok"] else float(pos[1]), float(pos[2])]
            pkg.add("markers", name=pkg.str(name), kind=ng3d.MK_WARP, position=feet, dir=d)
            warps.append(dict(name=name, landmark=lm, feet=feet, yaw=yaw, **{k: v for k, v in r.items() if k != "ok"}))
        if bad:
            raise SystemExit("warp spots without room for the Mii:\n  " + "\n  ".join(bad))
        self.say("markers: spawn, %d landmarks (of %d used: %d ride vehicles, positions relative to them), "
                 "15 seqcam, %d view, %d warps (each on walkable ground, nothing solid within %.2f of the body's "
                 "spheres at %.2f and %.2f, %.0f clear above; the spawn: %s)" % (
                     len(fixed), len(used), len(used) - len(fixed), len(VIEWS), len(warps), body["radius"],
                     body["knee"], body["chest"], WARP_HEADROOM, spawn_room["why"]))
        self.inv["markers"] = dict(spawn=dict(position=[SPAWN[0], g[0], SPAWN[2]], dir=SPAWN_DIR, check=spawn_check,
                                              room=spawn_room),
                                   landmarks=len(fixed),
                                   landmarks_on_vehicles=[l["name"] for l in used if l["kind"] == 3],
                                   seqcams=15, views=list(VIEWS), warp_body=body, warps=warps)

    # ------------------------------------------------------------------------------- visibility
    def stage_cvis(self, kcl, tool, workdir):
        """The cells' visibility rows (CVIS) by tools/ng3d_pvs.cpp, from eyes over each cell's floors
        (PVS_*): the input written to workdir, the tool run, the rows into the package."""
        pkg, e = self.pkg, self.pkg.env
        n = len(pkg.cells)
        ncx, ncz, x0, z0 = e.n_cells_x, e.n_cells_z, e.grid_origin[0], e.grid_origin[1]
        sets = []
        for ci, c in enumerate(pkg.cells):
            masks = {}
            for l in range(c.n_lods):
                for k in range(c.first[l], c.first[l] + c.count[l]):
                    masks[pkg.cell_batches[k]] = masks.get(pkg.cell_batches[k], 0) | 1 << l
            if c.n_lods == 1:     # (one list for every level)
                masks = {b: 7 for b in masks}
            for b, mask in sorted(masks.items()):
                m = pkg.materials[pkg.batches[b].material]
                T = batch_triangles(pkg, b)
                sets.append((ci, 1, mask, m.cull, T))
                # (a detail batch hides nothing: beyond Env::detail_distance from it it isn't drawn)
                if occludes(m) and not pkg.batches[b].flags & ng3d.BATCH_DETAIL:
                    sets.append((ci, 0, mask, m.cull, T))
        # the open sea (the sea model, as it stands): an occluder drawn whatever the cells' levels
        sea = pkg.models[e.sea_model]
        for b in range(sea.first_batch, sea.first_batch + sea.n_batches):
            bt = pkg.batches[b]
            m = pkg.materials[bt.material]
            if occludes(m):
                M = np.eye(4)
                M[:3] = np.array(pkg.nodes[sea.first_node + bt.node].bind)
                sets.append((0xFFFFFFFF, 0, 7, m.cull, batch_triangles(pkg, b, M)))
        # the eyes: over every floor of a column grid, up to PVS_MAX_HEIGHT
        tris, attrs, normals = kcl
        lo, hi = tris.min(1), tris.max(1)
        floor_ok = (normals[:, 1] > 0.05) & ~np.isin(attrs >> 8, (0x00, 0x08))
        eyes = []
        for iz in range(ncz):
            for ix in range(ncx):
                cx0, cz0 = x0 + ix * CELL, z0 + iz * CELL
                near = (lo[:, 0] <= cx0 + CELL) & (hi[:, 0] >= cx0) & (lo[:, 2] <= cz0 + CELL) & (hi[:, 2] >= cz0) & \
                    ~np.isin(attrs >> 8, (0x00, 0x08))
                T, A, N = tris[near], attrs[near], normals[near]
                if not len(T):
                    continue
                ok = floor_ok[near]
                steps = int(round(CELL / PVS_SPACING))
                for a in range(steps):
                    for b in range(steps):
                        x, z = cx0 + (a + 0.5) * PVS_SPACING, cz0 + (b + 0.5) * PVS_SPACING
                        ys, _, nys = kcl_surfaces(T, A, N, x, z)
                        ys_f, _, _ = kcl_surfaces(T[ok], A[ok], N[ok], x, z)
                        for fy in ys_f:
                            above = ys > fy + 0.01
                            # (open space over the floor: nothing above it, or the first surface above
                            # faces down; under a surface facing up the floor is inside something
                            # solid -- the sea plane under the island)
                            if above.any() and nys[above][np.argmin(ys[above])] >= 0:
                                continue
                            top = min(fy + PVS_MAX_HEIGHT, float(ys[above].min()) - 2.5 if above.any() else np.inf)
                            for h in PVS_HEIGHTS:
                                y = min(fy + max(2.5, h * PVS_MAX_HEIGHT), top)
                                if y > fy:
                                    eyes.append((iz * ncx + ix, x, y, z))
        # where a cell's eye may be (what its detail levels are picked from): x/z its square of the
        # grid, the columns' gaps to its edges included; y the eyes' range
        ey = {}
        for c, _, y, _ in eyes:
            lo_y, hi_y = ey.get(c, (y, y))
            ey[c] = (min(lo_y, y), max(hi_y, y))
        out = [b"PVSI", struct.pack("<4I", 2, n, PVS_FACE, 0)]
        for ci, c in enumerate(pkg.cells):
            cx0, cz0 = x0 + (ci % ncx) * CELL, z0 + (ci // ncx) * CELL
            lo_y, hi_y = ey.get(ci, (0.0, 0.0))
            out.append(struct.pack("<6f3fI6f", *c.bbox, *c.lod_distance, c.n_lods, cx0, lo_y, cz0, cx0 + CELL, hi_y,
                                   cz0 + CELL))
        out.append(struct.pack("<I", len(sets)))
        for ci, kind, mask, cull, T in sets:
            out.append(struct.pack("<5I", ci, kind, mask, cull, len(T)))
            out.append(np.ascontiguousarray(T, "<f4").tobytes())
        out.append(struct.pack("<I", len(eyes)))
        out.append(b"".join(struct.pack("<I3f", c, x, y, z) for c, x, y, z in eyes))
        os.makedirs(workdir, exist_ok=True)
        src, dst = os.path.join(workdir, "cvis.pvsi"), os.path.join(workdir, "cvis.bin")
        with open(src, "wb") as f:
            f.write(b"".join(out))
        t0 = time.time()
        r = subprocess.run([tool, src, dst], check=True, capture_output=True, text=True)
        rows = open(dst, "rb").read()
        rb = (n + 7) // 8
        if len(rows) != rb * n:
            raise SystemExit("ng3d-pvs: %d bytes of rows for %d cells" % (len(rows), n))
        pkg.cvis = rows
        e.pvs_row_bytes, e.pvs_max_height = rb, PVS_MAX_HEIGHT
        # what the rows hide: per cell with eyes and geometry, the share of the cells with geometry
        has_geo = [any(c.count[:c.n_lods]) for c in pkg.cells]
        has_eyes = Counter(c for c, *_ in eyes)
        shares = []
        for i in range(n):
            if not has_eyes[i] or not has_geo[i]:
                continue
            row = rows[i * rb:(i + 1) * rb]
            shares.append(sum(1 for j in range(n) if has_geo[j] and row[j >> 3] >> (j & 7) & 1) / sum(has_geo))
        self.say("cell visibility (CVIS): %d eyes (a column every %.0f units, every floor, heights %s of %.0f), "
                 "%.1f s; from a cell with ground and geometry %.1f%% of the cells with geometry may show on "
                 "average (%.1f%%-%.1f%%); %s" % (
                     len(eyes), PVS_SPACING, PVS_HEIGHTS, PVS_MAX_HEIGHT, time.time() - t0,
                     100 * float(np.mean(shares)), 100 * min(shares), 100 * max(shares), r.stdout.strip()))
        self.inv["cvis"] = dict(eyes=len(eyes), spacing=PVS_SPACING, heights=list(PVS_HEIGHTS),
                                max_height=PVS_MAX_HEIGHT, face=PVS_FACE, row_bytes=rb,
                                visible_share_mean=float(np.mean(shares)), visible_share_min=min(shares),
                                tool=r.stdout.strip())

    # ------------------------------------------------------------------------------- collision
    # ------------------------------------------------------------------------------- the sea
    def stage_sea(self, kcl):
        """The sea that moves (SEAW; SEA_* above, engine/g3d/sea.h): the waves, the amplitude map from
        the collision, the wave grid's batch (its vertices about its centre, the grid material) and the
        trails' (TRAIL: a placeholder buffer of the ribbons' layout, their fixed indices), the sea's
        texture coordinates as a function of the place (fitted to the open sea's), the visible sun."""
        pkg = self.pkg
        isl = self.island
        tris, attrs, normals = kcl
        # ---- the amplitude map: the distance from land (not water, not a fence, reaching above y -3)
        # on a raster of `raster` units over the land's box (and `margin` round it), as factors 0-1 a
        # texel of `texel` units (the raster's averaged)
        A = SEA_AMP
        land = (attrs != 0x1106) & ~np.isin(attrs >> 8, (0x00, 0x08)) & (tris[:, :, 1].max(1) > SEA_LEVEL - 3.0)
        lt = tris[land]
        lo = lt[:, :, [0, 2]].reshape(-1, 2).min(0) - A["margin"]
        hi = lt[:, :, [0, 2]].reshape(-1, 2).max(0) + A["margin"]
        step = int(round(A["texel"] / A["raster"]))
        cells = np.ceil((hi - lo) / A["texel"]).astype(int)
        hi = lo + cells * A["texel"]
        nx, nz = cells * step
        mask = np.zeros((nz, nx), bool)
        res = A["raster"]
        # each land triangle's raster cells: the cells its box covers whose centres it covers, and the
        # cells of its corners and middle (a sliver narrower than a cell)
        tx, tz = lt[:, :, 0], lt[:, :, 2]
        i0 = np.clip(np.floor((tx.min(1) - lo[0]) / res).astype(int), 0, nx - 1)
        i1 = np.clip(np.floor((tx.max(1) - lo[0]) / res).astype(int), 0, nx - 1)
        j0 = np.clip(np.floor((tz.min(1) - lo[1]) / res).astype(int), 0, nz - 1)
        j1 = np.clip(np.floor((tz.max(1) - lo[1]) / res).astype(int), 0, nz - 1)
        for pts in (lt[:, 0], lt[:, 1], lt[:, 2], lt.mean(1)):
            ii = np.clip(np.floor((pts[:, 0] - lo[0]) / res).astype(int), 0, nx - 1)
            jj = np.clip(np.floor((pts[:, 2] - lo[1]) / res).astype(int), 0, nz - 1)
            mask[jj, ii] = True
        for t in np.nonzero((i1 > i0) | (j1 > j0))[0]:
            ci, cj = np.meshgrid(np.arange(i0[t], i1[t] + 1), np.arange(j0[t], j1[t] + 1))
            px, pz = lo[0] + (ci + 0.5) * res, lo[1] + (cj + 0.5) * res
            a, b, c = lt[t, 0], lt[t, 1], lt[t, 2]

            def edge(p, q):
                return (q[0] - p[0]) * (pz - p[2]) - (q[2] - p[2]) * (px - p[0])
            e0, e1, e2 = edge(a, b), edge(b, c), edge(c, a)
            inside = ((e0 >= 0) & (e1 >= 0) & (e2 >= 0)) | ((e0 <= 0) & (e1 <= 0) & (e2 <= 0))
            mask[cj[inside], ci[inside]] = True
        # the distance to the nearest land cell (its middle), within `whole`
        reach = int(np.ceil(A["whole"] / res)) + 1
        dist = np.full(mask.shape, np.inf)
        dist[mask] = 0.0
        padded = np.pad(mask, reach)
        for dj in range(-reach, reach + 1):
            for di in range(-reach, reach + 1):
                d = math.hypot(di, dj) * res
                if 0 < d <= A["whole"] + res:
                    near = padded[reach + dj:reach + dj + nz, reach + di:reach + di + nx]
                    dist[near] = np.minimum(dist[near], d)
        t = np.clip((dist - A["calm"]) / (A["whole"] - A["calm"]), 0.0, 1.0)
        amp = t * t * (3 - 2 * t)
        amp = amp.reshape(cells[1], step, cells[0], step).mean((1, 3))
        amp_bytes = np.rint(amp * 255).astype(np.uint8)
        # ---- the grid: log-polar, `segments` around, rings from `inner` to `radius` (a ring's step
        # its spacing along it), and a vertex in the middle
        G = SEA_GRID
        S = G["segments"]
        q = math.exp(2 * math.pi / S)
        rings = int(math.ceil(math.log(G["radius"] / G["inner"]) / math.log(q))) + 1
        radii = G["inner"] * np.power(G["radius"] / G["inner"], np.arange(rings) / (rings - 1))
        th = 2 * math.pi * np.arange(S) / S
        P = [[0.0, 0.0, 0.0]]
        for r in radii:
            P += [[r * math.cos(a), 0.0, r * math.sin(a)] for a in th]
        P = np.array(P, np.float32)
        # (its triangles facing up as GX's front faces are wound: mt_za_water culls the back ones)
        idx = []
        for k in range(S):
            idx += [0, 1 + k, 1 + (k + 1) % S]
        for i in range(rings - 1):
            for k in range(S):
                a, b = 1 + i * S + k, 1 + i * S + (k + 1) % S
                c, d = a + S, b + S
                idx += [a, c, b, c, d, b]
        grid_batch = self.raw_batch(self.sea_waters["grid"], P.tobytes(), len(P), ng3d.VA_POS, 12, 0, idx, P)
        # ---- the trails: ribbons of pairs of vertices (position f32, colour, coordinates f32: the
        # engine's world.cpp TRAIL_STRIDE) whose segments are two triangles
        T = TRAIL
        nb, ns = T["boats"], T["samples"]
        tidx = []
        for k in range(nb):
            for j in range(ns - 1):
                a = 2 * (k * ns + j)
                tidx += [a, a + 1, a + 2, a + 2, a + 1, a + 3]
        nv = 2 * nb * ns
        trail_batch = self.raw_batch(self.trail_material, bytes(24 * nv), nv, ng3d.VA_POS | ng3d.VA_CLR | ng3d.VA_UV0,
                                     24, ng3d.VB_UV_FLOAT, tidx, np.zeros((1, 3)), ng3d.BATCH_DYNAMIC)
        # ---- the sea's texture coordinates at a place: fitted to the open sea's at its level
        sea_m = isl.models["WS2_common_sea"]
        shapes = {sh.index: sh for sh in sea_m.shapes}
        mats = {m.index: m for m in sea_m.materials}
        Ps, Us = [], []
        for d in sea_m.draws("DrawOpa"):
            if sea_m.nodes[d.node].name != "Water_Sea" or mats[d.mat].name != SEA_WATER:
                continue
            t = sea_m.shape_triangles(shapes[d.shape], world=True)
            p3 = np.asarray(t["pos"], np.float64).reshape(-1, 3, 3)
            u3 = np.asarray(t["uv0"], np.float64).reshape(-1, 3, 2)
            keep = np.all(np.abs(p3[:, :, 1] - SEA_LEVEL) < SEA_LEVEL_TOL, axis=1)  # (flat ones only)
            Ps.append(p3[keep].reshape(-1, 3)), Us.append(u3[keep].reshape(-1, 2))
        Ps, Us = np.concatenate(Ps), np.concatenate(Us)
        X = np.c_[Ps[:, 0], Ps[:, 2], np.ones(len(Ps))]
        uv_map = [np.linalg.lstsq(X, Us[:, i], rcond=None)[0] for i in range(2)]
        uv_err = max(float(np.abs(X @ uv_map[i] - Us[:, i]).max()) for i in range(2))
        # ---- the record
        sun = np.asarray(read_penv(self.wsr.get(*ENV_PENV))["pos"], np.float64)
        sun /= np.linalg.norm(sun)
        waves = []
        for deg, length, ampl, phase in SEA_WAVES:
            a = math.radians(deg)
            waves.append(ng3d.Rec("SeaWave", dir=[math.sin(a), math.cos(a)], wavelength=length, amplitude=ampl,
                                  speed=math.sqrt(SEA_G * length / (2 * math.pi)), phase=phase,
                                  fade=[min(SEA_FADE[0] * length, 0.9 * G["radius"]),
                                        min(SEA_FADE[1] * length, 0.95 * G["radius"])]))
        pkg.sea = ng3d.Rec("Sea", waves=waves, level=SEA_LEVEL, amp_box=[float(lo[0]), float(lo[1]), float(hi[0]), float(hi[1])],
                           amp_size=[int(cells[0]), int(cells[1])], grid_batch=grid_batch, trail_batch=trail_batch,
                           grid_radius=G["radius"], grid_snap=G["snap"], grid_height=G["height"],
                           uv_map=[[float(v) for v in m] for m in uv_map], sun=[float(v) for v in sun],
                           shade=list(SEA_SHADE), trail_boats=nb, trail_samples=ns, trail_spacing=T["spacing"],
                           trail_life=T["life"], trail_spread=T["spread"], trail_speed=T["speed"])
        pkg.sea_amp = amp_bytes.tobytes()
        whole = float((amp_bytes == 255).mean())
        flat = float((amp_bytes == 0).mean())
        self.say("the sea: %d waves (%s), an amplitude map of %dx%d over (%.0f, %.0f)-(%.0f, %.0f) (%.0f %% whole, "
                 "%.0f %% flat), a grid of %d vertices and %d triangles (%d rings of %d to %.0f units), %d trails of "
                 "%d samples; its texture coordinates fitted to within %.4f" % (
                     len(waves), ", ".join("%.0f long, %.2f high, %.1f s" % (l, a, l / math.sqrt(SEA_G * l / (2 * math.pi)))
                                           for _, l, a, _ in SEA_WAVES),
                     cells[0], cells[1], lo[0], lo[1], hi[0], hi[1], 100 * whole, 100 * flat, len(P), len(idx) // 3,
                     rings, S, G["radius"], nb, ns, uv_err))
        self.inv["sea"] = dict(waves=[dict(direction_deg=d, wavelength=l, amplitude=a, phase=p) for d, l, a, p in SEA_WAVES],
                               amplitude_map=dict(A, size=[int(cells[0]), int(cells[1])],
                                                  box=[float(v) for v in list(lo) + list(hi)], whole_share=whole,
                                                  flat_share=flat),
                               grid=dict(G, vertices=len(P), triangles=len(idx) // 3, rings=rings),
                               trails=T, shade=SEA_SHADE, sky=SEA_SKY, sun=[float(v) for v in sun],
                               uv_map=[[float(v) for v in m] for m in uv_map], uv_fit_error=uv_err)

    def raw_batch(self, material, data, count, attrs, stride, vflags, idx, P, bflags=0):
        """A batch of its own (no model, no cell: the sea's grid and trails) over a vertex buffer of
        `data` (count vertices of this layout), with these indices; its box from P."""
        pkg = self.pkg
        vb = pkg.add("vertex_buffers", count=count, attrs=attrs, stride=stride, flags=vflags, uv_scale=[0.0, 0.0],
                     pos_origin=[0.0, 0.0, 0.0], pos_scale=0.0)
        pkg.vertex_data.append(bytes(data))
        first = len(pkg.indices)
        pkg.indices.extend(int(i) for i in idx)
        lo, hi = np.asarray(P).min(0), np.asarray(P).max(0)
        pkg.add("batches", material=material, vbuf=vb, first_index=first, n_indices=len(idx),
                bbox=[float(x) for x in list(lo) + list(hi)], node=NONE16, flags=bflags)
        return len(pkg.batches) - 1

    def stage_collision(self):
        pkg, w = self.pkg, self.wsr
        all_t, all_a, all_n = [], [], []
        files = []
        for i in range(1, 6):
            name = "WS2_common_island%d" % i
            d = w.get(SA, "Island/%s.kcl" % name)
            pkg.add("collisions", name=pkg.str(name))
            pkg.blobs.append(d)
            t, a, n = kcl_triangles(d)
            ok = np.isfinite(t.reshape(len(t), -1)).all(1)
            all_t.append(t[ok]), all_a.append(a[ok]), all_n.append(n[ok])
            files.append(dict(name=name, bytes=len(d), triangles=int(ok.sum())))
        kcl = (np.concatenate(all_t), np.concatenate(all_a), np.concatenate(all_n))
        # tree trunks: each tree class's trunk (attribute 0x1105 in Common/ArcScene's tree KCLs,
        # model space) as an upright cylinder through the placed matrix
        trunks = {}
        arc = "Common/ArcScene/common.carc"
        for cls, f in ((2, "WS2_common_tree.kcl"), (3, "WS2_common_tree2.kcl"), (4, "WS2_common_tree3.kcl")):
            t, a, _ = kcl_triangles(w.get(arc, f))
            v = t[a == 0x1105].reshape(-1, 3)
            cx, cz = float(v[:, 0].min() + v[:, 0].max()) / 2, float(v[:, 2].min() + v[:, 2].max()) / 2
            rad = float(np.max(np.hypot(v[:, 0] - cx, v[:, 2] - cz)))
            trunks[cls] = (cx, cz, float(v[:, 1].min()), float(v[:, 1].max()), rad)
        n = 0
        for o in self.objs:
            if o["cls"] not in trunks:
                continue
            cx, cz, y0, y1, rad = trunks[o["cls"]]
            M = o["mtx"]
            base = M[:3, :3] @ [cx, y0, cz] + M[:3, 3]
            top = M[:3, :3] @ [cx, y1, cz] + M[:3, 3]
            sxz = math.sqrt(abs(np.linalg.det(M[[0, 2]][:, [0, 2]])))
            pkg.add("cylinders", x=float(base[0]), z=float(base[2]), y0=float(min(base[1], top[1])),
                    y1=float(max(base[1], top[1])), radius=float(rad * sxz))
            n += 1
        self.say("collision: %s; %d trunk cylinders (model trunks %s)" % (
            ", ".join("%s %d B %d tris" % (f["name"], f["bytes"], f["triangles"]) for f in files), n,
            {CLASSES[c][0]: [round(x, 1) for x in v] for c, v in trunks.items()}))
        self.inv["collision"] = dict(files=files, cylinders=n,
                                     trunks={CLASSES[c][0]: dict(x=v[0], z=v[1], y0=v[2], y1=v[3], radius=v[4])
                                             for c, v in trunks.items()})
        self.cylinders = [(c.x, c.z, c.y0, c.y1, c.radius) for c in pkg.cylinders]
        return kcl


def three_unit_sea(sea_m):
    """mt_za_water (WS2_common_sea's open sea) as three texture units: jog_waterA is only its indirect
    map (dropped with the warp), so its texgens 2 (jog_waterB) and 3 (sea2) go through texture
    matrices 1 and 2 with their own SRTs there (SEA_WATER_MATRIX; stage_anims moves sea_P's tracks
    alike). Changes the brres material in place, before anything is converted from it."""
    m = next(m for m in sea_m.materials if m.name == SEA_WATER)
    for j, k in SEA_WATER_MATRIX.items():
        m.srt[k] = dict(m.srt[j])
        m.texgens[j]["matrix"] = k


def tev_color_word(a, b, c, d, scale=0, dest=0):
    """A GX TEV colour combiner word (BP 0xC0 + 2i, without its address): d + lerp(a, b, c), clamped,
    times 2^scale, into dest (0 PREV). Inputs: 0 CPREV, 10 RASC, 11 RASA, 14 KONST, 15 ZERO..."""
    return d | c << 4 | b << 8 | a << 12 | 1 << 19 | scale << 20 | dest << 22


def tev_alpha_word(a, b, c, d, dest=0):
    """A GX TEV alpha combiner word (BP 0xC1 + 2i): d + lerp(a, b, c), clamped, the swap tables 0.
    Inputs: 0 APREV, 1 A0, 4 TEXA, 5 RASA, 6 KONST, 7 ZERO."""
    return d << 4 | c << 7 | b << 10 | a << 13 | 1 << 19 | dest << 22


def tev_field(word, lo, n, value):
    return (word & ~(((1 << n) - 1) << lo)) | value << lo


def grid_material(M):
    """The wave grid's material (a variant of mt_za_water: its three units and stages, the flat sea's
    look), with two stages more: times the colour channel's rgb, doubled (the vertices' shade, half
    of it in the channel: sea.h), then plus SEA_SKY (konst 1) times its alpha (their brightening); the
    channel's colour and alpha the vertex's, unlit. The grid's vertex program (and the host's soft
    backend) give the vertices those."""
    n = M.n_stages
    M.color_env[n] = tev_color_word(15, 0, 10, 15, scale=1)       # cprev x rasc x 2
    M.alpha_env[n] = tev_alpha_word(7, 7, 7, 0)                    # aprev
    M.color_env[n + 1] = tev_color_word(15, 14, 11, 0)            # cprev + konst x rasa
    M.alpha_env[n + 1] = tev_alpha_word(7, 7, 7, 0)
    for k in (n, n + 1):
        # no texture; the rasterised colour from channel 0 (TREF: map 7, coord 7, off, COLOR0A0)
        lo = 12 * (k & 1)
        M.tref[k // 2] = tev_field(M.tref[k // 2], lo, 12, 7 | 7 << 3)
        # konst colour K1 (KCSEL 0x0D) for the second
        M.ksel[k // 2] = tev_field(M.ksel[k // 2], 4 if not k & 1 else 14, 5, 0x0D)
    M.n_stages = n + 2
    M.tev_konst[1] = list(SEA_SKY) + [255]
    M.n_chans = 1
    M.chan_color = (M.chan_color & ~3) | 1
    M.chan_alpha = (M.chan_alpha & ~3) | 1
    M.flags &= ~(ng3d.MAT_FOG | ng3d.MAT_PRELIT)


def wake_registers(M, clr0, mname):
    """A wake material's colour registers at its CLR0's frame 0 (the day's: WSR picks the frame by the
    time of day)."""
    for t, x in clr0.materials.get(mname, {}).items():
        reg = convert.CLR_TARGET_REG[t]
        if ng3d.CR_TEV0 <= reg <= ng3d.CR_TEV2:
            v = x["values"][0]
            M.tev_reg[1 + reg - ng3d.CR_TEV0] = [(v >> 24) & 255, (v >> 16) & 255, (v >> 8) & 255, v & 255]


def trail_material(M):
    """The boats' trails' material (a variant of the wake's m_nami_ef: ws2f_waterStream's streaks in
    the wake's colours): its first stage alone, the foam's alpha times the vertices' (RASA, the
    ribbon's fade, in place of the C0 alpha the wake's is multiplied by) instead of the bow wave's
    mask; the channel's alpha the vertex's."""
    if (M.alpha_env[0] >> 7) & 7 != 1:
        raise SystemExit("the wake's material isn't what the trails are made from (stage 0 alpha %06x)" % M.alpha_env[0])
    M.alpha_env[0] = tev_field(M.alpha_env[0], 7, 3, 5)
    M.tref[0] = tev_field(M.tref[0], 7, 3, 0)
    M.n_stages = 1
    M.n_chans = 1
    M.chan_alpha = (M.chan_alpha & ~3) | 1


def batch_triangles(pkg, bi, M=None):
    """A batch's triangles (n, 3, 3) as float64, decoded (VA_POS16 too), moved by M (4x4) if given."""
    b = pkg.batches[bi]
    vb = pkg.vertex_buffers[b.vbuf]
    P = ng3d.vertex_positions(vb, pkg.vertex_data[b.vbuf]).astype(np.float64)
    idx = np.array(pkg.indices[b.first_index:b.first_index + b.n_indices], np.int64)
    T = P[idx].reshape(-1, 3, 3)
    if M is not None:
        T = T @ M[:3, :3].T + M[:3, 3]
    return T


def occludes(m):
    """Does a material's draw hide what is behind it: opaque (no blending, not translucent), depth
    tested and written, no alpha test, not culled whole."""
    import preview
    return not m.flags & ng3d.MAT_XLU and not m.blend & 1 and m.zmode & 0x11 == 0x11 and \
        not preview._has_alpha_test(m.alpha_compare) and m.cull != 3


def sun_matrix(penv):
    """Where the sun goes in the sky model (whose origin is the eye): SUN_DISTANCE along the .penv
    position's direction, turned as its STD billboard would be when looked at (its +z back toward
    the eye, x level), at the scale WSR's update gives it at that distance. (The engine leaves the
    sky's nodes unturned; a renderer that billboards them turns it the same way.) -> (4x4, info)."""
    d = np.asarray(penv["pos"], np.float64)
    d /= np.linalg.norm(d)
    z = -d
    x = np.cross([0.0, 1.0, 0.0], z)
    x /= np.linalg.norm(x)
    y = np.cross(z, x)
    s = penv["size"] * SUN_DISTANCE / SUN_SCREEN
    M = np.eye(4)
    M[:3, 0], M[:3, 1], M[:3, 2], M[:3, 3] = x * s, y * s, z * s, d * SUN_DISTANCE
    info = dict(direction=[round(float(v), 4) for v in d], distance=SUN_DISTANCE, scale=round(s, 4),
                elevation_deg=round(math.degrees(math.asin(d[1])), 2),
                azimuth_deg=round(math.degrees(math.atan2(d[0], d[2])), 2))
    return M, info


def sample_path(anim, node, step):
    """[(frame, x, y, z)] of a CHR0 node's translation, every `step` frames."""
    nd = anim.nodes[node]
    fr = np.arange(0.0, anim.frames + 1e-6, step)
    return np.array([[f] + [c(f) for c in nd["trans"]] for f in fr])


def path_length(p):
    return float(np.sum(np.linalg.norm(np.diff(p[:, 1:4], axis=0), axis=1)))


def nearest_frame(p, pos):
    return float(p[np.argmin(np.linalg.norm(p[:, 1:4] - np.asarray(pos), axis=1)), 0])


def add_orbit(ab, model, node, radius, period, n=16):
    """A node going round a circle of `radius` on x/z once in `period` frames, heading along it:
    Hermite keys with the circle's own slopes (16 of them keep it within 0.02% of round)."""
    w = 2 * math.pi / period
    kx, kz = [], []
    for i in range(n + 1):
        th = 2 * math.pi * i / n
        f = period * i / n
        kx.append((f, radius * math.sin(th), radius * math.cos(th) * w))
        kz.append((f, radius * math.cos(th), -radius * math.sin(th) * w))
    ab.add(ng3d.TT_NODE_TRANS_X, node, kx, ng3d.IP_HERMITE, model=model)
    ab.add(ng3d.TT_NODE_TRANS_Y, node, [(0.0, 0.0, 0.0)], ng3d.IP_LINEAR, model=model)
    ab.add(ng3d.TT_NODE_TRANS_Z, node, kz, ng3d.IP_HERMITE, model=model)
    # the direction of travel at angle th is (cos th, 0, -sin th): a Y rotation of th + 90 degrees
    ab.add(ng3d.TT_NODE_ROT_Y, node, [(0.0, 90.0, 0.0), (float(period), 450.0, 0.0)], ng3d.IP_LINEAR, model=model)
    for t in (ng3d.TT_NODE_ROT_X, ng3d.TT_NODE_ROT_Z):
        ab.add(t, node, [(0.0, 0.0, 0.0)], ng3d.IP_LINEAR, model=model)
    for t in (ng3d.TT_NODE_SCALE_X, ng3d.TT_NODE_SCALE_Y, ng3d.TT_NODE_SCALE_Z):
        ab.add(t, node, [(0.0, 1.0, 0.0)], ng3d.IP_LINEAR, model=model)


def texture_summary(pkg):
    names = {v: k for k, v in ng3d.ENUMS["TexFormat"].items()}
    out = Counter()
    n = Counter()
    for t, d in zip(pkg.textures, pkg.texture_data):
        out[names[t.format]] += len(d)
        n[names[t.format]] += 1
    return {k: dict(count=n[k], gx_bytes=out[k]) for k in sorted(out)}


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("data", help="the port's data directory (the package goes to DATA/wuhu/island.ng3d)")
    ap.add_argument("--wsr", default=DEFAULT_WSR, help="the Wii Sports Resort extraction's root (holding files/)")
    ap.add_argument("--bake", default=DEFAULT_BAKE,
                    help="the renderer's ng3d-bake, run as TOOL UNBAKED OUT (default: %s)" % os.path.relpath(
                        DEFAULT_BAKE, ROOT))
    ap.add_argument("--no-bake", action="store_true",
                    help="write the GX texels as they are (the host converts them; the 3DS refuses the package)")
    ap.add_argument("--keep-raw", action="store_true",
                    help="keep the unbaked package too, as DATA/wuhu/island.unbaked.ng3d (tools/g3d/preview.py "
                         "reads GX texels)")
    ap.add_argument("--json", help="where the inventory goes (default DATA/wuhu/NAME.json)")
    ap.add_argument("--name", default="island",
                    help="the package's name: DATA/wuhu/NAME.ng3d (default island; the speed round's v2 package "
                         "was staged as island_v2 beside the version 1 one the engine read until then)")
    ap.add_argument("--pvs", default=DEFAULT_PVS,
                    help="the cells' visibility tool, ng3d-pvs (default: %s)" % os.path.relpath(DEFAULT_PVS, ROOT))
    ap.add_argument("--no-pvs", action="store_true", help="no visibility rows (CVIS)")
    ap.add_argument("--no-prelit", action="store_true", help="leave every material lit at run time (comparisons)")
    ap.add_argument("--no-pos16", action="store_true", help="write every position as f32 (comparisons)")
    a = ap.parse_args(argv)
    bake = None if a.no_bake else a.bake
    if bake and not os.access(bake, os.X_OK):
        raise SystemExit("no ng3d-bake at %s: build the host target (cmake -S . -B build-host && cmake --build "
                         "build-host --target ng3d-bake) or give --bake PATH. The 3DS refuses a package whose "
                         "textures are still GX's (--no-bake writes one for the host anyway)." % bake)
    if not a.no_pvs and not os.access(a.pvs, os.X_OK):
        raise SystemExit("no ng3d-pvs at %s: build the host target (cmake --build build-host --target ng3d-pvs) or "
                         "give --pvs PATH (--no-pvs: a package without visibility rows)" % a.pvs)
    t0 = time.time()
    st = Stager(Wsr(a.wsr), prelit=not a.no_prelit, pos16=not a.no_pos16)
    st.stage_island()
    st.stage_props()
    st.build_cells()
    st.stage_instances()
    st.stage_anims()
    kcl = st.stage_collision()
    st.stage_sea(kcl)
    st.stage_env()
    st.stage_markers(kcl)
    if not a.no_pvs:
        # (the tool's input and output: kept with --keep-raw, as DATA/wuhu/NAME.pvs/)
        work = os.path.join(a.data, "wuhu", a.name + ".pvs") if a.keep_raw else tempfile.mkdtemp(prefix="ng3d-pvs")
        st.stage_cvis(kcl, a.pvs, work)
        if not a.keep_raw:
            shutil.rmtree(work, ignore_errors=True)
    if not a.no_prelit:
        shadow_stats = shadows.bake_soft_sun_shadow(st.pkg)
        st.inv["sun_shadow_bake"] = shadow_stats
        st.say("sun shadows: %u triangles, %u static vertices, %u strongly shadowed, %ux%u map, %ux%u PCF" %
               (shadow_stats["triangles"], shadow_stats["vertices"], shadow_stats["shadowed"],
                shadow_stats.get("resolution", 0), shadow_stats.get("resolution", 0),
                shadow_stats.get("pcf", 0), shadow_stats.get("pcf", 0)))
    pkg = st.pkg
    bad = pkg.check()
    if bad:
        for b in bad[:30]:
            print("BAD", b)
        raise SystemExit("the package has %d bad references" % len(bad))
    data = pkg.write()
    back = ng3d.Package.read(data)
    if back.write() != data:
        raise SystemExit("the package doesn't read back to the same bytes")
    out_dir = os.path.join(a.data, "wuhu")
    os.makedirs(out_dir, exist_ok=True)
    out = os.path.join(out_dir, a.name + ".ng3d")
    raw = os.path.join(out_dir, a.name + ".unbaked.ng3d") if bake else out
    with open(raw, "wb") as f:
        f.write(data)
    if bake:
        # (a bake that fails must not leave the last run's package looking like this one's)
        if os.path.exists(out):
            os.remove(out)
        subprocess.run([bake, raw, out], check=True)
        if not a.keep_raw:
            os.remove(raw)
        baked = ng3d.Package.read(open(out, "rb").read())
        left = sum(1 for t in baked.textures if t.flags & ng3d.TEX_GX_RAW)
        if left:
            raise SystemExit("%s: %d textures still GX's after the bake" % (out, left))
    # the sections of the file as written (the bake changes TDAT); texture bytes as GX has them
    sections = ng3d.Package.read(open(out, "rb").read()).sections
    st.inv["package"] = dict(path=out, bytes=os.path.getsize(out), baked=bool(bake),
                             unbaked=dict(path=raw, bytes=len(data)) if bake and a.keep_raw else None,
                             sections=[dict(tag=t, bytes=s.size, count=s.count) for t, s in sections],
                             textures=texture_summary(pkg), stats=dict(st.cv.stats),
                             positions=dict(quantum=convert.POS_QUANTUM, snap_error=st.cv.snap_error,
                                            snap_error_3d=st.cv.snap_error_3d))
    st.inv["log"] = st.log
    with open(a.json or os.path.join(out_dir, a.name + ".json"), "w") as f:
        json.dump(st.inv, f, ensure_ascii=False, indent=1,
                  default=lambda x: x.tolist() if hasattr(x, "tolist") else str(x))
    st.say("wrote %s (%s): %d bytes in %.1f s (%s)%s" % (
        out, "baked" if bake else "GX texels", os.path.getsize(out), time.time() - t0,
        ", ".join("%s %d" % (t, s.size) for t, s in sections),
        "; unbaked %d bytes kept as %s" % (len(data), raw) if bake and a.keep_raw else ""))
    for k, v in sorted(st.cv.stats.items()):
        print("  %s: %s" % (k, v))
    print("  positions on a grid of 1/%g unit: moved by at most %.4f a coordinate, %.4f in all" % (
        1 / convert.POS_QUANTUM, st.cv.snap_error, st.cv.snap_error_3d))
    return 0


if __name__ == "__main__":
    sys.exit(main())
