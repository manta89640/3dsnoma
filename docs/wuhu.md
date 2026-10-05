# Wuhu Island: the staged assets

What the walkable Wuhu Island is made of: which of Wii Sports Resort's files go into the island's
package, what each is for, how it is changed on the way, and what is left out and why. The
package is made from your own copy of the game by `tools/wuhu/stage_wuhu.py`; nothing of the
game is in this repository (docs/wuhu_design.md has the design; this file is its asset side).
Numbers are measured on the PAL disc (SP2P01) unless marked est.

## Staging

    cmake -S . -B build-host -G Ninja && cmake --build build-host --target ng3d-bake
    tools/wuhu/stage_wuhu.py DATA [--wsr ROOT] [--bake TOOL | --no-bake] [--keep-raw]

- `ROOT` is the extraction of your Wii Sports Resort disc (its `files/` is the disc's root);
  default `../wii-sports-extract/wii-sports-resort` beside this repository.
- `DATA` is the port's data directory (`tools/stage_data.py`'s, `sdmc:/3ds/noma` on the 3DS). The
  package goes to `DATA/wuhu/island.ng3d` (13,266,560 bytes), and `DATA/wuhu/island.json` records
  what went in: classes, counts, decisions, sizes, the checks below.
- The package is baked: the stager writes it with GX texels to `DATA/wuhu/island.unbaked.ng3d`
  (12,339,440 bytes), runs the renderer's offline converter on it (`TOOL UNBAKED OUT`; TOOL is
  `build-host/ng3d-bake` unless `--bake` names another), checks that no texture is left in GX's
  format, and deletes the unbaked file unless `--keep-raw` (keep it for `tools/g3d/preview.py`,
  which reads GX texels). Without the tool it stops and says how to build it: the 3DS refuses an
  unbaked package, because converting GX texels at load time on the CPU thread would race the GX
  path's texture uploads, which share `pica_tex.h`'s tables, on the GPU thread. `--no-bake` writes
  an unbaked package anyway (the host converts as it loads, in about 37 ms).
- It takes about 4 s on a Mac and reads only the archives it needs (Yaz0 U8 `.carc`, unpacked in
  memory); two runs give the same bytes. Before writing it checks every reference in the package,
  that the package reads back to the same bytes, and that every warp spot has room for the Mii
  (below); it stops if any fails.

The tools: `tools/g3d/brres.py` reads BRRES (models, textures, CHR0/SRT0/CLR0 keyframes, SCN0
light sets), `tools/g3d/gxtex.py` decodes GX textures, `tools/g3d/convert.py` turns BRRES
objects into package records (a model can be built from parts of several: the sky with the sun in
it, Wedge Island with its stretch of sea), `tools/g3d/ng3d.py` writes and reads packages
(`ng3d.py selftest` compares its structs with `engine/g3d/ng3d_format.h`; `ng3d.py check FILE`
validates a package and round-trips it), and `tools/g3d/preview.py` renders an unbaked package to
PNG. `tools/wuhu/check_views.py` renders a package from its `view_*` cameras and compares the
pictures with references.

## The package

The baked file's sections:

| section | records | bytes | what |
|---|---|---|---|
| TDAT | 92 textures | 5,158,912 | PICA texels with their mip levels (TEXS: the records); 4,229,920 bytes as GX texels unbaked |
| VDAT | 348 vertex buffers | 4,012,112 | 171,702 vertices, 20-36 bytes each (VTXB: the records) |
| BLOB | 5 KCL files | 2,961,008 | the island's collision, as shipped (COLL: the records) |
| IDAT | 431,853 indices | 863,706 | u16 triangle lists |
| KEYS | 9,870 keys | 118,440 | animation keys (TRAK: 477 tracks, ANIM: 20 animations) |
| MATS | 90 materials | 48,600 | GX register images (TMTX: 128 texture matrices) |
| BTCH | 549 batches | 21,960 | draws: a material, a vertex buffer, an index range |
| NODE | 150 nodes | 15,000 | the models' node trees (13 models) |
| CYLS | 479 cylinders | 9,580 | tree trunks |
| MARK | 139 markers | 4,448 | spawn, landmarks, camera presets, warps |
| the other 15 | | 51,978 | strings, texture/vertex-buffer/collision records, texture matrices, models, cells and their batch lists, instances, animations and tracks, environment, lights, light sets, fogs |

The header, the section table and alignment take the rest.

At run time (est., from the package): textures 5.16 MB in their PICA formats (all levels; the
loader counts 5,013 KB), geometry 4.88 MB, collision 2.96 MB, everything resident.

## Sources

Paths are inside the extraction's `files/`; an archive's members after the colon.

| asset | source | role | staged as |
|---|---|---|---|
| `WS2_common_island` (9 models, 72 textures, island and sea animations) | `Stage/Static/StageArc.carc:Island/G3D/WS2_common_island.brres` (7,236,352 B) | the island, sea, sky | cells (island, island_V), models (sea, sky, Wedge Island), animations |
| `sea2_day` | `StageArc.carc:Island/G3D/WS2_common_seatex.brres` | the sea's day colour (WSR swaps it for the placeholder `sea2_dummy` at run time) | a texture |
| the sun | `StageArc.carc:Sun/G3D/WS2_common_sun_model.brres` (and its SRT0) | the sun in the sky | a part of the sky model |
| placed objects | `Stage/PlnScene/PlnEnv0.carc:WS2_pln_island_tag0.pmp` (1,228 records) | Island Flyover's day placement: the complete set (research placement.md 0) | baked props, instances |
| the objects' models | `StageArc.carc:<Class>/G3D/*.brres` (table below) | trees, boats, cars, ... | baked props, models |
| collision | `StageArc.carc:Island/WS2_common_island1..5.kcl` (2.96 MB, 42,934 triangles) | the ground and walls | BLOB as shipped |
| tree trunks | `Common/ArcScene/common.carc:WS2_common_tree{,2,3}.kcl` (trunk triangles, attribute 0x1105) | trunk size per tree class | cylinders |
| lights | `Common/BicScene/common.carc:Env/WS2_bic_F0/WS2_bic_F0_Light.plight` | the ground-level day's lights and ambient colours | LITE, LSET |
| light sets | `Common/Static/common.carc:G3D/WS2_Scene.brres` (SCN0) | which lights and ambient each material light set uses | LSET |
| fog table | `common.carc:Env/WS2_bic_F0/WS2_bic_F0_fog.pfog` | fog 0 (off) | FOGS[0] |
| the sea's fog | `StageArc.carc:Binary/WS2_common_binary/ws2_common_day.pfobj` | the island writes it into fog 1 and switches it on | FOGS[1] |
| the sun's place | `common.carc:Env/WS2_bic_F0/WS2_bic_F0_Env.penv` | where the sun stands, its size | the sun's direction and scale |
| landmarks | `Stage/Shared/Activity.carc:LandMarkData/LandMarkData.exbin` | the Flyover's 80 named places | markers, the warps' names |
| camera shots | `Stage/Shared/SeqData.carc:Map/WS2_seq1_IslandCamParam.exbin` | the title sequence's 15 ground-level shots | markers |
| the WSR Mii's body | `Common/Kokeshi/common.carc:G3D/WS2_avaA1.brres` | k's check (head_set's height) | the inventory |

The island's materials use light set 0 and fog 0 (off); props light set 1; flags light set 6; the
open sea's two materials fog 1. No island material references `lm_0`/`lm_1` (character light maps).

## The island at Island Flyover's visibility

WSR shows the island model's nodes per mode from a table in main.dol (0x8066A37C, research
wsrdol.md 2). Island Flyover in daylight (mode 13) is the only mode that shows everything at full
detail, so it is the canonical set:

- **Shown (33 nodes, 59,635 triangles):** Island_1-15 models (the main island, the islets), their
  Kiwa edge strips, Island_16_Model (Wedge Island, the golf island, at full detail), Island_17c
  (the harbour pier), Island_18 (rope barriers), Island_19b (Mt. Wuhu with its cave and lava
  chamber), Island_20b (the underwater cave), Island_21 (the round stage), Island_3b/3c/8b (court,
  town buildings, hotel). All but Island_16 go into the cells (49,659 triangles); Wedge Island is
  an instance of its own (below).
- **Hidden:** 17 and 17b (pier variants), 19 and 20 (mountain and coast without caves), 19c and 20c
  (their night versions), Island_22 (a villa, hidden in every mode), Light1-4 (the night lights).
  Island_16b and Water16b (the golf island's light model and its sea rim) are hidden in mode 13
  and staged as Wedge Island's far model.
- `WS2_common_island_V` (198 triangles, a translucent blend over the volcano's inside) goes into
  the cells with the island.

### Wedge Island far away

WSR's ground-level modes (1-4, 6-10, 15-16) show the golf island's light model, Island_16b, with
its own sea rim, Water16b, instead of Island_16 and Water16. From the main island the two can't be
told apart, even through a 6x zoom (research env_choice/env.md, its quay renders). So Wedge Island
is a still instance (`INST_STATIC`, class 0) of a model made of `Island_16_Model` and the sea's
`Water16` (10,794 triangles), whose model chain (`Model.lod_next`) goes to `Island_16b_Model` and
`Water16b` (4,065 triangles: no shore foam, simpler waves) from 12,000 units off the island's
centre. Everywhere on Wedge Island and its islets (all within 5,000 units of the centre) the full
model is drawn, from the main island and its islets (25,000 and more) the light one: the switch
happens over open sea. Through the `view_distant` camera this takes the frame from 37,814 triangles
to 31,095 and from 159 draws to 125. Being an instance, Wedge Island is culled whole (one sphere,
radius 6,640) instead of by cell; its trees and props stay in their cells. Its two models carry the
whole island model's node list (the sea's too), unused but for two nodes: about 10 KB.

## Cells and detail levels

The island and the props that stand still are one static world cut into cells of 4,000 units on
x/z: a 12 x 10 grid from (-36,000, -16,000), 44 cells with geometry (Cell index = z * 12 + x).
Terrain triangles go to the cell of their centroid; a placed object goes whole to the cell of its
position. A cell has a batch per material (per detail level for trees), its vertex buffers
deduplicated and split before 65,536 vertices. A cell's `group` is the island node with most of
its triangles.

Each cell lists its batches for three levels, by the eye's distance to the cell's box (a cell with
terrain alone has one list for all three):

| level | from | what | whole world |
|---|---|---|---|
| 0 | 0 | terrain, trees at full detail, every prop | 435 batches, 110,267 triangles |
| 1 | 3,000 | terrain, the trees' lighter models, palms, boats, fountain, the two gates | 406, 84,620 |
| 2 | 20,000 | terrain | 320, 49,857 |

Why these distances: on the 3DS's 240-line top screen with a 45-degree view, a 230-unit tree is
about 22 pixels tall at 3,000 units, where `WS2_common_tree_H` and `_M` look alike, and a 30-unit
car or parasol is 3 pixels; at 20,000 units a tree is 3 pixels and only Wedge Island and the far
islets are that far from anywhere on the main island.

## Placed objects

`WS2_pln_island_tag0.pmp`'s group-1 records as WSR's object manager spawns them for the Flyover's
1-player game (fn_8026D230 / fn_8026D514: filter 0, include bit 0, exclude mask 2): 699 objects.
Group 5 (130 ambient-sound boxes) and groups 3/7 (the Flyover's own game objects and markers) are
not objects anyone sees.

| class | placed | staged as | how |
|---|---|---|---|
| conifer | 159 | baked, two levels | `WS2_common_tree_H` (level 0), `_M` (level 1) |
| broadleaf tree | 179 | baked, two levels | `WS2_common_tree2_H`, `_M` |
| palm | 141 | baked, levels 0-1 | `WS2_common_tree3` (it has one model) |
| cruiser at anchor | 28 | baked, levels 0-1 | |
| yacht | 19 | baked, levels 0-1 | |
| fountain | 1 | baked, levels 0-1 | its water's SRT0 in the world animation |
| jet-ski start gate, wakeboard goal gate | 1 + 1 | baked, levels 0-1 | 266 and 400 units wide |
| parked car | 32 | baked, level 0 | |
| parasol | 17 | baked, level 0 | |
| kayak | 10 | baked, level 0 | |
| golf tee | 9 | baked, level 0 | |
| flag A | 3 | baked, level 0 | at rest: their SHP0 (cloth) is left out |
| frisbee-dog plate, archery target, table-tennis table, cycling gate | 4 | baked, level 0 | the venues' low-detail props |
| windmill | 8 | instances | CHR0 `WS2_common_windmill` turns the blades (299 frames) on the placed matrix; phases staggered 37 frames |
| balloon | 3 | instances | CHR0 `WS2_common_balloon_flight` (7,999 frames, a ~2,000-unit loop) on the placed matrix; all three variants use it (the others are the basketball and canoe scenes'), phases spread |
| airship | 1 | instance | CHR0 `WS2_common_airship_flight_title`: the title sequence's flight, a world path 2,364-2,813 units up over the main island (7,500 frames) |
| moving car | 10 | instances | CHR0 `WS2_common_car_drive`: one world route along the roads (12,000 frames); each car starts where the route passes nearest its placement |
| moving cruiser | 9 | instances | CHR0 `WS2_common_cruiser_revolve1..8`: eight world routes; each cruiser takes the route passing nearest it, at 20/29 units a frame, with its placed scale (1.3) |
| moving liner | 1 | instance | CHR0 `WS2_common_move_liner_revolve1` (a world route) at 0.00067 key a frame: 43,284 frames a lap |
| whale | 8 | instances | CHR0 `WS2_common_whale_surface_gush_dive` (1,319 frames: surface, blow, dive) where placed |
| seagull | 40 | instances | `WS2_common_bird_fly_a` flapping, on a circle of the placed scale's radius (500) at 1.9 units a frame |
| hawk | 14 | instances | the same with `WS2_common_hawk_fly_a` |
| spectator Mii | 358 | **left out** | exclude bit 2: the Flyover's 1-player game doesn't spawn them (they are also RFL Miis, not models) |
| golf flag | 9 | **left out** | exclude bit 2, as above |
| waterfall | 1 | **left out** | a particle effect (`WS2_cmn_waterfall`), no model |

How the moving ones move, and where it comes from: the liner advances its 29-key route by 0.00067
key a frame (its update, fn_802A5694); a moving cruiser by 20 / the route's length, i.e. 20/29
units a frame along it (fn_802A4A8C); the windmills, balloons, airship and cars by one frame a
frame. Gulls and hawks steer toward targets with a speed cap of 1.9 units a frame (fn_80281030):
a plain circle stands in for that. The cruisers' and liner's routes are 29-key CHR0s on nodes their
models don't have: here the route drives the model's root node, retimed to frames of the island's
clock. An airship placed at the origin with the default `flight` CHR0 would fly a 2,500-unit
circle at sea level through the island: the Flyover must place it by code not traced, so the title
sequence's flight is used instead. (The fn_ addresses in this file are Wii Sports Resort's
main.dol, not Wii no Ma's; Wii no Ma's functions go by their names.)

A moving model's box (`Model.bbox`, `radius`, model space) holds every pose its instances'
animations give it, not only its rest pose: the cars, cruisers, liner and airship run world routes
on an identity instance matrix, so their rest boxes sit at the origin, and a renderer that culls
an instance by its matrix and its model's box would drop them wherever they are seen. Grown so
(radius): car 35 -> 6,781, cruiser 68 -> 25,608 (its eight routes), liner 168 -> 16,061, airship
491 -> 11,028, balloon 295 -> 3,131, whale 342 -> 1,134, gull and hawk ~10 -> ~720, windmill 396
-> 474 (the blades turning). The price is that those instances are rarely culled (up to 23 more a
frame through the reference cameras); a renderer that culls by the posed root node instead could
do better with the rest boxes.

Not in this placement file, so not staged: the seaplanes (`PlnSub.carc`'s Flyover course and
`StageArc.carc:SeaPlane/WS2_common_pln_path.pmp`'s routes), hermit crabs (title sequence), and the
night's torches, campfire and fireworks (`WS2_pln_island_tag2.pmp`).

## Animations

| animation | frames | from | drives |
|---|---|---|---|
| `world_srt` | 2,400 | SRT0 `WS2_common_island` (lava), SRT0 `WS2_common_fountain` (300, repeated 8 times) | texture matrices of the cells' materials |
| `world_clr` | 200 | CLR0 `WS2_common_island` (lava, the volcano's inside), CLR0 `WS2_common_island_V` | colour registers |
| `sea_srt` | 20,000 | SRT0 `WS2_common_sea_P` and `WS2_common_sea_nami` (1,000, repeated 20 times): the Flyover's set (main.dol 0x806BE698) | the sea's texture matrices (Wedge Island's sea rim shares its materials) |
| `sea_clr` | 1,000 | CLR0 `WS2_common_sea_nami` | the waves' material alpha |
| `sky_srt` | 39,996 | SRT0 `WS2_common_vr` (9,999, repeated 4 times) and SRT0 `WS2_common_sun_model` (4,000, repeated 10 times, 0.01 % slower to fit) | the clouds' scroll, the sun's rays turning |
| 15 node animations | 299-111,848 | the objects' CHR0s (above) | instances |

SRT0 keys are nw4r's Hermite keys (frame, value, slope); CLR0 colours (a value a frame) become
linear keys per component, collinear ones dropped, and a component whose mask byte is set (the
material keeps its own) gets no track. An animation made of several (lava and fountain, sea and
waves, clouds and sun) runs to their common length, the shorter ones' keys repeated. A material
animation sets all five components of a texture matrix it touches (nw4r does), so constants are
tracks too.

## Materials and textures

A material is GX's register image as its display lists leave it: TEV colour and alpha words,
TREF, KSEL, the colour and konst registers, alpha compare, depth mode, blend, constant alpha,
colour channel 0 (control words and colours), texture maps, texgens and one texture matrix per
texgen (nw4r's SRT and matrix mode, map mode, effect matrix), light set, fog, cull, translucency.
The display lists write some registers in masked halves (BP 0xFE): a TEV KSEL word gets its
swap-table bits and its konst selections in two writes, so the reader runs the lists through a
register file instead of taking the first write. (The research renderer took the first write, so
its pictures have the swap-table bits as konst selections: wrong on the sea and the lava.)

- 90 materials. 6 have indirect stages (the lava `ef_volSurface`, the sea's `mt_za_water` and the
  river's `mt_zc_w_nagare` with their `_off` twins, the fountain's water), flagged and dropped as
  the engine drops them: the warp is lost, the scrolls stay. No material reads texture
  coordinates past tex1, or colour channel 1.
- The blend word's dither, colour-update and alpha-update bits are written by nw4r through a mask
  that leaves them to the global state, where colour update is on: the package has that bit set.
- Texgen `matrix` is the texgen's own texture matrix when nw4r applies one (its SRT is in use, or
  it maps the environment or a projection); an environment-mapping texgen (normalised) has it as
  its post matrix too.
- 92 textures, 4,229,920 bytes of GX texels with every level down to 8x8 (the PICA's smallest),
  5,158,912 bytes once baked. CMPR becomes ETC1 (37 textures) or, when a block uses CMPR's
  transparent texel, ETC1A4 (14), I8 L8 (15, the sun's two among them), IA8 LA8 (11), IA4 LA4 (2),
  RGB565 RGB565 (4), RGB5A3 and RGBA8 RGBA8 (9). A texture is never shared by name across files
  (56 names have different pictures in different WSR files).

## Environment

WSR's island scenes don't share one environment, and the Flyover's (Pln F0) is made for flying:
its fog turns the sea pale a few thousand units out and its sun comes from the south-south-east.
The package takes the day as WSR runs it at ground level, in cycling (mode 11), the island's own
ground-level day (research env_choice/env.md, its set (f), decoded from the files and main.dol):

- **Lights** (`WS2_bic_F0_Light.plight`, intensity multiplied in), in the sets of SCN0
  `WS2_Scene` (a material's light set number is the set's index):

  | set | name | ambient | lights |
  |---|---|---|---|
  | 0 | Map (the island) | #788287 | Map_Light: white, at (-30,000, 48,000, 60,000), 35.6 degrees up, from the south-south-west |
  | 1 | Obj (props) | #877a63 | Map_Light, Obj_Light: #8c795a at (0, 100,000, 25,000), 76 degrees up |
  | 6 | Flag (flags) | #bec8d2 | Flag_Light: white, directional (below) |
  | 8 | Obj2 | #646464 | Map_Light (Obj2_Light is off in this file) |
  | 2-5, 7, 9 | Abatar, stadium, jet-ski sea, player object, canoe sea, cycling fences | | not used by the island's materials |

  A light is on only when its flags have bits 0 and 0x40 (EGG's LightObject, fn_801BC6E0):
  Abatar_Light (flags 0x7A1) is off, so set 2 has no light. EGG's type-0 lights are positions in
  the world, unattenuated: Map_Light and Obj_Light, 82,000 and 103,000 units out, are kept as
  point lights (the engine lights with the direction from them toward the origin; across the island
  that differs from the true direction by up to 6 degrees on the east beach, 23 on Wedge Island).
  Flag_Light at (-10, 0, 50), PlayerObj_Light and Sea_Light sit within 400 units of the origin and
  light props thousands of units away: they are directions written as positions, staged as
  directional lights from there toward the origin. The Pln F0 lights (the Flyover's: sun from the
  south-south-east, Obj ambient #4d5173) are the alternative, in env_recommended.json.
- **Fogs:** fog 0 off: every daytime `.pfog` WSR has (Pln, Bic, the title sequence, skydiving) has
  fog 0 type NONE, so WSR never fogs the terrain, the props, the distant islands or the sky by day.
  Fog 1 is `ws2_common_day.pfobj`, linear in view depth from 27,978 to 65,527 to #83d5ff: the
  island object writes its `.pfobj` into the FogManager's second fog (fn_80275B38 points at
  fogs[1]; fn_802757F0 loads it there and sets its enable bit whatever the file's own flag says).
  Only the open sea's materials, `mt_za_water` and `mt_za_water_off`, use fog 1: the sea fades into
  the haze at the horizon. (Stage 1 had this the wrong way round, after the research's first
  reading: the `.pfobj` in fog 0, which washed out the sky 62,000-70,000 units off, and Pln F0's
  fog 1, 2,000-30,000 #66d1ff, which turned the near sea pale.) WSR rescales fog 1's start and end
  each frame by the camera's pitch (fn_80274A5C): at walking pitches under 15 degrees that is at
  most 3 %, left out.
- **The sky** (`WS2_common_vr`) follows the eye (the engine draws it centred on the eye, first,
  without depth). Its materials are unfogged in the package whatever their fog index says (WSR's
  sky is fog 0, off by day; a fog 0 turned on later, a night's, must not reach the dome). The
  clear colour is fog 1's #83d5ff, so that the gap under the dome's lower edge (231 units under
  the eye) meets the fogged sea in its own colour; the package has no clear colour of its own
  (the scene sets it; `tools/g3d/preview.py` takes the first fog in use).
- **The sun** (CmnObj class 1, 太陽) is a part of the sky model: `WS2_common_sun_model`, a glow disc
  (`P_sun_core1d`, alpha-blended, warm white) and a fan of rays (`P_sun_spreadline5_2` twice,
  added, turning both ways: the sky animation). WSR's sun update (fn_802A8D80) puts it at the
  scene's `.penv` position with x and z taken relative to the camera (so it keeps its direction as
  the eye moves, like the dome) and scales it each frame to `size * |view z| / (0.5 * 2.39 *
  10000 * 456)`: a constant size on the screen, 10,000 model units on 200 of 456 lines at size 200,
  which makes the glow disc 27 degrees across. So it sits in the `.penv`'s direction (Bic F0:
  (-34,200, 25,000, 93,964), 14.0 degrees up, 20 degrees west of +z) 60,000 units out (inside the
  dome and the camera's far plane), turned as its billboard would be when looked at, at the scale
  WSR gives it there (2.20). Its batches follow the dome's in the sky's draw order (gradient,
  clouds, glow, rays): as in WSR, whose sun is translucent and drawn after the opaque clouds, the
  sun shines over the clouds, and the island and the sea, drawn after the sky, cover it. The visible
  sun is lower than Map_Light (35.6 degrees): that is where WSR draws it, in sight of a level
  camera. WSR keeps its height absolute, so from the highest ground you walk to (2,200 units) it is
  1.2 degrees lower than staged. Left out: `WS2_common_sunshine` (a glare over the whole screen)
  and `WS2_common_sun_lensfx` (flares along the screen), whose alpha and place come from where the
  sun is on the screen and a read of the depth buffer (GXPeekZ, fn_802A89C4).
- **k = 10.0** Wii no Ma Mii units per island unit (research k.md, design section 8). Both games
  hang the same RFL head (`RFL_Res.dat`, byte for byte the same in both) from a body node, its
  origin at the chin. WSR's player body (`WS2_avaA1.brres`, every Kokeshi body alike) has `head_set`
  9.70 above the feet (measured by the stager) and draws the head at scale 0.08 (the KokeshiManager,
  fn_80246F70); Wii no Ma's (`g3d/Mii/Model.lex`) has its Head node at 108.86 and the head at 0.69
  (Wii no Ma's `mii_ModelRes_ctor`, 0x80191368) at height scale 1. With the typical hairstyle's top
  69.2 RFL units above the chin, a Mii is 9.70 + 0.08 x 69.2 = 15.24 island units and 108.86 + 0.69
  x 69.2 = 156.6 Wii no Ma units tall: 10.3. Feature by feature the ratio is 9.6-11 (the neck
  10.1-11.2, the shoulders 10.0-11.1, the channel's default Mii 9.55), and the island's own measures
  agree: a doubles tennis court is 236.3 units long, 23.77 m, so 10.1 cm a unit, taking a Wii no Ma
  unit as a centimetre. Rounded to 10: a 158-tall Mii is 15.8 island units, about 1.6 m. (Stage 1's
  9.2 came from the spectators' body, whose `head_set` sits at 9.86 times their placement scale
  1.2.)
- World box (cells): x -35,120..10,985, y -217..4,639, z -14,693..20,651. The sea model reaches
  +-114,000 and the sky dome 70,000.

## Markers

- `spawn` (kind 0): (-2029, 69, 2330) facing -Z, Wuhu Town's plaza south of the coin-toss
  fountain. On the collision: the ground there is at 69.00 (attribute 0x1103, hard ground, normal
  y 1.0000), and on a 9 x 9 grid 200 units across every point is at 69.00, attribute 0x1103.
- 76 landmarks (kind 1), named in UTF-8 as the game's debug names have them without their number
  (コイン・トスの噴水, レイクピア・キャッスル, ...). The other 4 of the 80 ride vehicles (ガイド気取りのミゲル,
  ホエール・シャーク号, 飛行艇チーム, 高速ライナー・アルバトロス): their positions are relative to them, so they
  aren't markers.
- Camera presets (kind 2, position = eye, dir = target): `seqcam_0`..`seqcam_14` (the title
  sequence's shots) and `view_town`, `view_coast`, `view_beach`, `view_interior`, `view_distant`,
  `view_overview` (the research's reference views) and `view_wedge` (research env_choice's
  quay_wedge: from the plaza's sea front at a walking eye's height toward Wedge Island, 27,000
  units off). Their vertical fields of view (55, 55, 55, 65, 40, 50, 50 degrees) are in
  `stage_wuhu.py` VIEWS.
- **Warps** (kind 3, `MK_WARP`): the exploration menu's 40 places, in the order it lists them,
  each with its name, the feet on the ground and a facing (unit vector on x/z):

  | region | places |
  |---|---|
  | the town | the plaza by the coin-toss fountain; the liner's deck; the Swordplay stage and its stands; New Town |
  | the beaches and the coasts | Powder White Beach; the hotel; the tennis court; Stardust Beach; Sweet Beach; the sandy hill's fireworks site; the lighthouse's cape and the sea cave under it; the villa cove; a sea cave's ledge |
  | the mountain | the Red Gate Bridge; the suspension bridge; Wind Park; the lone cedar; the rock in the lava; the volcano's summit rim |
  | the castle and the ruins | the castle; the ruins; the ruins' heights |
  | the islands | Pointy Rock and the islet north of it; Poseidon's and Triton's rings; the two uninhabited islands; two islets in the north-east; Azuki islet's fireworks site; Wedge Island's hotel, marina, beginners' golf, Crab Rock Twins; the islet north of it; the intermediate and advanced golf islets |

  27 of them are places you can't walk to (research reach/reach.md section 9: islets, Wedge Island,
  the villa cove under 700-unit sea cliffs, the summit rim, the fenced tennis court, the liner's
  deck, the stage over water, sea caves, cliff-ringed plateaus); the other 13 are landmarks you can
  walk to, a few a region, for getting about faster. The names are the landmarks' own (from
  LandMarkData, so in the package and not in this repository); a place with no landmark of its own
  takes a name the stager composes: a landmark's name with の観客席 (its stands), の山頂 (the
  summit) or の北の小島, or 北東の小島, 北東の小島（南）, ゴルフ島の北の小島. The spots are the research's,
  from its walk graph over the island's collision at 50-degree slopes and 8-unit steps (each with
  10 units of walkable ground all round, facing its landmark or the volcano or the main island),
  kept as a small table in the stager. The stager checks each on the collision with the Mii's body
  at k (15.8 tall: spheres of radius 3.16 at 7.11 and 11.85 above the feet, a 3.95 step,
  engine/wuhu/player.cpp's proportions): the ground within 2 units of the feet, walkable (50
  degrees at most, not lava, water or a fence) there and within a step at 5 units all round, no
  water over it, nothing solid between a step and 20 units up, the spheres clear of walls (fences
  and faces steeper than 72.5 degrees, the player's rule) and of tree trunks; the feet are put on
  the ground found. Two of the research's spots failed and were moved to the nearest that pass:
  the lone cedar's stood in its trunk (now 17.5 units south of it), and the summit rim's was 5
  units from the rim's edge (now 52.5 units along it). The nearest wall to any spot is 11.2 units
  away (the marina) or more.

## Collision

The five island KCL files go in as shipped (big-endian), `WS2_common_island1`..`5`: the east and
north (17,445 triangles), the town and west (16,110), Wedge Island (8,342), the sea plane at y 0
(140), the Swordplay ring (897). 479 tree trunks are upright cylinders: each tree class's trunk in
its own KCL (attribute 0x1105) measured in model space -- conifer radius 8.7, y -41.9..177.2;
broadleaf 8.2, -3.0..164.9; palm 3.5, -0.9..38.4 -- then moved by the tree's placement matrix
(radius times its horizontal scale). Other props have collision files too (cars, tents,
parasols, windmills, boats in `PlnCol_1.carc`): not staged.

## Left out (fidelity gaps)

- Indirect texturing: the water's and lava's warp (the engine has no dependent reads).
- The water reflection (`WS2_common_island_R`, drawn mirrored by WSR),
  `WS2_common_under` (a navy skirt under the sea drawn with depth test off before the island),
  the night models (`island_L`, `vr_N`) and the evening sky (`vr_E`): daytime only.
- The sun's glare and lens flares (`WS2_common_sunshine`, `WS2_common_sun_lensfx`): their alpha
  follows the sun's place on the screen and whether the depth buffer hides it. The sun's height
  doesn't follow the eye's (1.2 degrees at the highest walkable ground). Billboards in models drawn
  by the cells are baked as placed.
- Fog 1's per-frame rescale by the camera's pitch (at most 3 % at walking pitches).
- Particle effects: the waterfalls' mist, the volcano's smoke, the fountain's spray, whales'
  spouts (`Effect/Static/effect.carc`).
- Bloom and depth of field (`WS2_bic_F0_Bloom.pblm`, `_dof.pdof`), the caves' darker ambient
  (WSR fades the ambient colours inside 42 "shadow area" boxes, PMP group 5 type 3).
- Flags' cloth motion (SHP0) and every PAT0: flags stand at rest.
- Rigid multi-bone shapes (gulls, hawks, whales): a triangle that spans two bones (270 of them)
  is kept with the bone most of its corners use, so it is exact at rest and stretches a little
  wrong when the bones move.
- Whales stay where they are placed (WSR also moves them along routes); gulls and hawks circle
  instead of flocking; the airship takes the title sequence's flight.
- The ambient sounds (PMP group 5), the time of day.
- Model nodes whose batches aren't one run in the model's draw order (10: the sea's, and Wedge
  Island's sea rims, each with opaque and translucent batches split by the island's) have
  `n_batches` 0: renderers find a node's batches by `Batch.node`.

## How it was checked

- `ng3d.py selftest`: 23 structs (fields, types, array sizes), 23 static_asserts, 25 section tags
  and 13 enums against `engine/g3d/ng3d_format.h` (changing a field's type or a tag in a copy of
  the header is caught).
- `ng3d.py check`: the unbaked package (every reference in range, every payload inside its section
  and of the size its levels or layout take, key frames in order, marker kinds; read and written
  again, the same 12,339,440 bytes) and the baked one (the same records, laid out by ng3d-bake).
  Two staging runs give byte-identical files.
- The engine's loader and world on the baked package (`NOMA_SELFTEST=g3d` with
  `NOMA_G3D_PACKAGE`, host): loaded and checked in 0.2 ms with no texture converted (unbaked:
  37 ms converting 92), k = 10, 140 checks passed. Through the view cameras (a 400:240 frustum,
  near 1.5, far 80,000; frame 600):

  | view | cells | instances | draws (opaque + translucent) | triangles |
  |---|---|---|---|---|
  | town | 19/120 | 25/95 | 256 + 86 | 81,306 |
  | coast | 33 | 27 | 258 + 95 | 70,128 |
  | beach | 16 | 17 | 103 + 28 | 24,854 |
  | interior | 10 | 26 | 149 + 39 | 36,588 |
  | distant | 43 | 27 | 109 + 16 | 31,095 (stage 1: 37,814) |
  | overview | 36 | 34 | 327 + 115 | 84,936 |
  | wedge | 47 | 25 | 129 + 19 | 44,509 |

- The package rendered by `tools/g3d/preview.py` from those cameras at 1280x720 against stage 1's
  package (PSNR in dB, mean difference in levels, share of pixels more than 16 levels off): the
  differences are the environment's, as they should be -- the sky deep blue instead of fogged pale,
  the near sea deep blue instead of fog 1's #66d1ff, the light from the south-south-west, the sun
  where it shows -- and, far off, Wedge Island's light model; the volcano's lava chamber, lit by
  nothing that changed, is the same.

  | view | PSNR | mean | > 16 levels |
  |---|---|---|---|
  | town | 16.33 | 19.6 | 37.0 % |
  | coast | 17.07 | 19.8 | 44.6 % |
  | beach | 17.22 | 17.8 | 40.9 % |
  | interior | 56.42 | 0.0 | 0.0 % |
  | distant | 13.42 | 42.8 | 90.1 % |
  | overview | 17.90 | 24.6 | 81.7 % |
  | wedge | 12.58 | 48.3 | 93.0 % |

  Stage 1 compared its package's terrain with the research's renders of the BRRES: with the
  research renderer's conventions they matched to 37-52 dB (the rest CMPR's in-between colours on
  the sea's sun glint and the mip chain stopping at 8x8). The island's geometry, materials and
  textures haven't changed since, but for Wedge Island's far model and the sun.
