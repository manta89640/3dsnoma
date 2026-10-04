# Wuhu Island: the staged assets

What the walkable Wuhu Island is made of: which of Wii Sports Resort's files go into the island's
package, what each is for, how it is changed on the way, and what is left out and why. The
package is made from your own copy of the game by `tools/wuhu/stage_wuhu.py`; nothing of the
game is in this repository (docs/wuhu_design.md has the design; this file is its asset side).
Numbers are measured on the PAL disc (SP2P01) unless marked est.

## Staging

    tools/wuhu/stage_wuhu.py DATA [--wsr ROOT] [--bake TOOL]

- `ROOT` is the extraction of your Wii Sports Resort disc (its `files/` is the disc's root);
  default `../wii-sports-extract/wii-sports-resort` beside this repository.
- `DATA` is the port's data directory (`tools/stage_data.py`'s, `sdmc:/3ds/noma` on the 3DS). The
  package goes to `DATA/wuhu/island.ng3d` (12,155,376 bytes), and `DATA/wuhu/island.json` records
  what went in: classes, counts, decisions, sizes, the checks below.
- `--bake TOOL` runs `TOOL DATA/wuhu/island.raw.ng3d DATA/wuhu/island.ng3d` after writing: the
  renderer's offline converter of the GX texels to PICA ones. Without it, the textures stay GX
  texels, which the loader converts.
- It takes about 4 s on a Mac and reads only the archives it needs (Yaz0 U8 `.carc`, unpacked in
  memory). Before writing it checks every reference in the package and that the package reads
  back to the same bytes; it stops if either fails.

The tools: `tools/g3d/brres.py` reads BRRES (models, textures, CHR0/SRT0/CLR0 keyframes, SCN0
light sets), `tools/g3d/gxtex.py` decodes GX textures, `tools/g3d/convert.py` turns BRRES
objects into package records, `tools/g3d/ng3d.py` writes and reads packages (`ng3d.py selftest`
compares its structs with `engine/g3d/ng3d_format.h`; `ng3d.py check FILE` validates a package
and round-trips it), and `tools/g3d/preview.py` renders a package to PNG.
`tools/wuhu/check_views.py` renders a package from its `view_*` cameras and compares the pictures
with references.

## The package

| section | records | bytes | what |
|---|---|---|---|
| TDAT | 90 textures | 4,211,328 | GX texels with their mip levels (TEXS: the records) |
| VDAT | 350 vertex buffers | 3,887,168 | 167,227 vertices, 20-36 bytes each (VTXB: the records) |
| BLOB | 5 KCL files | 2,961,008 | the island's collision, as shipped (COLL: the records) |
| IDAT | 419,628 indices | 839,256 | u16 triangle lists |
| KEYS | 9,816 keys | 117,792 | animation keys (TRAK: 467 tracks, ANIM: 20 animations) |
| MATS | 88 materials | 47,520 | GX register images (TMTX: 125 texture matrices) |
| BTCH | 567 batches | 22,680 | draws: a material, a vertex buffer, an index range |
| CYLS | 479 cylinders | 9,580 | tree trunks |
| the other 17 | | 58,382 | strings, texture/vertex-buffer/collision records, texture matrices, nodes, models, cells and their batch lists, instances, animations and tracks, environment, lights, light sets, fogs, markers |

The header, the section table and alignment take the other 662 bytes.

At run time (est., from the package): textures 5.14 MB once in their PICA formats (all levels),
geometry 4.73 MB, collision 2.96 MB, everything resident.

## Sources

Paths are inside the extraction's `files/`; an archive's members after the colon.

| asset | source | role | staged as |
|---|---|---|---|
| `WS2_common_island` (9 models, 72 textures, island and sea animations) | `Stage/Static/StageArc.carc:Island/G3D/WS2_common_island.brres` (7,236,352 B) | the island, sea, sky | cells (island, island_V), models (sea, sky), animations |
| `sea2_day` | `StageArc.carc:Island/G3D/WS2_common_seatex.brres` | the sea's day colour (WSR swaps it for the placeholder `sea2_dummy` at run time) | a texture |
| placed objects | `Stage/PlnScene/PlnEnv0.carc:WS2_pln_island_tag0.pmp` (1,228 records) | Island Flyover's day placement: the complete set (research placement.md 0) | baked props, instances |
| the objects' models | `StageArc.carc:<Class>/G3D/*.brres` (table below) | trees, boats, cars, ... | baked props, models |
| collision | `StageArc.carc:Island/WS2_common_island1..5.kcl` (2.96 MB, 42,934 triangles) | the ground and walls | BLOB as shipped |
| tree trunks | `Common/ArcScene/common.carc:WS2_common_tree{,2,3}.kcl` (trunk triangles, attribute 0x1105) | trunk size per tree class | cylinders |
| lights | `PlnEnv0.carc:WS2_pln_F0/WS2_pln_F0_Light.plight` | the day's lights and ambient colours | LITE, LSET |
| light sets | `Common/Static/common.carc:G3D/WS2_Scene.brres` (SCN0) | which lights and ambient each material light set uses | LSET |
| fog 0 | `StageArc.carc:Binary/WS2_common_binary/ws2_common_day.pfobj` | the island's day fog (WSR writes it into fog 0) | FOGS[0] |
| fog 1 | `PlnEnv0.carc:WS2_pln_F0/WS2_pln_F0_fog.pfog` entry 1 | the sea's far materials' fog | FOGS[1] |
| landmarks | `Stage/Shared/Activity.carc:LandMarkData/LandMarkData.exbin` | the Flyover's 80 named places | markers |
| camera shots | `Stage/Shared/SeqData.carc:Map/WS2_seq1_IslandCamParam.exbin` | the title sequence's 15 ground-level shots | markers |
| spectator body | `StageArc.carc:Audience/G3D/WS2_common_avaC0.brres` | the Mii scale k | ENVR |

The island's materials use light set 0 and fog 0; props light set 1; flags light set 6; the sea's
far materials fog 1. No island material references `lm_0`/`lm_1` (character light maps).

## The island at Island Flyover's visibility

WSR shows the island model's nodes per mode from a table in main.dol (0x8066A37C, research
wsrdol.md 2). Island Flyover in daylight (mode 13) is the only mode that shows everything at full
detail, so it is the canonical set:

- **Shown (33 nodes, 59,635 triangles):** Island_1-15 models (the main island, the islets), their
  Kiwa edge strips, Island_16_Model (Wedge Island, the golf island, at full detail), Island_17c
  (the harbour pier), Island_18 (rope barriers), Island_19b (Mt. Wuhu with its cave and lava
  chamber), Island_20b (the underwater cave), Island_21 (the round stage), Island_3b/3c/8b (court,
  town buildings, hotel).
- **Hidden:** Island_16b (the golf island's light model), 17 and 17b (pier variants), 19 and 20
  (mountain and coast without caves), 19c and 20c (their night versions), Island_22 (a villa,
  hidden in every mode), Water16b (the light golf island's sea rim), Light1-4 (the night lights).
- `WS2_common_island_V` (198 triangles, a translucent blend over the volcano's inside) goes into
  the cells with the island.

## Cells and detail levels

The island and the props that stand still are one static world cut into cells of 4,000 units on
x/z: a 12 x 10 grid from (-36,000, -16,000), 44 cells with geometry (Cell index = z * 12 + x).
Terrain triangles go to the cell of their centroid; a placed object goes whole to the cell of its
position. A cell has a batch per material (per detail level for trees), its vertex buffers
deduplicated and split before 65,536 vertices (the largest has 5,684). A cell's `group` is the
island node with most of its triangles (`Island_16_Model` for Wedge Island's cells).

Each cell lists its batches for three levels, by the eye's distance to the cell's box:

| level | from | what | whole world |
|---|---|---|---|
| 0 | 0 | terrain, trees at full detail, every prop | 482 batches, 120,243 triangles |
| 1 | 3,000 | terrain, the trees' lighter models, palms, boats, fountain, the two gates | 453, 94,596 |
| 2 | 20,000 | terrain | 367, 59,833 |

Why these distances: on the 3DS's 240-line top screen with a 45-degree view, a 230-unit tree is
about 22 pixels tall at 3,000 units, where `WS2_common_tree_H` and `_M` look alike, and a 30-unit
car or parasol is 3 pixels; at 20,000 units a tree is 3 pixels and only Wedge Island and the far
islets are that far from anywhere on the main island. Through the six reference cameras with a
45-degree, 5:3 frustum (est., cell boxes only): 7-27 cells, 82-348 batches and 15,038-67,496
triangles a frame (town 279 batches, 66,497 triangles; overview 348, 67,496).

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
sequence's flight is used instead.

Not in this placement file, so not staged: the seaplanes (`PlnSub.carc`'s Flyover course and
`StageArc.carc:SeaPlane/WS2_common_pln_path.pmp`'s routes), hermit crabs (title sequence), and the
night's torches, campfire and fireworks (`WS2_pln_island_tag2.pmp`).

## Animations

| animation | frames | from | drives |
|---|---|---|---|
| `world_srt` | 2,400 | SRT0 `WS2_common_island` (lava), SRT0 `WS2_common_fountain` (300, repeated 8 times) | texture matrices of the cells' materials |
| `world_clr` | 200 | CLR0 `WS2_common_island` (lava, the volcano's inside), CLR0 `WS2_common_island_V` | colour registers |
| `sea_srt` | 20,000 | SRT0 `WS2_common_sea_P` and `WS2_common_sea_nami` (1,000, repeated 20 times): the Flyover's set (main.dol 0x806BE698) | the sea's texture matrices |
| `sea_clr` | 1,000 | CLR0 `WS2_common_sea_nami` | the waves' material alpha |
| `sky_srt` | 9,999 | SRT0 `WS2_common_vr` | the clouds' scroll |
| 15 node animations | 299-111,848 | the objects' CHR0s (above) | instances |

SRT0 keys are nw4r's Hermite keys (frame, value, slope); CLR0 colours (a value a frame) become
linear keys per component, collinear ones dropped, and a component whose mask byte is set (the
material keeps its own) gets no track. An animation made of several (lava and fountain, sea and
waves) runs to their common length, the shorter ones' keys repeated. A material animation sets all
five components of a texture matrix it touches (nw4r does), so constants are tracks too.

## Materials and textures

A material is GX's register image as its display lists leave it: TEV colour and alpha words,
TREF, KSEL, the colour and konst registers, alpha compare, depth mode, blend, constant alpha,
colour channel 0 (control words and colours), texture maps, texgens and one texture matrix per
texgen (nw4r's SRT and matrix mode, map mode, effect matrix), light set, fog, cull, translucency.
The display lists write some registers in masked halves (BP 0xFE): a TEV KSEL word gets its
swap-table bits and its konst selections in two writes, so the reader runs the lists through a
register file instead of taking the first write. (The research renderer took the first write, so
its pictures have the swap-table bits as konst selections: wrong on the sea and the lava.)

- 88 materials. 6 have indirect stages (the lava `ef_volSurface`, the sea's `mt_za_water` and the
  river's `mt_zc_w_nagare` with their `_off` twins, the fountain's water), flagged and dropped as
  the engine drops them: the warp is lost, the scrolls stay. No material reads texture
  coordinates past tex1, or colour channel 1.
- The blend word's dither, colour-update and alpha-update bits are written by nw4r through a mask
  that leaves them to the global state, where colour update is on: the package has that bit set.
- Texgen `matrix` is the texgen's own texture matrix when nw4r applies one (its SRT is in use, or
  it maps the environment or a projection); an environment-mapping texgen (normalised) has it as
  its post matrix too.
- 90 textures, 4,209,440 bytes of GX texels with every level down to 8x8 (the PICA's smallest).
  Each records the PICA format the loader converts it to: CMPR as ETC1 (37 textures, 2.61 MB) or,
  when a block uses CMPR's transparent texel, ETC1A4 (14, 1.14 MB once converted), I8 as L8 (13),
  IA8 as LA8 (11), IA4 as LA4 (2), RGB565 as RGB565 (4), RGB5A3 and RGBA8 as RGBA8 (9, 0.72 MB
  once converted): 5.14 MB in all once converted. A texture is never shared by name across files
  (56 names have different pictures in different WSR files).

## Environment

- **Lights** (from `WS2_pln_F0_Light.plight`, intensity multiplied in): Map_Light, the sun, white
  at (10,000, 48,000, 60,000); Obj_Light #736e5a at (1,000, 100,000, 25,000); and the other sets'
  lights. EGG's type-0 lights are positions in the world (the sun 77,000 units off, unattenuated)
  and are kept as point lights; every light also carries the direction toward its aim point (the
  origin), for a renderer that only does directional lights. Across the island the sun's
  direction varies up to about 25 degrees (Wedge Island against the origin).
- **Light sets** (SCN0 `WS2_Scene`, a material's light set number is its index): 0 Map (Map_Light,
  ambient #788287: the island), 1 Obj (Map_Light and Obj_Light, ambient #4d5173: props), 2
  Abatar, 3 SWF stadium, 4 JSK sea, 5 player object, 6 Flag (Flag_Light, ambient #bec8d2), 7 canoe
  sea, 8 Obj2, 9 cycling fence. Lights switched off in the day file are left out of their sets.
- **Fogs:** 0 linear 27,978-65,527 #83d5ff (`ws2_common_day.pfobj`: the island); 1 linear
  2,000-30,000 #66d1ff (Pln F0's entry 1: the sea's far materials, which turn light blue with
  distance as in the Flyover).
- **k = 9.200** Wii no Ma Mii units per island unit. Both games hang the same RFL head from a body
  node: Wii no Ma's Mii body (`g3d/Mii/Model.lex`, Female_Body and Male_Body) has its Head node at
  70 + 38.9 = 108.9 in a Mii 158 tall (height scale 1); WSR's spectator body has `head_set` at
  9.86 (its bind matrix) times the spectators' placement scale 1.2 = 11.84 island units. k =
  108.9 / 11.84: a 158-tall Mii is 17.2 island units, about 1.5 m at the research's estimate of
  9 cm a unit. (The spectator model's bounding box alone, 2.56-10.92, is a body without its head.)
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
  `view_overview` (the research's reference views: their vertical fields of view are 55, 55, 55,
  65, 40, 50 degrees, in `stage_wuhu.py` VIEWS).

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
- The water reflection (`WS2_common_island_R`, drawn mirrored by WSR; usable as a far LOD later),
  `WS2_common_under` (a navy skirt under the sea drawn with depth test off before the island),
  the night models (`island_L`, `vr_N`) and the evening sky (`vr_E`): daytime only.
- The sun (`Sun/G3D/*`: a billboarded disc, its shine and lens flare) and billboards in general.
- Particle effects: the waterfalls' mist, the volcano's smoke, the fountain's spray, whales'
  spouts (`Effect/Static/effect.carc`).
- Bloom and depth of field (`WS2_pln_F0_Bloom.pblm`, `_dof.pdof`), the caves' darker ambient
  (WSR fades the ambient colours inside 42 "shadow area" boxes, PMP group 5 type 3).
- Flags' cloth motion (SHP0) and every PAT0: flags stand at rest.
- Rigid multi-bone shapes (gulls, hawks, whales): a triangle that spans two bones (270 of them)
  is kept with the bone most of its corners use, so it is exact at rest and stretches a little
  wrong when the bones move.
- Whales stay where they are placed (WSR also moves them along routes); gulls and hawks circle
  instead of flocking; the airship takes the title sequence's flight.
- The ambient sounds (PMP group 5), the time of day.
- Model nodes whose batches aren't one run in the model's draw order (7 of the sea's: each has
  opaque and translucent batches) have `n_batches` 0: renderers find a node's batches by
  `Batch.node`.

## How it was checked

- `ng3d.py selftest`: 23 structs (fields, types, array sizes), 23 static_asserts, 25 section tags
  and 12 enums against `engine/g3d/ng3d_format.h` (changing a field's type or a tag in a copy of
  the header is caught).
- `ng3d.py check DATA/wuhu/island.ng3d`: every reference in range; read and written again, the
  same 12,155,376 bytes.
- The package rendered by `tools/g3d/preview.py` from the six `view_*` cameras at 1280x720,
  compared with the research's renders of the BRRES (`tools/wuhu/check_views.py`; PSNR in dB, and
  the share of pixels more than 16 levels off):

  | view | full package | terrain only | terrain, research conventions, vs research with KSEL fixed |
  |---|---|---|---|
  | town | 18.19 / 19.95% | 26.60 / 8.01% | 39.18 / 0.78% |
  | coast | 23.73 / 15.20% | 23.81 / 15.13% | 42.49 / 0.22% |
  | beach | 22.14 / 19.78% | 22.91 / 19.53% | 52.10 / 0.01% |
  | interior | 22.59 / 31.30% | 22.70 / 31.25% | 52.18 / 0.00% |
  | distant | 16.92 / 65.21% | 17.00 / 65.21% | 37.27 / 1.40% |
  | overview | 20.60 / 61.18% | 21.09 / 60.46% | 39.45 / 0.43% |

  The full package differs from the research renders as it should: trees, props, boats, birds and
  balloons are there now. Terrain only (cell level 2, no instances) still differs by the KSEL fix
  (the sea and the lava; terrain with the research's conventions against the original research
  renders: town 38.69, interior 20.98, distant 24.05, overview 23.72 dB), the material animations
  at frame 0 (the lava's colours, the sea's scroll scale), fog 1 on the sea's far materials (the
  research used fog 0 for all), the sky following the eye (the research left it at the origin)
  and the sun as a point light. With the research renderer's conventions (no material animation,
  the sky at the origin, fog 0 everywhere, the sun as a directional light from the origin) and
  the research renderer patched only for the KSEL masking, the package's pictures match to
  37-52 dB; what remains is CMPR's in-between colours (GX's 5:3 weights here, as the port's
  texture decoder has them, DXT1's 1/3 there) on the sea's sun glint, and the mip chain stopping
  at 8x8 near the horizon.
