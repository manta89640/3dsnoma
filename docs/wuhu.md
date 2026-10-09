# Wuhu Island: the staged assets

What the walkable Wuhu Island is made of: which of Wii Sports Resort's files go into the island's
package, what each is for, how it is changed on the way, and what is left out and why. The
package is made from your own copy of the game by `tools/wuhu/stage_wuhu.py`; nothing of the
game is in this repository (docs/wuhu_design.md has the design; this file is its asset side).
Numbers are measured on the PAL disc (SP2P01) unless marked est.

## Staging

    cmake -S . -B build-host -G Ninja && cmake --build build-host --target ng3d-bake ng3d-pvs
    tools/wuhu/stage_wuhu.py DATA [--wsr ROOT] [--bake TOOL | --no-bake] [--pvs TOOL | --no-pvs]
                             [--keep-raw] [--name NAME]

- `ROOT` is the extraction of your Wii Sports Resort disc (its `files/` is the disc's root);
  default `../wii-sports-extract/wii-sports-resort` beside this repository.
- `DATA` is the port's data directory (`tools/stage_data.py`'s, `sdmc:/3ds/noma` on the 3DS). The
  package goes to `DATA/wuhu/NAME.ng3d` (NAME `island` unless `--name` says otherwise; 12,284,032
  bytes), and `DATA/wuhu/NAME.json` records what went in: classes, counts, decisions, sizes, the
  checks below. The package is NG3D version 2 (`engine/g3d/ng3d_format.h`): an engine that reads
  version 1 refuses it, and the engine reads version 2 only (`DATA/wuhu/island.ng3d`; a version 1
  file there, 13,266,560 bytes, is refused with the reason: stage the island again). The speed
  round staged it as `island_v2` first; staged as `island`, the bytes are the same.
- The package is baked: the stager writes it with GX texels to `DATA/wuhu/NAME.unbaked.ng3d`
  (11,356,912 bytes), runs the renderer's offline converter on it (`TOOL UNBAKED OUT`; TOOL is
  `build-host/ng3d-bake` unless `--bake` names another), checks that no texture is left in GX's
  format, and deletes the unbaked file unless `--keep-raw` (keep it for `tools/g3d/preview.py`,
  which reads GX texels). Without the tool it stops and says how to build it: the 3DS refuses an
  unbaked package, because converting GX texels at load time on the CPU thread would race the GX
  path's texture uploads, which share `pica_tex.h`'s tables, on the GPU thread. `--no-bake` writes
  an unbaked package anyway (the host converts as it loads, in about 37 ms).
- The cells' visibility rows come from `build-host/ng3d-pvs` (`tools/ng3d_pvs.cpp`, the host build;
  `--pvs` names another, `--no-pvs` stages without the rows): the stager writes its input (the
  cells' triangles, their boxes and detail distances, the eye points) to a temporary directory
  (`DATA/wuhu/NAME.pvs/` with `--keep-raw`) and reads the rows back.
- `--no-prelit` and `--no-pos16` stage a package with the lighting left live or the positions f32,
  for comparisons.
- It takes about 16 s on a Mac (7 of them the visibility rows, on all cores) and reads only the
  archives it needs (Yaz0 U8 `.carc`, unpacked in memory); two runs give the same bytes. Before
  writing it checks every reference in the package, that the package reads back to the same bytes,
  and that every warp spot has room for the Mii (below); it stops if any fails.
- After the package it stages the island's sound effects beside it: `DATA/wuhu/sounds.nsfx` and
  `DATA/wuhu/sounds.json` ("Sounds" below; under a second). `tools/wuhu/stage_sounds.py DATA [--wsr
  ROOT] [--wav DIR]` stages them alone (`--wav`: each sound as a .wav to audition).

The tools: `tools/g3d/brres.py` reads BRRES (models, textures, CHR0/SRT0/CLR0 keyframes, SCN0
light sets), `tools/g3d/gxtex.py` decodes GX textures, `tools/g3d/convert.py` turns BRRES
objects into package records (a model can be built from parts of several: the sky with the sun in
it; static geometry is gathered into the cells' batches; positions are snapped and compacted,
lighting baked), `tools/g3d/ng3d.py` writes and reads packages (`ng3d.py selftest` compares its
structs with `engine/g3d/ng3d_format.h`; `ng3d.py check FILE` validates a package and round-trips
it), `tools/ng3d_pvs.cpp` computes the cells' visibility rows, and `tools/g3d/preview.py` renders
an unbaked package to PNG (`--per-vertex` lights as GX does, at the vertices).
`tools/wuhu/check_views.py` renders a package from its `view_*` cameras and compares the pictures
with references; `tools/g3d/viewstats.py` counts what a frame submits; `tools/wuhu/check_cvis.py`
checks the visibility rows by pictures.

## The package

The baked file's sections (version 1's in brackets):

| section | records | bytes | what |
|---|---|---|---|
| TDAT | 92 textures | 5,158,912 | PICA texels with their mip levels (TEXS: the records); 4,229,920 bytes as GX texels unbaked |
| VDAT | 340 vertex buffers | 2,947,008 (4,012,112) | 172,046 vertices, 12-28 bytes each, 125,715 of them 16 (VTXB: the records) |
| BLOB | 5 KCL files | 2,961,008 | the island's collision, as shipped (COLL: the records) |
| IDAT | 466,851 indices | 933,702 (863,706) | u16 triangle lists (a cell's levels share vertex buffers and repeat some indices) |
| KEYS | 9,870 keys | 118,440 | animation keys (TRAK: 477 tracks, ANIM: 20 animations) |
| MATS | 110 materials | 59,400 | GX register images (TMTX: 148 texture matrices); island_R's 20 added |
| BTCH | 652 batches | 26,080 | draws: a material, a vertex buffer, an index range, detail flag |
| NODE | 46 nodes | 4,600 | the models' node trees (11 models) |
| CYLS | 479 cylinders | 9,580 | tree trunks |
| MARK | 139 markers | 4,448 | spawn, landmarks, camera presets, warps |
| CVIS | 120 rows | 1,800 | the cells' visibility rows (15 bytes a cell) |
| the other 15 | | 44,316 | strings, texture/vertex-buffer/collision records, texture matrices, models, cells and their batch lists, instances, animations and tracks, environment, lights, light sets, fogs |

The header, the section table and alignment take the rest: 12,284,032 bytes against version 1's
13,266,560.

At run time (est., from the package): textures 5.16 MB in their PICA formats (all levels; the
loader counts 5,013 KB), geometry 3.88 MB (4.88 in version 1), collision 2.96 MB, everything
resident.

## Sources

Paths are inside the extraction's `files/`; an archive's members after the colon.

| asset | source | role | staged as |
|---|---|---|---|
| `WS2_common_island` (9 models, 72 textures, island and sea animations) | `Stage/Static/StageArc.carc:Island/G3D/WS2_common_island.brres` (7,236,352 B) | the island, its far copy, sea, sky | cells (island, island_V, island_R, the sea by the shores), models (open sea, sky), animations |
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
| effect pictures | `Effect/{Jsk,Swf,Glf,Can}Scene/effect.carc:WS2_effect_*.breft` (16 textures) | splashes, rings and spray of a Mii in the water | the water effects' atlas (WAFX) |

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
  town buildings, hotel). All go into the cells' level 0.
- **Hidden:** 17 and 17b (pier variants), 19 and 20 (mountain and coast without caves), 19c and 20c
  (their night versions), Island_22 (a villa, hidden in every mode), Light1-4 (the night lights).
  Island_16b and Water16b (the golf island's light model and its sea rim) are hidden in mode 13;
  the far island (island_R, below) has its own light Wedge Island.
- `WS2_common_island_V` (198 triangles, a translucent blend over the volcano's inside) goes into
  the cells with the island.

### Wedge Island far away

WSR's ground-level modes (1-4, 6-10, 15-16) show the golf island's light model, Island_16b, with
its own sea rim, Water16b, instead of Island_16 and Water16. From the main island the two can't be
told apart, even through a 6x zoom (research env_choice/env.md, its quay renders). Wedge Island is
in the cells like the rest of the island (version 1 staged it as a still instance with a model
chain, culled whole by one sphere of radius 6,640): Island_16 and Water16 at level 0, and at the
far levels island_R's light model of it (`Island_16b_model`, 1,658 triangles, below) with
Water16's pond. Its trees and props are in its cells too. Through a 6x zoom from the west headland
the far version has no grass edges over its cliffs (island_R has no blend strips); at the views'
own fields of view it is a few pixels tall.

## Cells and detail levels

The island and the props that stand still are one static world cut into cells of 4,000 units on
x/z: a 12 x 10 grid from (-36,000, -16,000), 44 cells with geometry (Cell index = z * 12 + x).
Terrain triangles go to the cell of their centroid; a placed object goes whole to the cell of its
position. A cell has a vertex buffer per vertex layout and, in each level's list, one batch per
material (and detail flag): the pieces of a material in a cell share one encoding and one vertex
buffer, and two levels drawing the same pieces share a batch. The slivers a centroid cut leaves
where a material crosses a cell's edge (under 32 triangles) go to the neighbour that draws the
rest of it when they lie within its box (26 moved): its box, which picks its level and culls it,
stays as it was. A cell's `group` is the island node with most of its triangles.

Each cell lists its batches for three levels, by the eye's distance to the cell's box (a cell with
water alone has one list for all three):

| level | from | what | whole world |
|---|---|---|---|
| 0 | 0 | the island at full detail (Wedge Island too) with its edge-blend strips, island_V, the trees' full models, static props, the sea by the shores and inland (shore foam, waves, ripples, the lake, the falls, the cave's water) | 460 batches, 112,347 triangles |
| 1 | 5,000 | the far island (island_R), the lake, the falls, the sea by the shores, the trees' lighter models, the boats at anchor, the venues' gates | 232, 52,550 |
| 2 | 15,000 | the far island and its waters, with the tree L models | 176 batches, 20,008 triangles |

(Version 1: 0 / 3,000 / 20,000, the island at full detail at every level, trees at 0-1: 435 /
406 / 320 batches, 110,267 / 84,620 / 49,857 triangles.)

**The far island** is `WS2_common_island_R`, WSR's own low-detail copy of the island, which it
draws mirrored as the island's reflection in the water: the same nodes and frame, 15,293
triangles for the whole archipelago (the full island 59,635), one TEV stage a material, the
island's own textures (no texture memory added), no edge strips, Wedge Island as its light model.
Three of its nodes are left out and the island's own taken instead at every level: the islets
Island_12-14, which in R have no grass (22-28 triangles against 31-59). Two of the island's nodes
that R lacks are added to the far levels: the round stage of Swordplay on the water (Island_21,
1,568 triangles) and the court by the lighthouse (Island_3b, 554). R folds the town's and the
hotel's buildings into its Island_3 and Island_8. Its mountain is the cave-less version
(Island_19, closed), so the cave's water and island_V's glow stay at level 0.

**Why these distances.** On the 3DS's 240-line top screen at 45 degrees a pixel is 1/290 of the
distance: a 230-unit conifer is 13 pixels tall at 5,000 units and 4 at 15,000, a 160-unit boat 3
at 15,000. At 5,000 the far island switches in; the step (each of the 8 cells with the most
level-0 triangles seen from just past 5,000 units of its box, on the ground, the cell alone at
level 0 and at level 1, 800x450) is 34-64 dB, at most 1.4 % of the pixels more than 16 levels
off: the grass edges along cliff tops and the cliffs' shading. Tree silhouettes switch to their
five- and six-triangle source meshes at level 2. The speed round's walking frames had 36 % of their
vertices 4,000-12,000 units off.

**Detail** (`BATCH_DETAIL`, culled by the batch's own box beyond `Env::detail_distance`, 3,000
units): the edge-blend strips (`mt_gake_blend`, `mt_kusa_blend`, `mt_suna_blend`, `mt_tuti_blend`),
the decals (`mt_stage_logo`, `png_net_b`), the sea's shore foam, waves and ripples
(`mt_zd_w_kiwa` and its `_off`, `mt_ze_w_nami`, `mt_ze_w_nami2`, `mt_zb_w_hamon`), and the small
props (parasols, tents, kayaks, flags, poles, tees, buoys, the venues' small props): 144 batches.
At 2,000 the shore foam across a bay goes visibly (a walking frame by the volcano,
28.9 dB against none culled); at 3,000 it stays (54.5 dB) for 2.6 more draws and 850 more
triangles a walking frame. Strips further off than that are on cells that switch to the far island
at 5,000, which has none.

## Placed objects

`WS2_pln_island_tag0.pmp`'s group-1 records as WSR's object manager spawns them for the Flyover's
1-player game (fn_8026D230 / fn_8026D514: filter 0, include bit 0, exclude mask 2): 699 objects.
Group 5 (130 ambient-sound boxes) and groups 3/7 (the Flyover's own game objects and markers) are
not objects anyone sees.

| class | placed | staged as | how |
|---|---|---|---|
| conifer | 159 | baked, three levels | `WS2_common_tree_H` / `_M` / `_L` (levels 0 / 1 / 2; 32 / 27 / 5 triangles) |
| broadleaf tree | 179 | baked, three levels | `WS2_common_tree2_H` / `_M` / `_L` (levels 0 / 1 / 2; 54 / 30 / 6 triangles) |
| palm | 141 | baked, levels 0-1 | `WS2_common_tree3` (it has one model) |
| cruiser at anchor | 28 | static instances | enter nearby with 3DS X (Wii Remote 1); steer on water with the circle pad: 170 units a second, and held ahead it gathers speed to 255 over 6 s (`engine/wuhu/vehicles.cpp`, the yachts alike) |
| yacht | 19 | static instances | enter nearby with 3DS X (Wii Remote 1); steer on water with the circle pad |
| fountain | 1 | baked, level 0 | its water's SRT0 in the world animation |
| jet-ski start gate, wakeboard goal gate | 1 + 1 | baked, levels 0-1 | 168 and 400 units wide |
| parked car | 32 | static instances | enter nearby with 3DS X (Wii Remote 1), drive with the circle pad; 385 triangles each |
| parasol | 17 | baked, level 0, detail | |
| kayak | 10 | baked, level 0, detail | |
| golf tee | 9 | baked, level 0, detail | |
| flag A | 3 | baked, level 0, detail | at rest: their SHP0 (cloth) is left out |
| frisbee-dog plate, archery target, table-tennis table, cycling gate | 4 | baked, level 0, detail | the venues' low-detail props |
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

Small instances are culled by distance (`Instance::cull_distance`, from the near side of their
bounding sphere): gulls and hawks (20 units, 377-385 triangles in 5 batches) beyond 2,000 units,
where they are 3 pixels across; moving cars (55 units) beyond 3,000; whales (600 units, 10
batches) and moving cruisers beyond 8,000 (22 and 6 pixels). Version 1 had gulls and hawks at
6,000, cars at 12,000, cruisers at 20,000, whales never. Through the speed round's 144
walking frames this takes the whales from 17.3 draws a frame to 1.4 and the birds from 18.8 to
4.8. The cars' and cruisers' spheres hold their whole routes (below), so a renderer that culls them
by where they are now gains most there: they are 19 draws and 6,700 triangles of a walking frame.

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

## The Mii's clips

The household's Mii plays the channel's own clips (engine/wuhu/island_mii.cpp has the table): the
gaits from Move (driven by the distance walked at each clip's measured planted-foot speed times the
Mii's height scale, one shared phase across gait changes, a run's flight stretched over 4.6 steps a
second), Wait by time with Wait_A..D fidgets, Turn_Simple turning on the spot. The stager also writes
`DATA/wuhu/mii_anim.brres` (tools/wuhu/mii_anims.py, `--noma` for Wii no Ma's content 26, by default
the content archive in DATA/content holding g3d/Mii/Model.lex): ten CHR0 clips on Wii no Ma's body,
29,812 bytes -- the jump (Send_DSi_Jump 12..47) and the landings (Male_Land, Female_Land) without
their own rise, a slide pose, WSR's swf_ava_fall and omk_ava_parachute moved onto the body, a pilot's
seat (pln_ava_wait) and a standing helm with jsk_ava_drivepose's arms as a 21-frame steering axis, and
a made front crawl and treading water. tools/g3d/chr0_write.py writes it (its dictionary trees come
out as Nintendo's do for all 588 entries of the channel's files; every node carries its translation
and the model node's segment scale compensation bits, without which nw4r scales the bones twice on a
Mii of height scale under 1). The island hands it to the actor manager as one more animation file,
"Island" (engine/ui/g3d.h anim_file_add), in place of Vote, Send_DSi and Sit_Sofa, which it loads
when the archive isn't staged: 5 animation files of 314,712 bytes then, against 472,576 without it
(and 319,488 before, with Seal). NOMA_WUHU_FEET=1 logs the planted foot's slide measured on the
posed body each second (NOMA_WUHU_FEET_TRACE=1 every node's place every frame).

In a vehicle the Mii stands at a boat's helm (Vehicles::seat: a cruiser's flybridge, a yacht's
cockpit) and isn't drawn in a car: WS2_common_car is closed and its windows opaque, so a seated Mii
was a whole guest model drawn unseen. Gliding, WSR's parachute (WS2_omk_parachute_01) hangs from the
Mii's shoulders at the Mii's size (its height over WSR's Mii's 15.24 units), its 30-frame clip (the
canopy opening) played once from the glide's start and held open (World::set_instance_anim_frame).

## Materials and textures

A material is GX's register image as its display lists leave it: TEV colour and alpha words,
TREF, KSEL, the colour and konst registers, alpha compare, depth mode, blend, constant alpha,
colour channel 0 (control words and colours), texture maps, texgens and one texture matrix per
texgen (nw4r's SRT and matrix mode, map mode, effect matrix), light set, fog, cull, translucency.
The display lists write some registers in masked halves (BP 0xFE): a TEV KSEL word gets its
swap-table bits and its konst selections in two writes, so the reader runs the lists through a
register file instead of taking the first write. (The research renderer took the first write, so
its pictures have the swap-table bits as konst selections: wrong on the sea and the lava.)

- 110 materials (island_R's 20 among them). 6 have indirect stages (the lava `ef_volSurface`, the
  sea's `mt_za_water` and the river's `mt_zc_w_nagare` with their `_off` twins, the fountain's
  water), flagged and dropped as the engine drops them: the warp is lost, the scrolls stay. No
  material reads texture coordinates past tex1, or colour channel 1.
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

## Vertices and lighting

Version 2 of the format makes a vertex cheaper to fetch and to shade:

- **Compact positions** (`VA_POS16`): every position is snapped to a grid of 1/12 unit (in the
  space it is stored in: the world for the cells, a node's for a model's meshes) and a vertex
  buffer whose positions lie within 32,767 steps of a grid point (5,461 units across) is written
  as s16 x3 and a pad, with that point as `pos_origin` and 1/12 as `pos_scale`: 8 bytes for 12.
  One step for every buffer, so a vertex two buffers share (neighbouring cells, two layouts of a
  cell) decodes to the same place in both and no crack opens between them; the snap moves a
  coordinate by 0.043 units at most (half a step, and float32's rounding at 30,000 units), a
  position by 0.072 (4 mm at the island's Mii scale). The widest cell's buffer is 5,208 units
  across, so every cell fits; the open sea and the sky (5 buffers, 308 vertices) stay f32.
- **Prelit materials** (`MAT_PRELIT`): a material lit by colour channel 0 whose material or
  ambient colour no animation drives has its lighting baked into its vertex colours: the light
  set's ambient and lights evaluated per vertex as GX does (and `tools/g3d/preview.py` with
  `--per-vertex`): colour = material source (the vertex colour, or the material colour) x clamp(
  ambient + the sum of N.L x light colour), the sun's point lights from their positions (the
  engine's World lights the package's point lights from the direction toward the origin, up to 6
  degrees off on the east beach, 23 on Wedge Island: the bake is GX's; in the island scene's frames
  from the follow camera at view_wedge and view_distant, the hotel's walls and a tree, baked against
  the same package lit live is 26 dB, 19-25 % of the pixels more than 16 levels off, where version 1
  against it is 38-41 dB), alpha the source's times
  the ambient's. Its channel words then say unlit with the vertex colour (colour and alpha), its
  light set is dropped. 90 of the 110 materials; lit at run time stay the waves (`mt_ze_w_nami`,
  `nami2`: their CLR0 drives the material alpha) and the windmills (`mt_Windmill`: a model's
  lighting is baked once, at its rest orientation, and the eight windmills stand 6-87 degrees apart
  about y, so six of them would be lit from the wrong side, 19-42 colour levels off on average and
  up to 122; their blades turn too; 96 vertices). The moving models are lit as they stand at rest,
  unplaced: their light turns with them (a balloon in the beach view is the one visible
  difference in the views; a car or cruiser heading the other way from its rest has its lit side
  away from the sun).
- **Only what a material reads**: normals only where a material needs them (lit at run time, or a
  texgen from the normal: the town's walls, the harbour's ship, `mt_DriveHead` on Wedge Island), vertex
  colours only where a channel reads them (none on the open sea). 125,715 of the 172,046 vertices
  are 16 bytes (position, colour, one UV), 44,225 20, 547 12, 1,559 28; version 1's were 20-36.

The speed round's measurements (a model of the PICA's vertex units from the shader's instruction
counts, not hardware) put its vertex work at the larger part of a frame's GPU time (the town view:
7.6 of 8.6 ms) and baked lighting alone at 7.6 -> 4.5 ms there; the renderer's version 2 shader
decides what it is on hardware.

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
  Only the open sea's materials, `mt_za_water` and `mt_za_water_off`, use fog 1 in WSR: the sea
  fades into the haze at the horizon. (Stage 1 had this the wrong way round, after the research's
  first reading: the `.pfobj` in fog 0, which washed out the sky 62,000-70,000 units off, and Pln
  F0's fog 1, 2,000-30,000 #66d1ff, which turned the near sea pale.) WSR rescales fog 1's start and
  end each frame by the camera's pitch (fn_80274A5C): at walking pitches under 15 degrees that is
  at most 3 %, left out. **Since version 2 no material is fogged** (`MAT_FOG` cleared; the fog
  table stays, and the format keeps fog): the PICA backend draws a fogged item a second time for
  its fog, and from the ground fog 1 only touches the sea a few lines under the horizon. From
  raised views it shows: the overview camera (3,500 units up) sees the sea plane's far edge as a
  band against the clear colour (27.5 dB against the fogged picture, 7 % of its pixels); the
  distant camera (1,100 up) 32.4 dB; the walking views 45 dB and more.
- **The sky** (`WS2_common_vr`) follows the eye (the engine draws it centred on the eye, first,
  without depth). Its materials are unfogged in the package whatever their fog index says (WSR's
  sky is fog 0, off by day; a fog 0 turned on later, a night's, must not reach the dome). The
  clear colour is fog 1's #83d5ff, so that the gap under the dome's lower edge (231 units under
  the eye) meets the sea in the haze's colour; the package has no clear colour of its own (the
  scene sets it from the first fog in the table that is on; `tools/g3d/preview.py` likewise).
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

## The sea

`tools/wuhu/stage_wuhu.py` (SEA_*, WAKES, TRAIL) and `engine/g3d/sea.h`; research sea/report.md.

- The flat sea is WSR's `mt_za_water` with three texture units: the camera-projected glare
  (`jog_water_light`), `jog_waterB` and `sea2_day` (for `sea2_dummy`), its texture matrices 2 and 3
  moved to the package's 1 and 2 so sea_P's scrolls drive them every frame (the 16-frame reflection
  atlas of tools/g3d/water.py is gone). The sea at its level (a corner at y 0, none above 20: the
  open sea and the ramps up the beaches) has a material of its own flagged `MAT_SEA`; the inland
  waters that share the material (lake, pool, streams, Wedge Island's rim) keep the plain one.
- Near the eye a wave grid (section `SEAW`): a log-polar grid of 2,257 vertices and 4,464 triangles,
  48 around, rings from 3 to 1,200 units, centred under the eye on an 8-unit lattice, drawn after the
  sky while the eye is under 600 units over the sea. Four waves of deep water's speed (300, 190, 120
  and 75 units long, 2.6/1.6/0.95/0.5 high, periods 4.2-2.1 s, within 45 degrees of one wind), each
  faded out from 1.5 to 3 of its wavelengths from the centre, flattened toward the shores by an
  amplitude map (375 x 300 texels of 128 units over the islands' box; none within 40 units of land,
  whole from 400; land the collision's triangles that aren't water or a fence reaching above y -3).
  Its material is the flat sea's plus two combiner stages: times its vertices' shade (doubled) and
  plus a sky colour times their brightening (the sun's side of a swell, the sky's Fresnel reflection,
  the sun's glint toward the visible sun, each relative to the flat surface's, so the rim is the flat
  sea's colour). It marks its pixels in the stencil buffer; `MAT_SEA` items aren't drawn there. On
  the 3DS its own vertex program (`platform/3ds/shaders/g3d_sea.v.pica`, 104 instructions, the phases
  folded on the CPU in double precision); on the host the soft backend does the same on the CPU.
- Boats ride the same waves on the CPU (`World::sea_height`): the driven one and the boats at anchor
  within 1,500 units of the eye, lifted, pitched and rolled by the swell under their hulls; a
  swimming Mii is drawn on the waves (its body stays on the collision's surface). A landmark out at
  sea (NOMA_WUHU_START=ホエール・ウオッチング) starts the Mii swimming.
- Wakes: WSR's `WS2_common_cruiser_ef` and `WS2_common_move_liner_ef` (SRT0 scroll, CLR0 frame 0)
  as parts of the moving cruisers' and the liner's models; the driven boat's as an instance of its
  own (class 0xFFFD, `MAT_RUNTIME`: its C0 alpha follows the boat's speed, scaled to its hull).
  Trails: every boat (`INST_TRAIL`) samples its path every 14 units; the nearest four with samples
  are drawn as one dynamic item first in the translucent pass, ribbons of the wake's foam streaks
  widening 7 units a second and fading over 5 s, on the waves.
- Costs: the grid 1 draw (+ two program switches on the 3DS), 2,257 vertices, ~9 KB of amplitudes a
  frame into a 64 KB ring; the trails 1 draw of at most 192 vertices; a moving cruiser's wake one
  translucent draw of 32 triangles. The CPU's part (host): the grid's amplitudes only when its centre
  moves (2,257 bilinear lookups), the trails' ribbons a frame (192 vertices on the waves), the moored
  boats' poses (5 height queries each). NOMA_G3D_STATS counts the wave grids and trails drawn; the
  island's frame line says `sea grid` and `trails`.

## Water effects

`tools/wuhu/stage_wuhu.py` (WATER_FX*, `stage_water_fx`), `tools/g3d/reft.py`, `convert.AtlasTexture`;
drawn by `engine/g3d/water_fx.h`.

- WSR has no swimming Mii: its splashes and ripples are particles of its effect library (emitters in
  each scene's `.breff`, textures in the `.breft` beside it, `Effect/<scene>/effect.carc`). The
  island draws them natively, as quads and patches the World makes each frame; the emitters' numbers
  are in `water_fx.cpp`, and only the pictures are staged.
- 16 pictures from four archives' REFT files (version 11: a texture's header is 0x40 bytes, its image
  after it). From Jsk: the spray's flipbook (`ws2f_waterSp00`-`15`, every third), a drop
  (`rk_mizutama1`), a foam puff (`ws2f_waterSand0`) and a foam disc (`rk_hamon2`). From Swf: the
  crown's wall (`ws2n_water01`), a big entry's rings (`ws2n_drop_wave`, 128 x 128) and bubbles
  (`ws2n_bubble01`). From Glf: a thin ring (`pc_glf_ripple_bl02`). From Can: the swimmer's bow
  spray (`ws2n_water_splash1`, `3`, `5`).
- One 256 x 256 IA8 atlas (`waterfx_atlas`), LA8 once baked, 3 levels, 172,032 bytes. It holds
  4 x 4 cells of 64 texels.
  - Each picture is resampled to 60 x 60 with a tent filter. IA8 pictures have their intensity
    weighted by alpha; I8 ones keep intensity = alpha, as GX reads them.
  - A 2-texel border round each picture repeats its edge texels. A picture is then sampled at its
    rim as a clamped texture is, so copies laid side by side meet without a seam (the crown's wall:
    `ws2n_water01` repeats down its rows).
  - Each level is box-filtered within its cell, so no neighbour's texels reach a picture. At the
    smallest level the border is half a texel.
  - The stager checks that the GX texels decode back to the pictures at every level.
- Two materials, `#waterfx_mul` and `#waterfx_erode`, made from the cruiser wake's `m_nami_ef` as
  the trails' is. They keep its blending and its depth test (less or equal, not written: an overlay).
  - One stage: colour K0 + (vertex - K0) x texture, WSR's blend between a particle's two colours. K0
    is `#CED0D3`, `WS2_jsk_pocha_splash`'s secondary colour.
  - Alpha is texture x vertex (multiply), or texture - vertex (erode: WSR's alpha by subtraction,
    the particle's alpha a threshold that eats the picture away).
  - Unlit, no fog, no culling, no texture matrix.
- Four dynamic batches in the trails' vertex layout (24 bytes): 64 quads and 16 patches of 3 x 3
  vertices for each material. They take 19,200 bytes of placeholder vertices and a box of +-64 units.
  A quad's or patch's indices refer only to its own vertices, so the first n of them draw whole.
- Section `WAFX`: one `WaterFx` of 292 bytes, holding the atlas, the four batches, their capacities
  and each cell's rectangle. The section is optional. A package staged before it still loads, and the
  island then draws no water effects. `island.json`'s `water_fx` lists each picture's archive,
  texture, format, size and cell.

## Sounds

`tools/wuhu/stage_sounds.py` (SOUNDS, GROUNDS), the bank's format `engine/audio/nsfx.h`; played by
`engine/audio/sfx.*` and `engine/wuhu/sounds.*`, told what happens by `engine/wuhu/island.cpp`.

- WSR has one sound archive, `Sound/Static/Sp2Snd.brsar` (59,383,776 bytes; RSAR 1.04: 1,610 sounds,
  21 banks, 141 files). Most of its effects are sequences (RSEQ) playing a note or two on an
  instrument of an effects bank; some are wave sounds (RWSD). The bank keeps the waves those notes
  play, the disc's own mono DSP-ADPCM as it is, with nw4r::snd's level and pitch for each note: the
  velocity squared, times the instrument's volume, the track's volume squared and the sound's archive
  volume (`tools/music/sndrender.py`'s model); 2^((key - original key) / 12) times the instrument's
  tune.
- A sequence is run 64 times (seeds 1-64) through `tools/music/brsar.py`'s model of nw4r's sequencer,
  with the player variables the game sets. Its random transposes give the variants: Showdown's
  sequence picks one of two recordings for each foot. Its random pitch bends and volumes give the
  spreads the engine draws at each play: a step varies up to a semitone either way and 1.6 dB.
- The bank: 50 sounds, 82 variants, 76 waves (45.7 s at their own rates, 16 to 32 kHz), 672,200
  bytes of ADPCM, 681,104 bytes in all. `sounds.json` lists each sound's WSR label, variables,
  variants (wave, level, pitch, length) and spreads.

| what | WSR's sound | |
|---|---|---|
| a foot down, walking or running | `SE_SWF_KUMITE_FOOTNOTE_WALK_L`, `_R`, `_RUN_L`, `_R` (Swordplay Showdown) | by the ground (below) |
| a jump, a landing | `SE_SWF_KUMITE_FOOTNOTE_JUMP`, `_LANDING` | by the ground; a landing at half to full level by how fast the Mii came down |
| into the water | `SE_BIC_LANDING_WATER` (a wheel in the shallows, two layers), `SE_BIC_FALL_WATER_ONE` (a cyclist falling in), `SE_SWF_PLAYER_FALL_WATER` (a Mii knocked off the duel stage into the sea) | by the splash's strength: under 0.45, under 0.9, over (a jump off a beach is about 0.65, the plaza's sea front 1.5); over a boat's side the first; walking in, Showdown's landing in water |
| a swimming hand going in | `SE_CAN_PADDLING_S` (Canoeing's light stroke, two layers) | at 0.4 of its level: four a second at speed |
| the parachute opening, the glide | `SE_OMK_PARACHUTE_L_OPEN_PLAYER` (Skydiving), `SE_PLN_WIND_FLYING` (Island Flyover; a loop) | |
| into and out of a car | `SE_OMK_FOOTNOTE_TO_HATCH`, `SE_OMK_FOOTNOTE_JUMP_OUT` (Skydiving: steps on the plane's floor and out of its hatch) | WSR has no car door |
| driving a car | `SE_CMN_ELEC_CAR_RUNNING_1` (the island's electric cars, a loop) | louder and up to a fifth higher with the speed and the throttle |
| into and out of a boat | Showdown's landing on wood (the deck); out onto land, the landing on the ground there | out into the water: the small splash |
| driving a boat | `SE_JSK_BOAT_ENGINE_IDLE`, `_LOW`, `_HIGH`, `SE_JSK_BOAT_RUN_WATER_LOW`, `_HIGH` (Wakeboarding's tow boat, loops) | the engine's three crossfaded by the speed, each higher with it; the hull's water rising with it |

- The grounds. Showdown's footsteps play program 8 + var1 of bank `SE_SWF_KUMITE`, and programs 8 to
  13 are six grounds: var1 is the KCL attribute's low byte less one, 0x1101 grass, 0x1102 dirt,
  0x1103 hard ground, 0x1104 sand, 0x1105 wood, 0x1106 water. The six programs match the attributes'
  first six; the hard ground's waves are the same recordings as the duel stage's (`SE_SWF`) and the
  basketball court's (`SE_BSK`); the water's are long splashes with no slide among them. The island
  plays the hard ground's for 0x1108 (structures) and anything else, and the water's where the water
  over the feet is deeper than 0.08 of the Mii's height (wading). Of the island's walkable ground by
  area: grass 1.2 %, sand 0.4 %, hard ground 0.4 %, dirt 0.1 %, wood and structures under 0.05 %
  (the sea's plane under it all is the other 97.9 %).
- When: the feet as the gait's clip puts them down (its strike frames passed: `IslandMii::footfalls`,
  as `strokes` for the hands), so a step is heard as the foot lands at any speed; a run's strides
  are capped at 4.6 a second (the clip's cadence), and so are its steps.
- The engine plays them natively (`engine/audio/sfx.h`): nw4r::snd can play only the sounds of the
  channel's own archive. 24 voices mixed into the AX main bus beside the music stream (through
  `stream_mix`, the AX frame's one call into the native mixing); each decodes its wave's ADPCM a frame
  of 14 samples at a time as it plays and resamples it to 32 kHz (linear); a play, a change and a
  stop are commands queued from the game's thread, so the mixer never waits for it. The bank is read
  on the island's load worker with the package (`island_load.h`).
- Checked against a reference in Python (the stager's decoder, the mixer's own fixed-point
  arithmetic): one-shots at their own pitch and at 1.37, and a loop through 2 s (its wraps by the
  loop's history), bit-identical to it; a level change's and a fade's steps smaller than the wave's
  own largest; 40 plays at once keep 24 voices; released while playing, silence the next frame.
- Levels: WSR's own, all times one level against the island's music. 「広い世界へ」 plays at a loud RMS
  (50 ms windows within 20 dB of the loudest) of -21.6 dBFS in the host's recording, WSR's streamed
  music at its archive's volumes at a median of -18.3 dBFS (42 pieces over 30 s): WSR's balance
  would be 3.3 dB down. A jump into the sea off the plaza's front (the large splash with strokes
  after it) went 8 dB over the music there and clipped: the effects are 6 dB down (the splash then
  at -15.5 dBFS against the music's -19, its peaks 1.7 dB under full scale), the steps 3 dB up from
  that (WSR keeps Showdown's well under its music), the strokes at 0.4. `NOMA_WUHU_SFX_DB` moves the
  whole by so many dB, `NOMA_WUHU_SFX_LOG=1` logs each sound as it plays.
- The loops follow the speed and the stick eased over a quarter of a second, and fade out while the
  island is frozen (the menu, a warp, the way out). A bank staged before this one, or none, leaves the
  island its music only (logged: `wuhu: sounds: none (...)`).

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
  `view_overview` (the research's reference views), `view_wedge` (research env_choice's
  quay_wedge: from the plaza's sea front at a walking eye's height toward Wedge Island, 27,000
  units off) and `view_windmill` (Wind Park, for the shadows). Their vertical fields of view (55, 55, 55, 65, 40, 50, 50 degrees) are in
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

## Cell visibility

The package carries a visibility row per cell (`CVIS`, `Env::pvs_row_bytes` = 15, bit j of row i
at byte j >> 3, bit j & 7): bit j is set when some triangle of cell j may show from an eye over
cell i's ground no higher than `Env::pvs_max_height` = 80 units over it (three heights of the
island's Mii, at k / 1.7: the follow camera on flat ground and a little over; a camera higher over
the ground under it uses no row). `tools/ng3d_pvs.cpp` renders cube maps (6 x 128 x 128, faces 94
degrees wide) at eye points: a column every 400 units over each cell, at every floor the collision
has there (ground or water, not the invisible fences, not a floor under something solid -- the sea
plane under the island), 2.5, 40 and 80 units over it and under whatever is above; 36,243 eyes,
7 s on all cores. Occluders are the cells' opaque, depth-writing triangles without alpha test, as
their materials cull them, but not their detail batches (gone beyond `Env::detail_distance`: the
kayaks and parasols), and the open sea; a cell occludes a pixel only as far as the farthest of
the detail levels its distance from anywhere in the eye's cell allows (its square of the grid, the
gaps between the eye columns and its edges included: a level switch can't uncover what the row
hides). The occluders' depth is eroded to the farthest of each 3x3 neighbourhood before a
cell's triangles (every level, translucent and cut-out ones too, a pixel each corner as well as the
pixels covered) are tested. Neighbouring cells always see each other; cells without geometry are
set.

What it hides is little, as the speed round's measurements found for cells this size: from a cell with
ground and geometry, 95.1 % of the cells with geometry may show on average (72.7 % at the least:
the northern islets, whose sea-level eyes have the main island's ridges between them and the
south-east). Through the view cameras and the 144 walking frames it removes nothing (the frames
look along the island, not past the volcano at sea level); it is there for the places it helps
and for a finer grid later.

Checked by `tools/wuhu/check_cvis.py`: renders (800x450, preview.py) from 20 random walkable
points, the follow camera at rest behind the Mii at three yaws, with and without the rows: 60 of
60 identical (the rows hide 1.6 % of the cells there); from 20 points in the cells whose rows hide
the most, looking toward what they hide: 60 of 60 identical (6.7 % hidden).

## Shadows

Wii Sports Resort has no shadow map and bakes no object's shadow into the island (research
shadow/report.md, from main.dol): each tree, parasol and venue prop has a 32x32 silhouette drawn as
alpha-blended geometry on the ground under it, and the cars and the spectators have silhouettes of
their own. The staged island has those, and on top of them a shadow map around the player. The user
asked for both, because baked shadows miss what moves (the windmills' blades, the boats, the cars, the
Mii). The shadow map is switchable: `NOMA_WUHU_SHADOWS=0` turns it off, and `NOMA_WUHU_SHADOW_MAP=N`
sets its size. On the 3DS it is off unless `NOMA_G3D_SHADOWS=1` (below: it has never come out right
there), so the decals and the Mii's blob are the island's shadows, as in WSR.

- **Decals** (`tools/g3d/shadow_decals.py`):
  - **Types and textures:** WSR's table at main.dol 0x8066F1C0 (size, offset, depth, straight down)
    by PMP class: parasol 21, tent 43, conifer 100, broadleaf 80, palm 28, archery target 35,
    table-tennis table 20, cycling gate 85. The silhouettes come from StageArc `Tree/`, `Parasol/`,
    `Tent/`, `IslandLowObj/`, `Car/` and `Audience/G3D/*_shadow.brres`, packed in one 128x128 I8 atlas
    with 3 levels (10 of its 16 cells used).
  - **Placement** (fn_802874B0):
    - Find the ground under the object's raised centre: its origin plus (size + offset) x scale up.
    - A straight-down shadow is the object's top view, size x scale each way, along its axes.
    - A sun shadow is the raised centre pushed along the light onto the ground's plane. It is
      size x scale wide and as long as a sphere of that radius casts (WSR measures the object's extent
      along the light in a function not traced closely enough to copy). Its image's top is at the far
      end.
  - **Mesh:** each decal is WSR's near mesh: the ground's triangles that face the light under the
    footprint, clipped to it and to the depth range, and lifted 0.15 units. They are cut from the
    render terrain, not the collision (unlike WSR), so the overlay depth pull keeps them on what is
    drawn.
  - **Counts:** 499 decals, 16,490 triangles, one detail batch a cell (culled beyond the detail
    distance like the edge strips).
  - **Material:** WSR's GX state. Black at alpha 0.45 x coverage, blended, depth tested and not
    written (an overlay), drawn first among the translucent (the strips and water go over it).
- **The cars:** WSR's car silhouette is a quad under the car's body, part of the car model, so it goes
  with every car (parked, driven, on its route). It is a detail batch.
- **The Mii's blob:** WSR's spectators' blob, `shadow_blob`, an instance the island puts on the ground
  under the Mii's feet, lying along the slope. It is hidden while the Mii swims or drives. The shadow
  map fades it out where it covers, so it shows only without the map.
- **The static sun bake** (`tools/g3d/shadows.py`) stays for the terrain, buildings and cliffs
  shading each other (no cost at run time). The placed objects (`BATCH_CASTER`) no longer cast in it:
  a tree's shadow fell between the terrain's vertices (texels of 30 units, vertices 70-140 apart), and
  the trees shaded themselves as solid cards. They still receive the terrain's and buildings' shadows.
- **The shadow map** (`engine/g3d/world.h`, `native_draw.h` ShadowMap):
  - **Coverage:** a square of 480 units seen along Map_Light (the light the static lighting has),
    512 texels (0.94 units a texel). It reaches from 6,000 units toward the sun down to 1,500 below
    the Mii. On the ground it is 480 x 825 units, pushed 120 units ahead of the Mii along the camera's
    heading.
  - **Snapping:** the square is snapped to the map's texels, so a still caster's shadow doesn't swim
    as the Mii moves.
  - **Casters:**
    - the trees and props of the cells (`CAST`: 525 runs of their level-0 batches, 33,870
      triangles; the leaves alpha-tested);
    - the instances' opaque batches: windmills with turning blades, cars, boats, birds, the airship;
      an instance off-screen is posed for the map;
    - the Mii's stand-in, `shadow_proxy`, a 64-triangle capsule as tall as the Mii. It casts only
      (`INST_CASTER_ONLY`), and only while the Mii is on foot.
  - **Receivers:** the island's materials (`MAT_SHADOW_RECEIVER`), where they fit: at most four
    combiner stages and two texture units, unit 0 not projected (material.h).
  - **Decals:** inside the square the decals fade out through a window texture (its outer 4 %
    blends), so nothing darkens twice.
  - **Darkness:** a shadow keeps 55 % of the light, as a decal's does.
- **On the 3DS** (`platform/3ds/g3d_pica.cpp`):
  - **Off by default:** the backend says it draws no shadow map (`native_draw.h`
    `Backend::shadow_maps`), so the World makes none: no casters, no receivers, no decal faded, no CPU
    spent on them. `NOMA_G3D_SHADOWS=1` in env.txt turns it on, for testing (the log's
    `g3d pica: backend ready` line says which; the island's `wuhu: shadows:` line says "the shadow map
    off (0 texels a side ...)" without it).
  - **Pass:** the opaque packet first draws the map into a shadow texture (VRAM, 1 MB at 512) through
    `pica_native_target` (gx_pica.cpp).
  - **Clear:** a quad clears it.
  - **Casters:** drawn in the GPU's shadow mode: green 0 (no penumbra), depth scale -1, no depth test
    (the GPU keeps the nearer depth), no culling, the leaves cut out by their alpha test.
  - **Receivers:** the material's unit 0 moves to unit 2 and the map takes unit 0 (only unit 0 can be
    a shadow texture: orthographic, a bias; its parameter word has the shadow bit as well as the shadow
    type, as citro3d's `C3D_TexInitShadow` makes it). Two combiner stages are added: darkness x lit +
    (1 - darkness), times the material's colour kept in the combiner buffer.
  - **Fade:** a decal's fade window is on unit 1.
  - **Shader:** the vertex shader is unchanged (unit 0's position mapping carries the depth in
    texcoord0w).
- **On the host:** the software backend draws the same map in 24-bit depths and samples it with the
  2x2 compare Azahar uses (`soft_backend.cpp`). `NOMA_SOFT3D_SHADOW_DUMP` writes it as a picture.
- **The package:**
  - 382,464 bytes more (VDAT +236 KB, IDAT +98 KB for the decals, CAST 18,900 bytes, SHDW 32
    bytes).
  - `CAST` and `SHDW` are optional; a package without them has no shadow map.
- **Cost a frame** (host counts through the views and runs; the 3DS's times are the user's to
  measure):
  - **Decals:** 0-9 more translucent draws (view_town +9, the others +0-2), 0-5,700 more triangles
    (view_town, its many trees).
  - **The map:** a clear of 512² texels and its casters: 2-21 runs, 100-1,800 triangles (the plaza
    21 / 1,787, Wind Park 16-18 / 830-905 with 7-8 instances).
  - **Receivers:** 6-22 items take one more texture unit and two combiner stages.
  - **The World:** about 0.0015 ms a frame more on the host (selftest g3d: 0.008 against 0.007 ms
    over the views).

Gaps:
- **Materials that can't receive:** the town walls `kabeAD`/`kabeBC`, the harbour ship and lava use
  three texture units, and the open sea and the cells' water aren't receivers. So boats shadow the
  shore but not deep water.
- **What doesn't receive:** props, vehicles and the Mii (drawn by GX) don't receive.
- **The map's edge:** shadows end at the map's edge, where the decals take over, with a seam in their
  shapes.
- **The Mii's shape:** the Mii casts as a capsule.

**Open on the 3DS, so off by default there.** In the emulator (Azahar 2126.1.2, Vulkan) the
receivers' compare doesn't read the map that was drawn, and what it reads instead has changed from run
to run:
- 2026-10-08: it darkened nothing, even against a map cleared to depth 0 (below).
- 2026-10-09 (the restaged package, the user's run): it darkened the whole square about the Mii, casters
  or not: "a shadow follows the player around". Against the host's frame from the same place
  (`NOMA_WUHU_AT=-3398.2,150,1382.7,-134`), the grass inside the square comes out at 0.64-0.69 of the
  host's brightness and outside it at 1.01-1.03; the host with every compare forced to "in shadow"
  comes out at 0.55. So the compare returned about a quarter lit everywhere in the square, the Mii's
  stand-in's shadow and the trees' were lost in it, and the decals, faded inside the square, were gone
  too. The receivers include the cliffs' rock and the grass edge strips laid over it, which is the
  likeliest source of the "squares of rock" the user saw where grass overhangs a cliff: a map texel
  (0.94 units) stretches over a steep face, and the strip and the rock under it can compare differently
  (host renders of those cliffs, without the emulator's compare, show nothing of the kind).
- What the map holds was right on 2026-10-08: drawn as an ordinary texture (`NOMA_G3D_SHADOW_SHOW`,
  and `NOMA_G3D_SHADOW_PROBE=1` on the receivers) it showed the casters where they belong. Only the shadow
  type's read differs, and Azahar's Vulkan shaders make it through a view of the surface of their own
  (an R32UI `texelFetch`); whether that view sees the surface the map was drawn into is the emulator's
  doing, and nothing in the command stream is known to decide it.
- Fixed on the way, for the console: the shadow unit's parameter word lacked the shadow bit
  (`GPU_TEXTURE_SHADOW_PARAM`, which citro3d sets for shadow textures; the emulator reads the type
  alone), and the pass's clear and casters gave `set_pixel` six of its eight words, so the stencil test
  and operation were whatever the stack held (it takes an array of eight by reference now).
- To switch it on by default, a console has to show it right (`NOMA_G3D_SHADOWS=1`: the Mii's capsule
  shadow at its feet, the trees' shadows inside the square, nothing darker elsewhere in it); an
  emulator's picture is no guide.

Narrowed down on 2026-10-08 with three debug switches (platform/3ds/g3d_pica.cpp, for env.txt, with
`NOMA_G3D_SHADOWS=1`):
- `NOMA_G3D_SHADOW_SHOW=1` draws the map in a corner as an ordinary RGBA8 texture: the casters reach
  it (the trees' crowns, the Mii's stand-in as coloured depth bytes on white).
- `NOMA_G3D_SHADOW_PROBE=1` shows the map on the receivers as an ordinary texture: their coordinates
  are right (the Mii's and the trees' shadows land where they should); `=2` leaves the unit in its
  shadow mode and shows the compare's result as it comes, without the darkening stages (not yet
  seen: that run never reached the island).
- `NOMA_G3D_SHADOW_CLEAR=0xAABBGGRR` clears the map to another value: at 0 (depth 0, all in shadow)
  the receivers still came out exactly as without the map, so either the PICA's compare (unit 0 in
  GPU_TEX_SHADOW_2D, TEXUNIT0_SHADOW, texcoord0w as the reference) never runs or reads lit, or the
  two combiner stages shadow_receiver_combiners appends (f = texture0 x darkness + (1 - darkness),
  then the buffered material colour x f) don't take effect. Azahar's sampler (its fragment shader
  generator's shadowTexture) returns 1 only outside the texture or where the stored depth is past
  the reference, so a depth-0 clear that reaches it must darken. (The combiner stages follow the
  PICA's buffer as the emulator's own reference model does: a stage reads the result of the stage two
  before it; the 2026-10-09 run, where the stages did darken, bears that out.)

## Left out (fidelity gaps)

- Indirect texturing: the open sea's warp of `jog_waterB` by `jog_waterA` (the engine has no
  dependent reads) and the lava's are left out; the sea's other three layers scroll as WSR's sea_P
  scrolls them. The waves near the eye are ours (WSR's island sea is flat; see "The sea"), and so are
  the trails behind boats; WSR's EFB reflections and refraction of its jet-ski sea are left out.
- The water reflection (`WS2_common_island_R`, drawn mirrored by WSR; its geometry is the far
  island), `WS2_common_under` (a navy skirt under the sea drawn with depth test off before the
  island), the night models (`island_L`, `vr_N`) and the evening sky (`vr_E`): daytime only.
- The far island (5,000 units off) is island_R's: no edge-blend strips (less grass over cliff
  edges), simpler shading, the mountain without its cave; detail (strips, foam, small props) goes
  beyond 3,000 units; trees and boats beyond 15,000.
- The order of the edge-blend strips: WSR draws the island's translucent list in its model's order
  (gake, kusa, stage logo, suna, tuti, net: the package's material order), the World each cell's
  batches back to front by their boxes' centres, so where two kinds overlap the one on top can change
  as the eye moves. Places where two kinds lie within 4 units of each other are few (about 40 such
  4-unit squares on the island: kusa and tuti by the castle and the jogging square, gake and kusa on
  the volcano's trails).
- Fog (since version 2): the far sea meets the clear colour in an edge from raised views.
- The sun's glare and lens flares (`WS2_common_sunshine`, `WS2_common_sun_lensfx`): their alpha
  follows the sun's place on the screen and whether the depth buffer hides it. The sun's height
  doesn't follow the eye's (1.2 degrees at the highest walkable ground). Billboards in models drawn
  by the cells are baked as placed.
- Particle effects: the waterfalls' mist, the volcano's smoke, the fountain's spray, whales'
  spouts (`Effect/Static/effect.carc`).
- Bloom and depth of field (`WS2_bic_F0_Bloom.pblm`, `_dof.pdof`). The stage reads the 42
  shadow-area boxes (PMP group 5/type 3) and bakes an initial ambient fade into prelit geometry:
  up to 45% within 5 units of a box's interior. It also bakes soft directional sun occlusion from
  opaque static island geometry (not the placed objects: "Shadows") into prelit vertex colours
  (1536-square light-space depth map, 5x5 PCF, at most 40% darkening), with no per-frame cost. Both
  are first approximations and need comparison against the original scene. The objects' shadows are
  WSR's decals and the shadow map ("Shadows").
- Flags' cloth motion (SHP0) and every PAT0: flags stand at rest.
- Rigid multi-bone shapes (gulls, hawks, whales): a triangle that spans two bones (270 of them)
  is kept with the bone most of its corners use, so it is exact at rest and stretches a little
  wrong when the bones move.
- Whales stay where they are placed (WSR also moves them along routes); gulls and hawks circle
  instead of flocking; the airship takes the title sequence's flight.
- The ambient sounds (PMP group 5: the beaches' waves, the birds, the waterfalls and windmills, the
  traffic), the time of day. The player's own sounds are in ("Sounds"), all from where it is: none
  placed in the world or panned by direction, no reverb.
- Model nodes whose batches aren't one run in the model's draw order have `n_batches` 0:
  renderers find a node's batches by `Batch.node`.

## How it was checked

- `ng3d.py selftest`: 23 structs (fields, types, array sizes), 23 static_asserts, 26 section tags
  and 14 enums against `engine/g3d/ng3d_format.h` (changing a field's type or a tag in a copy of
  the header is caught).
- `ng3d.py check`: the unbaked package (every reference in range, every payload inside its section
  and of the size its levels or layout take, VA_POS16 buffers with an origin and a scale, batch
  flags, the CVIS rows' size, key frames in order, marker kinds; read and written again, the same
  11,356,912 bytes) and the baked one (the same records, laid out by ng3d-bake). Two staging runs
  give byte-identical files.
- The engine's loader and world on the baked package (`NOMA_SELFTEST=g3d` with
  `NOMA_G3D_PACKAGE`, host; `engine/g3d/package.cpp` reads version 2's fields): loaded and checked
  in 0.2 ms with no texture converted, k = 10, 140 checks passed. Through the view cameras (a
  400:240 frustum, near 1.5, far 80,000; frame 600), with the World of version 1 (no detail
  distance, no visibility rows, cells culled whole), version 1's package in brackets:

  | view | cells | instances | draws (opaque + translucent) | triangles |
  |---|---|---|---|---|
  | town | 19/120 | 22/94 | 176 + 31 (256 + 86) | 66,536 (81,306) |
  | coast | 33 | 13 | 171 + 29 (258 + 95) | 44,828 (70,128) |
  | beach | 16 | 13 | 84 + 22 (103 + 28) | 23,685 (24,854) |
  | interior | 10 | 23 | 122 + 26 (149 + 39) | 34,316 (36,588) |
  | distant | 43 | 23 | 86 + 9 (109 + 16) | 24,302 (31,095) |
  | overview | 36 | 21 | 171 + 3 (327 + 115) | 34,372 (84,936) |
  | wedge | 47 | 21 | 94 + 13 (129 + 19) | 36,371 (44,509) |

  Averages, with what a version 2 World does on top (`tools/g3d/viewstats.py`, a Python copy of
  World's culling that gives the selftest's numbers exactly, with the detail distance, a test of
  each cell batch's own box and the visibility rows switchable), over the views and the speed
  round's 144 walking frames (`tools/wuhu/walk_cams.txt`: 18 places on the main island, 8 yaws each, the
  follow camera at rest):

  | | views: draws | triangles | vertices | walking: draws | triangles | vertices (lit at run time) |
  |---|---|---|---|---|---|---|
  | version 1 | 247.0 | 53,345 | 68,586 | 242.7 | 54,347 | 70,158 (68,151) |
  | version 2, World as it is | 148.1 | 37,773 | 48,684 | 180.4 | 52,073 | 67,077 (378) |
  | + detail distance | 137.7 | 35,113 | 44,729 | 165.5 | 47,646 | 60,560 (287) |
  | + each cell batch by its box | 110.9 | 27,454 | 34,148 | 120.5 | 35,664 | 44,624 (249) |
  | + visibility rows | 110.9 | 27,454 | 34,148 | 120.5 | 35,664 | 44,624 (249) |

  The engine's World does all of these since the speed round was integrated (a cell batch outside
  the view by its own box too), and more: an animated prop is drawn only where its meshes are and
  over a pixel across. `NOMA_SELFTEST=g3d` through the views (the ground under each eye from the
  collision): 91.6 draws and 21,256 triangles on average (view_town 140, 47,246; view_beach 44,
  5,899). (Vertices: the distinct ones each draw indexes, what the vertex shader runs on.) A walking
  frame's 35,664 triangles and 120.5 draws: the full island near 14,200 (40 draws, its detail
  strips with it), trees 5,300 (9), props 3,900 (7), the far island 1,500 (16), the sea in the
  cells 750 (12), the sky and the open sea, the instances 9,800 (34: the route-driven cars and
  cruisers 6,700 and 19 of them).
- The package rendered by `tools/g3d/preview.py` from those cameras at 1280x720 against version 1's
  (PSNR in dB, mean difference in levels, share of pixels more than 16 levels off), version 1 lit
  per vertex as GX lights it (preview.py lights per fragment by default; against that, the town is
  at 33.2 dB, all of the difference the lighting's way). Then each change alone:

  | view | v1 -> v2 | lighting baked (vs live, per vertex) | no fog (v1) | far island (vs level 0 everywhere) | detail beyond 3,000 |
  |---|---|---|---|---|---|
  | town | 37.23 / 0.99 / 0.53 % | 53.72 | inf | 49.09 | 71.12 |
  | coast | 43.19 / 0.23 / 0.35 % | 61.49 | 50.88 | 57.02 | 56.85 |
  | beach | 30.89 / 0.96 / 2.44 % | 39.87 | 46.44 | 33.75 | 40.12 |
  | interior | 47.95 / 0.23 / 0.03 % | 79.34 | 60.07 | inf | inf |
  | distant | 30.89 / 0.92 / 2.98 % | 45.92 | 32.40 | 33.48 | inf |
  | overview | 23.49 / 4.25 / 11.30 % | 45.16 | 27.49 | 26.99 | inf |
  | wedge | 37.95 / 0.14 / 0.33 % | 50.59 | 44.92 | 36.18 | inf |

  The baked lighting matches live per-vertex lighting but for rounding and the balloons (lit as
  they stand at rest); the fog's and the far island's are the changes the sections above describe;
  the rest of version 1 -> 2 is which props and tree models are drawn where (detail levels at
  5,000 and 15,000 instead of 3,000 and 20,000, the small instances' cull distances).
- The far island's switch: each of the 8 cells with the most level-0 triangles from just past
  5,000 units, alone at level 0 and at level 1: 34.3-63.6 dB, at most 1.43 % of the pixels more
  than 16 levels off (cells and detail levels, above).
- The visibility rows: 120 of 120 renders identical with and without them (cell visibility, above).
