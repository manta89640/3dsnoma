// NG3D: the native g3d package, the form 3D content takes for the native renderer (engine/g3d).
//
// Made at staging time from Nintendo's BRRES (and placement/collision data) by tools/g3d/ng3d.py,
// read at run time by engine/g3d/package.cpp. The file is one block: a header, a section table, and
// sections found by their tag. Every reference is an offset from the start of the file (or an index
// into a section's array), so the loader reads the file into memory and uses it in place.
//
// Byte order: little-endian (the 3DS's ARM11 and the hosts the tools run on). Sections start on
// 32-byte boundaries; texel data on 128-byte boundaries. Strings are UTF-8, NUL-terminated, in STRS,
// referenced by their offset in it (0 = the empty string). Index fields use NONE (0xFFFF / ~0u) for
// "none". This header is the one definition of the format: tools/g3d/ng3d.py mirrors it field by
// field (keep the two in step; bump VERSION when a layout changes).
#pragma once

#include <cstdint>

namespace engine::g3d::fmt {

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using s8 = int8_t;
using s16 = int16_t;
using f32 = float;

constexpr u32 tag(char a, char b, char c, char d) {
    return (u32)(u8)a | (u32)(u8)b << 8 | (u32)(u8)c << 16 | (u32)(u8)d << 24;
}

constexpr u32 MAGIC = tag('N', 'G', '3', 'D');
constexpr u32 VERSION = 2;  // 2: compact positions, prelit materials, cell visibility, detail batches
constexpr u16 NONE16 = 0xFFFF;
constexpr u32 NONE32 = 0xFFFFFFFFu;

struct Header {
    u32 magic;       // MAGIC
    u32 version;     // VERSION
    u32 file_size;   // bytes, the whole file
    u32 n_sections;  // Section entries right after the header
    u32 flags;       // 0
    u32 reserved[3];
};
static_assert(sizeof(Header) == 32, "Header");

// the section table
struct Section {
    u32 tag;     // e.g. tag('T','E','X','S')
    u32 offset;  // from the start of the file
    u32 size;    // bytes
    u32 count;   // records in it (for array sections), else 0
};
static_assert(sizeof(Section) == 16, "Section");

// section tags
constexpr u32 SEC_STRS = tag('S', 'T', 'R', 'S');  // strings
constexpr u32 SEC_TEXS = tag('T', 'E', 'X', 'S');  // Texture[count]
constexpr u32 SEC_TDAT = tag('T', 'D', 'A', 'T');  // texel data
constexpr u32 SEC_MATS = tag('M', 'A', 'T', 'S');  // Material[count]
constexpr u32 SEC_TMTX = tag('T', 'M', 'T', 'X');  // TexMtx[count] (materials' texture matrices)
constexpr u32 SEC_VTXB = tag('V', 'T', 'X', 'B');  // VertexBuffer[count]
constexpr u32 SEC_VDAT = tag('V', 'D', 'A', 'T');  // vertex data
constexpr u32 SEC_IDAT = tag('I', 'D', 'A', 'T');  // index data (u16)
constexpr u32 SEC_BTCH = tag('B', 'T', 'C', 'H');  // Batch[count]
constexpr u32 SEC_NODE = tag('N', 'O', 'D', 'E');  // Node[count]
constexpr u32 SEC_MODL = tag('M', 'O', 'D', 'L');  // Model[count]
constexpr u32 SEC_CELL = tag('C', 'E', 'L', 'L');  // Cell[count] (the static world)
constexpr u32 SEC_CBAT = tag('C', 'B', 'A', 'T');  // u16 batch indices the cells list
constexpr u32 SEC_INST = tag('I', 'N', 'S', 'T');  // Instance[count]
constexpr u32 SEC_ANIM = tag('A', 'N', 'I', 'M');  // Anim[count]
constexpr u32 SEC_TRAK = tag('T', 'R', 'A', 'K');  // Track[count]
constexpr u32 SEC_KEYS = tag('K', 'E', 'Y', 'S');  // Key[count]
constexpr u32 SEC_ENVR = tag('E', 'N', 'V', 'R');  // Env (one)
constexpr u32 SEC_LITE = tag('L', 'I', 'T', 'E');  // Light[count]
constexpr u32 SEC_LSET = tag('L', 'S', 'E', 'T');  // LightSet[count]
constexpr u32 SEC_FOGS = tag('F', 'O', 'G', 'S');  // Fog[count]
constexpr u32 SEC_MARK = tag('M', 'A', 'R', 'K');  // Marker[count] (landmarks, spawn points)
constexpr u32 SEC_COLL = tag('C', 'O', 'L', 'L');  // Collision[count] (raw KCL files)
constexpr u32 SEC_CYLS = tag('C', 'Y', 'L', 'S');  // Cylinder[count] (tree trunks etc.)
constexpr u32 SEC_BLOB = tag('B', 'L', 'O', 'B');  // data the above point into (KCL bytes, ...)
constexpr u32 SEC_CVIS = tag('C', 'V', 'I', 'S');  // cell visibility: a bit row per cell (Env::pvs_row_bytes)
constexpr u32 SEC_SEAW = tag('S', 'E', 'A', 'W');  // Sea (one, optional), then its amplitude map

// ---------------------------------------------------------------- textures
// PICA texel formats (the numbering of citro3d's GPU_TEXCOLOR)
enum TexFormat : u8 {
    TF_RGBA8 = 0, TF_RGB8 = 1, TF_RGBA5551 = 2, TF_RGB565 = 3, TF_RGBA4 = 4, TF_LA8 = 5,
    TF_HILO8 = 6, TF_L8 = 7, TF_A8 = 8, TF_LA4 = 9, TF_L4 = 10, TF_A4 = 11, TF_ETC1 = 12,
    TF_ETC1A4 = 13,
};
enum TexFlags : u8 {
    TEX_GX_RAW = 1,  // the texels are still GX's (gx_format, GX tiling, GX mip chain): the loader
                     // converts them (platform/3ds/pica_tex.h from_gx) to `format`
    TEX_ALPHA = 2,   // has alpha below 255 somewhere
    TEX_CUTOUT = 4,  // ... only 0 or 255 (alpha-tested)
};
struct Texture {
    u32 name;         // STRS
    u16 width, height;  // of level 0; powers of two >= 8 once converted
    u8 format;        // TexFormat (what the GPU gets)
    u8 levels;        // mip levels stored (>= 1); each level at least 8x8
    u8 flags;         // TexFlags
    u8 gx_format;     // GX's format (TEX_GX_RAW: how the data is laid out), else the source's
    u32 data;         // offset in the file (TDAT), 128-aligned
    u32 size;         // bytes of all levels
    u16 src_width, src_height;  // the source image (UVs address it: scale by src/width if padded)
    u32 reserved;
};
static_assert(sizeof(Texture) == 28, "Texture");

// ---------------------------------------------------------------- materials
// A material is GX's register image for it (what an nw4r material's display lists set), so that one
// translation (engine/gx/tev_combiner) turns it into GPU state for any backend. Field encodings are
// GX's own (BP/XF register values) unless stated.
constexpr int MAX_TEV_STAGES = 16;
constexpr int MAX_TEX_MAPS = 8;
constexpr int MAX_TEXGENS = 8;

struct TexMap {
    u16 texture;     // Texture index, NONE16 = unbound
    u8 wrap_s, wrap_t;  // GX wrap: 0 clamp, 1 repeat, 2 mirror
    u8 min_filter, mag_filter;  // GX filter: 0 near, 1 linear, 2 near-mip-near, 3 lin-mip-near, 4 near-mip-lin, 5 lin-mip-lin
    u8 max_aniso, flags;  // flags bit 0: bias clamp, bit 1: edge LOD
    f32 lod_bias;    // as GX (added to the computed LOD)
    f32 min_lod, max_lod;
};
static_assert(sizeof(TexMap) == 20, "TexMap");

struct TexGen {
    // GX texgen: type (0 MTX3x4, 1 MTX2x4, 2-9 bump, 10 SRTG), source (0 POS, 1 NRM, 2 BINRM,
    // 3 TANGENT, 4-11 TEX0-7, ...), matrix (index into Material::tex_mtx, NONE16 = identity),
    // normalize + post matrix (post_mtx: index into Material::tex_mtx or NONE16)
    u8 type, source, normalize, pad;
    u16 matrix, post_mtx;
};
static_assert(sizeof(TexGen) == 8, "TexGen");

// a texture matrix as nw4r computes it (MAYA/XSI/3DSMAX mode, scale/rot/trans), and its effect
// matrix (env/projection mapping: camera/light/specular map, the 3x4 the material's TexMtxEffect
// gives); the animation (ANIM tracks targeting it) changes s/r/t per frame
struct TexMtx {
    f32 scale[2], rotate, translate[2];
    u8 mode;         // nw4r TexMatrixMode: 0 Maya, 1 XSI, 2 3dsMax
    u8 map_mode;     // nw4r TexMapMode: 0 UV, 1 env camera, 2 projection, 3 env light, 4 env spec
    u8 ref_camera, ref_light;
    f32 effect[3][4];  // the effect matrix (identity unless map_mode needs one)
};
static_assert(sizeof(TexMtx) == 72, "TexMtx");

enum MatFlags : u32 {
    MAT_XLU = 1,          // drawn in the translucent pass (back to front)
    MAT_FOG = 2,          // fog applies (GX fog index 0 or 1: Material::fog)
    MAT_INDIRECT = 4,     // the source had indirect stages (dropped: approximated)
    MAT_LIGHT_SET = 8,    // light_set is valid
    MAT_PRELIT = 16,      // its lighting is baked into the vertex colours (a static sun): drawn as an
                          // unlit material whose channel colour is the vertex's (no normals needed)
    MAT_SEA = 32,         // the sea's flat surface at its level (Sea::level): not drawn where the wave
                          // grid (Sea) drew this frame
    MAT_RUNTIME = 64,     // the scene sets its colour registers while it runs (World::set_material_register)
};

struct Material {
    u32 name;          // STRS
    u32 flags;         // MatFlags
    u8 n_stages, n_texgens, n_chans, n_ind;  // GENMODE's counts (n_ind: indirect stages, dropped)
    u8 cull;           // GX cull: 0 none, 1 front, 2 back, 3 all
    u8 light_set;      // index into LSET (MAT_LIGHT_SET)
    u8 fog;            // Fog index
    s8 priority;       // nw4r draw priority (lower first)
    // the TEV: per stage the color and alpha combiner words (BP 0xC0+2i / 0xC1+2i, without the
    // register address byte), TREF and KSEL words (BP 0x28+i/2, 0xF6+i/2: as GX packs two stages a
    // word; swap tables in KSEL 0..7 as GX)
    u32 color_env[MAX_TEV_STAGES];
    u32 alpha_env[MAX_TEV_STAGES];
    u32 tref[MAX_TEV_STAGES / 2];
    u32 ksel[8];
    s16 tev_reg[4][4];    // PREV, C0, C1, C2 (RGBA, 11-bit signed as GX)
    s16 tev_konst[4][4];  // K0-K3
    u32 alpha_compare;    // BP 0xF3
    u32 zmode;            // BP 0x40
    u32 blend;            // BP 0x41
    u32 const_alpha;      // BP 0x42 (0 if not set)
    // color channel 0 (XF 0x100E color, 0x1010 alpha control) and its registers
    u32 chan_color, chan_alpha;
    u8 mat_color[4], amb_color[4];  // RGBA
    TexMap maps[MAX_TEX_MAPS];
    TexGen texgens[MAX_TEXGENS];
    u16 tex_mtx_first, n_tex_mtx;  // TMTX
    u32 reserved[2];
};
static_assert(sizeof(Material) == 540, "Material");

// ---------------------------------------------------------------- geometry
enum VtxAttr : u8 {
    VA_POS = 1,     // f32 x3
    VA_NRM = 2,     // s8 x3 + 1 pad (unit normal * 127)
    VA_CLR = 4,     // u8 x4 RGBA
    VA_UV0 = 8,     // s16 x2 * uv_scale[0] (or f32 x2 if VB_UV_FLOAT)
    VA_UV1 = 16,    // s16 x2 * uv_scale[1]
    VA_POS16 = 32,  // instead of VA_POS: s16 x3 + 1 pad, value = s16 * pos_scale + pos_origin
};
enum VbFlags : u8 {
    VB_UV_FLOAT = 1,  // UVs are f32 x2 instead of s16 x2
};
struct VertexBuffer {
    u32 data;      // offset in the file (VDAT), 16-aligned
    u32 count;     // vertices
    u8 attrs;      // VtxAttr mask, in this order in each vertex: POS, NRM, CLR, UV0, UV1
    u8 stride;     // bytes a vertex
    u8 flags;      // VbFlags
    u8 pad;
    f32 uv_scale[2];  // s16 UVs: value = s16 * uv_scale
    f32 pos_origin[3];  // VA_POS16: value = s16 * pos_scale + pos_origin (else 0)
    f32 pos_scale;      // (else 0)
};
static_assert(sizeof(VertexBuffer) == 36, "VertexBuffer");

// a draw: one material, triangles (u16 indices into one vertex buffer)
struct Batch {
    u16 material;
    u16 vbuf;
    u32 first_index;   // in IDAT (u16 units)
    u32 n_indices;     // a multiple of 3
    f32 bbox[6];       // min xyz, max xyz (in the space of whoever owns it: world for cells,
                       // the node's for model meshes)
    u16 node;          // model meshes: the node it hangs from; cells: NONE16
    u16 flags;         // BatchFlags
};
enum BatchFlags : u16 {
    BATCH_DETAIL = 1,  // small detail (edge-blend strips, small props): drawn only nearer than
                       // Env::detail_distance
    BATCH_DYNAMIC = 2, // its vertices are made each frame (the packet's): the buffer holds the layout and a
                       // placeholder of the most it takes, the indices are fixed (the sea's trails)
};
static_assert(sizeof(Batch) == 40, "Batch");

// ---------------------------------------------------------------- models (node trees)
enum NodeFlags : u16 {
    NODE_VISIBLE = 1,
    NODE_BILLBOARD = 2,   // billboard (mode in Node::billboard)
};
struct Node {
    u32 name;          // STRS
    u16 parent;        // index within the model's nodes, NONE16 = root
    u16 flags;         // NodeFlags
    f32 scale[3], rotate[3], translate[3];  // rest pose (degrees, nw4r XYZ order)
    f32 bind[3][4];    // rest world matrix (model space)
    u8 billboard;      // nw4r billboard mode (0 none)
    u8 pad[3];
    u16 first_batch, n_batches;  // its meshes (BTCH)
};
static_assert(sizeof(Node) == 100, "Node");

struct Model {
    u32 name;          // STRS
    u16 first_node, n_nodes;   // NODE
    u16 first_batch, n_batches;  // BTCH (all its meshes, in draw order: opaque then translucent)
    f32 bbox[6];       // model space
    f32 radius;        // bounding sphere about the bbox centre
    u16 lod_next;      // a lower-detail model (NONE16), and the distance from which it is used
    u16 pad;
    f32 lod_distance;
};
static_assert(sizeof(Model) == 48, "Model");

// ---------------------------------------------------------------- the static world
// The terrain (and baked props) cut into cells on an x/z grid. Each cell lists its batches for up to
// three detail levels: level l is used from lod_distance[l] on (lod_distance[0] = 0).
constexpr int MAX_CELL_LODS = 3;
struct Cell {
    f32 bbox[6];
    u32 first[MAX_CELL_LODS];   // into CBAT
    u16 count[MAX_CELL_LODS];
    u16 n_lods;
    f32 lod_distance[MAX_CELL_LODS];
    u32 group;                  // a name (STRS) for the group (node) it came from, for visibility
};
static_assert(sizeof(Cell) == 60, "Cell");

enum InstFlags : u16 {
    INST_STATIC = 1,     // never moves (could be baked; kept as an instance for LOD)
    INST_COLLIDE = 2,    // has a trunk cylinder (CYLS)
    INST_ANIMATED = 4,   // plays `anim` (node tracks), looping
    INST_TRAIL = 8,      // leaves a trail on the sea as it moves (a boat: Sea::trail_batch)
};
struct Instance {
    u16 model;           // MODL
    u16 flags;           // InstFlags
    u16 anim;            // ANIM, NONE16
    u16 class_id;        // WSR CmnObj class (for the record)
    f32 mtx[3][4];       // model -> world
    f32 anim_offset;     // frames: where its loop starts (so copies don't move together)
    f32 cull_distance;   // not drawn beyond this (0: never culled by distance)
};
static_assert(sizeof(Instance) == 64, "Instance");

// ---------------------------------------------------------------- animation
enum TrackTarget : u8 {
    TT_NODE_SCALE_X = 0, TT_NODE_SCALE_Y, TT_NODE_SCALE_Z,
    TT_NODE_ROT_X, TT_NODE_ROT_Y, TT_NODE_ROT_Z,
    TT_NODE_TRANS_X, TT_NODE_TRANS_Y, TT_NODE_TRANS_Z,
    TT_TEXMTX_SCALE_S = 16, TT_TEXMTX_SCALE_T, TT_TEXMTX_ROTATE, TT_TEXMTX_TRANS_S, TT_TEXMTX_TRANS_T,
    TT_MAT_REG_R = 32,   // + component (R, G, B, A): Track::reg says which register
    TT_MAT_REG_G, TT_MAT_REG_B, TT_MAT_REG_A,
};
enum Interp : u8 { IP_STEP = 0, IP_LINEAR = 1, IP_HERMITE = 2 };
enum ColorReg : u8 {  // Track::reg for TT_MAT_REG_*: what a CLR0 track drives
    CR_MAT0 = 0, CR_MAT1, CR_AMB0, CR_AMB1, CR_TEV0 = 4, CR_TEV1, CR_TEV2, CR_KONST0 = 8, CR_KONST1,
    CR_KONST2, CR_KONST3,
};
struct Anim {
    u32 name;            // STRS
    f32 frames;          // length
    u8 loop;             // 1: loops
    u8 pad[3];
    u32 first_track, n_tracks;  // TRAK
};
static_assert(sizeof(Anim) == 20, "Anim");
struct Track {
    u8 target;           // TrackTarget
    u8 interp;           // Interp
    u8 reg;              // ColorReg for TT_MAT_REG_*; texmap index for TT_TEXMTX_*
    u8 pad;
    u16 index;           // node (within the model) / material / TexMtx index (absolute)
    u16 model;           // MODL index for node tracks (NONE16 otherwise)
    u32 first_key, n_keys;  // KEYS; n_keys == 1: constant
};
static_assert(sizeof(Track) == 16, "Track");
struct Key {
    f32 frame, value, slope;  // hermite slope (per frame) at the key
};
static_assert(sizeof(Key) == 12, "Key");

// ---------------------------------------------------------------- environment
enum LightKind : u8 { LK_DIRECTIONAL = 0, LK_POINT = 1 };
struct Light {
    u32 name;            // STRS
    u8 kind;             // LightKind
    u8 rgba[4];          // colour (intensity already multiplied in)
    u8 pad[3];
    f32 position[3];     // POINT: where; DIRECTIONAL: unused
    f32 direction[3];    // DIRECTIONAL: the direction light travels (unit)
};
static_assert(sizeof(Light) == 36, "Light");

struct LightSet {
    u32 name;            // STRS
    u16 lights[8];       // Light indices, NONE16 = unused
    u8 ambient[4];       // RGBA
};
static_assert(sizeof(LightSet) == 24, "LightSet");

enum FogType : u8 { FOG_NONE = 0, FOG_LINEAR = 2 };  // GX: 2 PERSP_LIN (only linear is used)
struct Fog {
    u8 type;             // FogType
    u8 rgba[4];
    u8 pad[3];
    f32 start, end;      // view depth (world units of the package)
};
static_assert(sizeof(Fog) == 16, "Fog");

enum MarkerKind : u32 {
    MK_SPAWN = 0,     // where the player starts (dir: facing)
    MK_LANDMARK = 1,  // a named place (dir unused)
    MK_CAMERA = 2,    // a camera preset: position = eye, dir = target point
    MK_WARP = 3,      // a place a menu offers to go to: the name is what it lists, position = the
                      // feet on the ground, dir = facing (unit); listed in the package's order
};
struct Marker {
    u32 name;            // STRS (e.g. "spawn", or a landmark's name)
    u32 kind;            // MarkerKind
    f32 position[3];
    f32 dir[3];          // spawn, warp: facing (unit); camera: target point
};
static_assert(sizeof(Marker) == 32, "Marker");

struct Collision {
    u32 name;            // STRS
    u32 data, size;      // a KCL file (big-endian, as shipped), in BLOB
};
static_assert(sizeof(Collision) == 12, "Collision");
struct Cylinder {
    f32 x, z, y0, y1, radius;
};
static_assert(sizeof(Cylinder) == 20, "Cylinder");

struct Env {
    u16 sky_model;       // MODL index of the sky dome (drawn first, follows the camera), NONE16
    u16 sky_anim;        // its ANIM (cloud scroll), NONE16
    u16 sea_model;       // MODL index of the sea (drawn with the cells), NONE16
    u16 sea_anim_srt, sea_anim_clr;
    u16 world_anim_srt, world_anim_clr;  // the cells' material animations (lava...), NONE16
    u16 pad;
    f32 world_bbox[6];
    f32 mii_units_per_unit;  // k: Wii no Ma's Mii units per package unit
    f32 cell_size;           // x/z grid of CELL
    u32 n_cells_x, n_cells_z;
    f32 grid_origin[2];      // x, z of cell (0, 0)'s corner
    u32 pvs_row_bytes;       // CVIS: bytes a row (bit j of row i: cell j may be seen from cell i's
                             // walkable space, at eye heights up to pvs_max_height over the ground);
                             // 0: no visibility rows
    f32 pvs_max_height;      // above that over the ground (a raised camera, flying) the rows don't apply
    f32 detail_distance;     // BATCH_DETAIL batches are culled beyond it (0: never)
    u32 reserved[5];
};
static_assert(sizeof(Env) == 96, "Env");

// ---------------------------------------------------------------- the sea (optional: SEC_SEAW)
// The sea's waves, which a grid of vertices that follows the camera draws over the open water near
// it (engine/g3d/sea.h) and which boats ride: a sum of SEA_WAVES travelling waves, flattened where
// the amplitude map says so (by the shores) and, each from its fade distances on, toward the grid's
// rim (where its vertices are too far apart for it). The section is the Sea record, then its
// amplitude map: amp_size[0] x amp_size[1] u8 (255: the waves whole, 0: flat), a row of x a z after
// another, each texel's value at its centre; outside amp_box the waves are whole.
constexpr int SEA_WAVES = 4;
struct SeaWave {
    f32 dir[2];          // the direction it travels (x, z; unit)
    f32 wavelength;      // units
    f32 amplitude;       // units: the crest over the mean
    f32 speed;           // its phase speed, units a second
    f32 phase;           // at the world's origin at time 0 (radians)
    f32 fade[2];         // its amplitude whole within fade[0] of the grid's centre, none beyond fade[1]
};
static_assert(sizeof(SeaWave) == 32, "SeaWave");

struct Sea {
    SeaWave waves[SEA_WAVES];
    f32 level;           // the sea's mean level (y): where MAT_SEA's surface lies
    f32 amp_box[4];      // x0, z0, x1, z1: what the amplitude map covers
    u16 amp_size[2];     // its texels across x and z
    u16 grid_batch;      // the wave grid: a batch whose vertex buffer holds its vertices about its centre
                         // (f32 positions at y 0) and whose material draws it (NONE16: no grid)
    u16 trail_batch;     // the trails behind moving boats (BATCH_DYNAMIC): its material and the indices
                         // of trail_boats ribbons of trail_samples pairs of vertices (NONE16: none)
    f32 grid_radius;     // the grid's outer ring (units)
    f32 grid_snap;       // its centre kept on a lattice of this (units): moved in steps
    f32 grid_height;     // drawn while the eye is no higher than this over the level
    f32 uv_map[2][3];    // the sea's texture coordinates at a world (x, z): u = [0] . (x, z, 1), v = [1] . ...
    f32 sun[3];          // toward the sun (unit): where the waves glint
    f32 shade[4];        // the grid's shading: diffuse, sky reflection, glint, glint exponent
    u16 trail_boats, trail_samples;  // the trail batch's ribbons, and the pairs of vertices each has
    f32 trail_spacing;   // a ribbon's samples this far apart along the path (units)
    f32 trail_life;      // seconds a sample lasts
    f32 trail_spread;    // units a second a ribbon's half width grows behind the boat
    f32 trail_speed;     // units a second at which a boat leaves its trail whole (slower: fainter)
    u32 reserved[4];
};
static_assert(sizeof(Sea) == 256, "Sea");

}  // namespace engine::g3d::fmt
