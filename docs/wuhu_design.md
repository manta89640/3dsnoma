# Walkable Wuhu Island — design (v1, 2026-10-04, session 3dsnoma-8a)

Branch `wip/wuhu-8a`, worktree `/Users/user/Projects/3dsnoma-wuhu`. Research reports in
`scratchpad/wuhu/research/*.md` (brres, placement, wsrdol, port, gfx, ui, audio) are the evidence for
everything below; read the ones your track names. This file is the contract between tracks.

## 0. Decisions (user-approved where marked)

1. **Native g3d** (user, 2026-10-04): the island is drawn by a NEW native renderer, not by the
   channel's nw4r::g3d through the emulated GX path (that path can't carry ~19k GX draws/frame:
   gfx.md §3). It is written as the start of a general native g3d: a native package format (NG3D)
   made from BRRES at staging time, a renderer core in `engine/g3d/`, a PICA backend (3DS GPU) and a
   software backend (host). "We'll write it for the island first and then in another pass replace
   g3d everywhere else": keep the core general (models, materials, textures, node animation,
   instances), nothing island-specific in `engine/g3d/`.
2. **The Mii stays on the channel's g3d** for now (room ActorManager, RFL face, Move.lex
   animations), in its own guest ScnRoot. Island and Mii share the frame and the depth buffer through
   a **native-draw command in the GX command stream** (§4).
3. **Canonical assets** (brres.md §6, wsrdol.md §2): `WS2_common_island` at Island Flyover
   visibility (mode 13: hide Island_16b, 17, 17b, 19, 19c, 20, 20c, 22, Water16b; island_L off),
   `WS2_common_sea` (all but Water16b; `sea2_dummy` -> `sea2_day` from `WS2_common_seatex.brres`;
   SRT0 `sea_P` + `sea_nami`, CLR0 `sea_nami`), `WS2_common_island_V` (+CLR0), island CLR0/SRT0
   (lava), `WS2_common_vr` (day sky, SRT0 clouds). `WS2_common_island_R` = far LOD candidate (optional).
   `WS2_common_under`: left out (documented). Props: `PlnEnv0.carc/WS2_pln_island_tag0.pmp`, group 1,
   include 0, exclude mask 2 (placement.md §2), models from StageArc (class table, placement.md §2.3).
   Collision: `StageArc/Island/WS2_common_island1..5.kcl` (+ tree trunks). Day environment:
   Map_Light white from (10000,48000,60000) toward the origin, ambient Map #788287 (light set 0);
   Obj set 1 = Map_Light + Obj_Light #736e5a, ambient #4d5173; fog 0 = `ws2_common_day.pfobj`
   linear 27,978..65,527 #83d5ff (island units, view depth).
4. **Everything stays resident** (no streaming needed): native geometry ~2.5 MB + textures ~3.6 MB
   (ETC1/ETC1A4 + mips, staged) + KCL 3 MB + props. 3DS linear heap has ~16 MB free in a native scene,
   the normal heap ~49 MB (port.md §3). We still cull: cells by frustum + distance, tree/prop LOD by
   distance. Measured numbers go in the report; if a measurement disagrees, revisit.
5. **Units.** Island data stays in WSR units (y up; sea at y=0; 1 unit ≈ 9 cm). The Mii lives in
   Wii no Ma units (Mii 158 tall at height scale 1). `k = MII_UNITS_PER_ISLAND_UNIT` is computed once
   from the WSR audience Mii model's height (`Audience/G3D/WS2_common_avaC0.brres` bbox) vs 158, and
   written into the package (ENVR). Gameplay (player, KCL, camera) works in island units; the Mii's
   guest position = k * island position; the guest camera = the island camera scaled by k; the native
   renderer draws with model matrix S(k) under the guest camera's view, and the GX projection (§4), so
   depth agrees exactly with the Mii's draws.
6. **Bar button** (ui.md §1.5): 出発 hangs from `Loc_Tv_Btn`, made from `Move_Btn_Tv_Btn.lex`
   (the end cap), offline only (online the bar is unchanged). Palm-island icon + 出発 label, made like
   the Sound Player's (art.h). SceneTransit To = **10**.
7. **Transitions** (ui.md §2): `Fade_Black` (Wipe kind 2) out of the room and back; Progress_Black
   ripple rings while loading (only if > 0.5 s); the scene fades in from black when ready.
8. **Return** (ui.md §3): the Home tab (layout::parts::Bottom, mode HOME) + B/START open a confirm
   window built natively from Common_Scene/Common_Window/Common_SmallBtn: 「部屋にもどります。\n
   よろしいですか？」 いいえ / はい. はい -> music handoff, `ChangeSceneWithWipe(LivingRoomScene, 2)`.
9. **Music**: 「広い世界へ」, our own composition, played through the channel's sound system with
   the channel's own banks (audio.md); the room theme pauses on departure and resumes on return
   (the Sound Player's Music semantics, music.cpp).

## 1. Layout and ownership

| path | what | track |
|---|---|---|
| `tools/g3d/brres.py`, `gxtex.py` | BRRES/TEX0 reader (from research `nw4r_brres.py`, `gxtex.py`, cleaned) | P |
| `tools/g3d/ng3d.py` | NG3D writer + reader (round-trip check) | P |
| `tools/g3d/preview.py` | numpy reference renderer (research `g3d_preview.py`), can render a BRRES or an NG3D | P |
| `tools/wuhu/stage_wuhu.py` | user's WSR extraction -> `DATA/wuhu/island.ng3d` (+ inventory json) | P |
| `docs/wuhu.md` | asset inventory, placement, dependencies, fidelity gaps, measurements | P (+I) |
| `engine/g3d/ng3d_format.h` | THE pack format (C++ structs; Python mirrors it) | F (done first) |
| `engine/g3d/package.{h,cpp}` | load/validate a package in native memory, find things by name | R |
| `engine/g3d/texture.{h,cpp}` | texture records; GX->PICA conversion via `platform/3ds/pica_tex.h` when a texture is still GX-format | R |
| `engine/g3d/material.{h,cpp}` | GX material image -> neutral combiner program (via `engine/gx/tev_combiner`) + texgen/channel/pixel state | R |
| `engine/gx/tev_combiner.{h,cpp}` | GX TEV -> PICA combiner translation, EXTRACTED from `platform/3ds/gx_pica.cpp` (`tev_to_texenv`, `plan`, `apply_half`, ...), platform-neutral output; gx_pica.cpp uses it too, behaviour-identical (tell 03) | R |
| `engine/g3d/anim.{h,cpp}` | SRT0/CLR0/CHR0 keyframe evaluation (hermite/step/linear) | R |
| `engine/g3d/world.{h,cpp}` | cells, instances (models + node anim), sky, culling, LOD, per-frame packet build | R |
| `engine/g3d/native_draw.{h,cpp}` | packet ring + the GX stream command (§4) | R |
| `engine/g3d/backend.h` | backend interface | R |
| `platform/3ds/g3d_pica.cpp`, `platform/3ds/shaders/g3d.v.pica` | PICA backend | B3 |
| `engine/g3d/soft_backend.cpp` (+ accessor in `runtime/hw/gx/raster.*`) | host software backend into the host EFB | BH |
| `runtime/hw/gx/cp.cpp`, `gx.h` (`GxRenderer::native`), `gx_pica.cpp`, `raster.cpp` | the hook | R (cp/gx.h), B3 (gx_pica), BH (raster) |
| `engine/collision/kcl.{h,cpp}` | KCL loader + queries; trunk cylinders | G |
| `engine/wuhu/*` | the island scene: scene, loader, player (Mii), camera, island setup, menu, telop | G |
| `engine/input/remote.h`, `platform/*/main.cpp`, `runtime/hw/ios/input_script.cpp` | analog stick + camera input; host script tokens | G |
| `engine/ui/` (promoted from `engine/soundplayer/{game,lyt,art,g3d}.h`, tell 85) | shared layout/game helpers | U |
| `engine/wuhu/bar_button.cpp`, `runtime/patches/menus.cpp`, `runtime/patches/wipes.cpp` | 出発 button, To 10, fade-to-black patches | U |
| `engine/wuhu/music.*`, `tools/music/*`, score source | 「広い世界へ」 | M |

Tracks: F foundation (me), P pipeline, R renderer core, B3 PICA backend, BH host backend, G gameplay
scene, U UI, M music, I integration/test (me). Renames of fn_/lbl_ symbols: tracks LIST them in
`scratchpad/wuhu/renames.txt` (`fn_X=Name  # why`), and use the fn_ names in code meanwhile; the
integrator applies them with `tools/name_symbol.py` (it also rewrites hand-written code) and adds
PATCHED_NAMES. Shared files (CMakeLists.txt, decompile.py, symbols.txt): additive edits only, the
integrator merges.

## 2. NG3D package (see `engine/g3d/ng3d_format.h` for the exact structs)

Little-endian (host x86-64/arm64 and the 3DS ARM11 are both LE). One file, sections found by tag via
a table after the header; each section 32-byte aligned (texture data 128-byte aligned). Strings in
STRS. Everything is position-independent (offsets), so the loader reads the file into one block and
points into it; GPU-visible data (texels, vertices, indices) is copied to linear memory by the PICA
backend at prepare time and the file block's copy may then be freed by the backend's owner (tracked).

Sections (v1):
- `STRS` string table. `TEXS` texture records; `TDAT` texel data (PICA-tiled, mip levels
  consecutive; or GX raw when `Texture.flags & TEX_GX_RAW`, converted at load with pica_tex.h).
- `MATS` materials: a GX register image (genmode, TEV stages, TREF, KSEL, color/konst registers,
  alpha compare, zmode, blend, cull; texture map slots (texture index, wrap, filter, LOD bias, min/max
  LOD); texgens (source, type, matrix index, post/normalize); static texture matrices; color channel 0
  control and material/ambient colours; light set; fog flag; indirect flag (approximated: dropped);
  pass (opa/xlu) and sort priority). The runtime translates it with `engine/gx/tev_combiner` -- the
  same translation the GX path uses on the 3DS, so a material looks the same either way.
- `VTXB` vertex buffers, `IDXB` index buffers (u16, triangle lists). Vertex layout per buffer:
  attribute mask + formats (pos f32x3; normal s8x3 (+pad); color u8x4; uv0/uv1 s16x2 with per-buffer
  scale or f32x2). One layout per buffer, stride stated.
- `MESH` meshes = list of batches; batch = (material, vertex buffer, index range, bbox).
- `NODE` node trees per model (name, parent, SRT, bind matrix, visibility).
- `MODL` models: node tree + meshes per node (for instanced/animated models and for the room pass).
- `CELL` the static world: cells (x/z grid, bbox, list of batches per LOD level 0..2).
- `INST` instances of models (model index, Mtx34, flags, anim index, class id, LOD distances).
- `ANIM` animations: tracks (target kind: node SRT component / material texmatrix SRT component /
  material colour register component; target index; keyframes {frame, value, slope} with
  interpolation hermite/linear/step), length, loop.
- `ENVR` environment: lights (dir/pos, colour), light sets (light mask + ambient), fog table
  (start, end, colour, type), sky model index, sea info, k (Mii units per island unit), spawn point +
  yaw, landmark list (name, position) for camera presets/tests, world bbox.
- `COLL` collision: raw KCL files (big-endian, as WSR ships them) each with a name, + trunk
  cylinders (x, z, y0, y1, radius).

## 3. Renderer core (engine/g3d)

- `Package`: loaded from bytes (one `malloc`ed block, owned), validated (magic, version, every offset
  in range), indexes built (by name). No GPU calls.
- `Material` (runtime): from the GX image -> `TevCombiners` (6 stages: src/op/func/scale/const
  colour; buffer masks), texture units (<=3: texture, wrap, filter, lod bias, texgen), texgen plan
  (per unit: uv0/uv1/position/normal source, 2x3 or 3x4 matrix, projection, normalize+post),
  lighting (channel 0: lit, mat from vtx/reg, amb from reg/vtx, light set), pixel state (alpha test
  func/ref (one compare, as gx_pica), blend src/dst/op, depth func/write, cull), fog on/off, pass.
  CLR0/SRT0 animations modify the material's registers/matrices per frame (the runtime keeps a
  per-material dynamic block the packet snapshots).
- `World`: built from a package: static cells, instances, sky, env. Per frame:
  `update(time_frames)` (animations), `set_camera(view, eye)` in GUEST units with `world_scale = k`,
  then `emit(Pass)` builds a `Packet` (visible batch list in draw order with their dynamic material
  state, instance matrices, view matrix, light/fog values) and queues it through `native_draw`.
  Culling: cell bbox vs frustum (from the projection parameters the scene passes) + max distance;
  LOD by cell/instance distance. Draw order: OPAQUE pass = sky (depth write off, z-test off or at far),
  opaque batches front-to-back by cell; TRANSLUCENT pass = xlu batches back-to-front (by batch bbox
  centre distance), material priority respected.
- `native_draw`: ring of 8 packets; `emit` waits only if the ring is full (fence counters written by
  the backend thread). Command in the GX FIFO: opcode **0x50** then a big-endian u32 = packet id
  (written through the gather pipe, `guest::gx_fifo_write`). cp.cpp decodes it and calls
  `GxRenderer::native(id)`; the renderer (PicaRenderer / SoftRenderer) calls the registered native
  backend with the packet and the CURRENT GX projection (XF 0x1020-0x1026) and viewport/scissor, so the
  native draw is placed exactly like a GX draw at that point of the stream. Package lifetime: before
  freeing a package the scene calls `native_draw_release(package)` which does `gx_sync()` and the
  backend's release on its thread (a release packet).
- `Backend` interface: `prepare(Package&)` (upload texels/buffers; run at the first packet that
  references a package, or by a prepare packet during loading so the hitch is under the black
  screen), `draw(const Packet&, const GxView& view)`, `release(Package&)`.

## 4. The GX-stream hook (both renderers)

- `runtime/hw/gx/gx.h`: `GxRenderer::native(u32 id)` (default no-op). `cp.cpp`: case 0x50 (5 bytes).
- `platform/3ds/gx_pica.cpp` `PicaRenderer::native`: flush the current batch, `set_xform()` (so the
  projection/scissor/viewport are current), call the PICA backend, then invalidate everything the GX
  path caches about GPU state (shader program, attribute layout, uniforms, texenv, textures, depth/
  blend/alpha/cull state, `c3d_touched`, `all_dirty`, `!env_valid`, `!consts_valid`, `uni_resend`,
  `cull = ~0`; gfx.md §5.1) so the next GX draw re-sends what it needs.
- PICA backend (`g3d_pica.cpp`): its own shader (`g3d.v.pica`): uniforms MVP (= P.clip x GX projection x
  view x S(k) x instance), modelview for normals, light dir/colour x2 + ambient (set per batch from the
  material's light set), texgen matrices x3, flags (bool uniforms: lit, uv1, env, projection,...). Fixed
  attribute layouts per vertex-buffer layout. Fog: PICA fog LUT built from the env fog (start/end in
  view depth) mapped through the projection; fog enabled per material. Mipmaps: `C3D_TexInitMipmap` +
  LOD bias (+ log2(640/400) correction, verify visually). Textures and buffers in linear memory
  (VRAM optional later). State per batch via C3D_* calls (no per-vertex CPU work).
- Host backend (`soft_backend.cpp`): a software rasterizer for the same packets into the host EFB
  (`raster.h` gains an accessor for the colour/depth buffers, size, scissor and the viewport offset):
  clip, perspective-correct attributes, depth test/write (24-bit, same window-z as GX draws: computed
  from the same projection), combiner evaluation (the neutral TevCombiners, software, PICA semantics),
  texture sampling (decoded to RGBA8 at prepare; bilinear + mip selection), alpha test, blend, fog.
  Speed target: a 640x456 island frame in < 0.5 s (host runs dump every Nth frame).

## 5. Gameplay scene (engine/wuhu)

- `IslandScene : engine::system::HostScene` ("scene::IslandScene"; factory made at boot in
  `make_lasting`, like the Sound Player). States: LOADING -> FADING_IN -> PLAYING <-> MENU ->
  LEAVING.
- Loading: the scene ctor starts a native load of `wuhu/island.ng3d` (one file) on a host thread (or as
  an I/O-thread job; NOT on the guest CPU thread: it would stall the sound thread). While loading,
  draw black + Progress_Black after 0.5 s. When loaded: `World` built, backend prepare packet, the
  Mii's guest objects (ScnRoot without a stage model -- ScnRoot::Construct + camera/viewport like
  g3d::Stage minus the model, port.md §2.3; Mii shadow res; ActorManager with Move/Wait/Head/
  Color_Anim; Sound3DHandle list; room_SpawnMii(person 0)) -- all guest allocations in the ctor or the
  first frames, freed in reverse order in the dtor (heap = stack). Then Fade_Black In (16 f), telop
  「ウーフーアイランド」 + 「♪ 広い世界へ」 (Top_Message, optional).
- Player: position/yaw in island units; walk speed ~1 m/s, run ~3 m/s (the Mii's own Move-state
  speeds converted); analog: magnitude < 0.6 walk, > 0.6 run (accelerate/decelerate smoothly);
  facing turns toward the move direction at the BodyController's turn rate. Animation: baseline =
  SpecialAnim behaviour with names Wait / Walk (Female_Walk) / Run (Female_Run) chosen by speed, the
  clip frame driven by distance travelled (stride length per loop measured from the clip) so feet
  don't slide; footstep SE optional. Stretch: the game's own Move state via a behaviour vtable with
  native GetSpeed/GetDesiredYaw/GetTurnRate (port.md §4.4).
- Collision (`engine/collision/kcl`): ground = highest walkable triangle (normal.y > 0.7, attribute
  not water) under the feet within a step height (40 island units up, 200 down); walls = sphere
  (radius ~6 island units at knee and chest height) push-out against non-walkable triangles and
  invisible fences (attr high byte 0x08/0x00); water: the Mii may wade while the water surface
  (attr 0x1106) is < 8 units above the ground, else that move is refused (shore stops the Mii); slopes
  steeper than walkable push back; tree trunks = cylinders from INST (radius from the model). Must use
  the octree with neighbouring cells (the octree only lists triangles within its build radius).
- Camera: third person, orbit yaw/pitch/distance around the Mii's head (island units: distance ~35,
  pitch 15 deg default); auto-turn behind the Mii while walking (slowly), manual orbit overrides for
  2 s; terrain avoidance (ray from target to camera against the KCL, pull in); FOV 45 deg;
  near 1.5 / far 80,000 island units (×k in guest units).
- Controls. 3DS: circle pad = move (stick mode: no pointer nudge while the island runs); C-stick (New
  3DS, irrst) or L/R = orbit camera, D-pad up/down = pitch, D-pad left/right = orbit, Y = camera
  behind; START or B = the menu (B presses the Home tab like the Sound Player); touch = pointer
  (Home tab, dialog). Host: NOMA_INPUT tokens `S(x;y)[:secs]` (stick), `C(x;y)[:secs]` (camera
  stick) on top of the existing grammar; + key names L, R, Y, START.
- Menu: the confirm window (§0.8): the island freezes (player, camera; animations may go on),
  pointer or A/B (A = はい when the pointer is off) chooses. いいえ closes it.
- Leaving: music handoff (§6), Fade_Black via ChangeSceneWithWipe(..., 2); dtor frees everything
  (native_draw_release first). The room then fades in; patch the room's entry wipe to black once
  (ui.md §2.1) if needed.
- Logs (for the report): `wuhu: load: N bytes in T ms (read R ms, build B ms, prepare P ms)`,
  `wuhu: memory: native X KB (geometry, textures by format, collision), linear free, heap free, guest
  MEM2 free`, every 600 frames `wuhu: fps F, cells C/Ct, batches B, tris T, instances I, pos (x,y,z)`.

## 6. Music
See audio.md (pending at the time of writing): the score source (text, ours) + an RSEQ assembler in
`tools/music/`, the generated RSEQ committed as a header (our own work, not Nintendo data), injected
into the in-memory BRSAR at boot as a new sound (or a reused placeholder slot) on the BGM player with
an existing bank; `engine/wuhu/music` plays it via BGMManager on arrival, `leave()` restores the room
theme (Sound Player semantics).

## 7. Verification
- Pipeline: the NG3D round-trips through `ng3d.py`'s reader; `preview.py` renders the PACKAGE from the
  research cameras (town, coast, beach, interior, distant, overview) and must match the BRRES previews
  (`research/brres_views/*.png`) closely (difference image + PSNR in the report).
- Host: scripted runs (room -> 出発 -> island -> walk paths -> menu -> back -> room) with frame dumps at
  the 4 required views (town, coast, interior = volcano lava chamber/cave or a building interior,
  distant island); compare to the reference renders; collision tests (walk into a wall, the sea, up
  a slope) with logged positions.
- 3DS: build-3ds-rel; Azahar run ONLY after checking the user isn't running it (pgrep), SD backed up and
  restored (trap), env.txt with NOMA_INPUT for the same path + NOMA_SHOT_EVERY; logs give load time,
  memory, fps.
