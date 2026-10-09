// The GX renderer on the 3DS GPU (PICA200, through citro3d).
//
// The GX transform unit runs as the PICA's vertex shader (shaders/gx.v.pica): the CPU only decodes
// the vertex stream into model-space vertices (runtime/hw/gx/tnl.cpp), written once into the
// vertex buffer with the palette slots of their matrices, and sets the shader's uniforms (the
// projection, lights, material, texgens; matrix rows in the palette as the draws need them).
// Triangles go to the PICA as 16-bit indices (quads, strips and fans expanded), culled by the GPU,
// and draws in a row with unchanged GX state and uniforms as one.
// When a GX register changes, the pixel pipeline state becomes PICA state:
//   * the GX viewport (in the 640x456 frame) folded into one matrix with the scale to the
//     400x240 top screen, the rotated render target and the GX depth range, folded in turn into
//     the shader's projection;
//   * the TEV stages become texture combiner stages (tev_to_texenv: GX's d +- lerp(a, b, c)
//     with bias and scale maps to one or two PICA stages; register constants and konst colors
//     use each stage's constant color, results kept across stages use the combiner buffer);
//   * up to three textures (decoded by runtime/hw/gx/texture.cpp, then tiled for the PICA);
//   * alpha test, depth, blending, logic op and write masks; culling is done on the CPU.
// A frame starts at the first draw after the GX "draw done" and ends at the next draw done.
// The GPU draws a frame while the next one's commands are made (NOMA_PICA_OVERLAP, below): memory
// that a frame's commands read and the next frame would write is doubled.
// EFB copies to the frame buffer need nothing (the frame is already on screen); for a copy to
// a texture the GPU finishes the frame so far, the render target is read back and converted
// like the host renderer's EFB (scaled from 400x240), and the frame goes on.
// A layout drawn natively (engine/lyt/draw.h) comes as one packet of quads, each run of them with
// the GX state it would have been drawn with as register values: drawn through the same state
// machine, caches and batches as the GX draws (PicaRenderer::lyt).
#include <3ds.h>
#include <citro3d.h>
#include <malloc.h>

#include <sys/stat.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "engine/g3d/native_draw.h"
#include "engine/input/remote.h"
#include "engine/gx/tev_combiner.h"
#include "engine/lyt/draw.h"
#include "runtime/hw/gx/raster.h"
#include "runtime/hw/gx/regs.h"
#include "runtime/hw/gx/texture.h"
#include "runtime/hw/gx/tnl.h"
#include "runtime/hw/gx/vertex.h"
#include "runtime/runtime.h"
#include "gx_pica.h"
#include "pica_tex.h"

extern "C" u32 __ctru_heap_size, __ctru_linear_heap, __ctru_linear_heap_size;
extern "C" const uint8_t gx_shbin[];
extern "C" const size_t gx_shbin_size;
// citro3d's own: what its setters marked goes into the command list (C3D_DrawElements runs it
// first). A native draw writes the GPU's registers directly and runs it before it starts, so that
// what citro3d still has to write -- a frame's frame buffer, viewport and scissor, before the
// frame's first draw -- can't come after the native draw's state and replace it.
extern "C" void C3Di_UpdateContext(void);

namespace guest {

namespace {

const float FRAME_W = 640.0f, FRAME_H = 456.0f;  // the game's frame buffer
// The vertex pool: one 16-bit index space. [0, s_imm_vertices) holds the frame's immediate draws
// (written again every frame), the rest the display lists' vertices, kept from frame to frame
// (the display-list cache below). The palette slots are a buffer of their own beside it, so that
// a list's vertices stay untouched when its matrices get other slots.
// Sized from the scenes, about twice what they use at most: 9K immediate vertices and 23K indices
// in a frame in the room (12.5K immediate vertices on Wuhu Island), 10.6K list vertices kept in the
// room, 14.7K on the island (linear heap is the Old 3DS's scarcest memory: textures, the guest's
// MEM1, the sound). With frames overlapping the GPU the immediate part is two halves, a frame's
// each, so that it is larger: 2.5 MB + 160 KB of slots one frame after the other, 3.25 MB + 2 x
// 208 KB overlapping.
const u32 IMM_SERIAL = 20 << 10, IMM_OVERLAP = 32 << 10, CACHE_VERTICES = 20 << 10;
u32 s_imm_vertices = IMM_SERIAL, s_pool_vertices = IMM_SERIAL + CACHE_VERTICES;
const u32 IBUF_INDICES = 128 << 10;  // 256 KB

// NOMA_PICA_OVERLAP (default 1; 0: one after the other, as before). citro3d starts the GPU on a
// frame only at its end (C3D_FrameEnd runs its queue: the clears, the command list, the display
// transfers) and C3D_FrameBegin waits for that queue to finish, so with a frame begun at its first
// draw the GPU's drawing came after the commands' making, never beside it: on a New 3DS 12.9 ms of
// commands and 9.9 ms of drawing, a room frame of 22.9 ms (noma.log of 2026-10-05). Overlapping, a
// frame is made outside citro3d's frame, into its own half of citro3d's command buffer, and only
// its end waits for the frame before (its citro3d frame then begun, its clears queued, its list
// split off and its transfers queued and run, as C3D_FrameEnd does). What the frame before's
// commands read is not written meanwhile: there are two of the command buffer, the immediate
// vertices and indices and the palette slots, a frame using those of its parity; a list's vertices
// made again while the frame before may still draw them go into a second place of their own
// (DlRecord::alt), a texture made again into new memory; textures, packages and memory freed wait
// until the GPU has finished every frame that may use them (s_done). A frame that needs more than
// its half takes the other one too, once the frame before has finished (spill), and the next frame
// then waits for it at its start. NOMA_PICA_STEP's pieces need the serial order: it turns the
// overlap off.
bool s_overlap = false;
u32* s_cmd_half[2];       // the halves of citro3d's command buffer (overlapping)
u32 s_cmd_half_words = 0;
u32* s_cmd_base = nullptr;  // citro3d's command buffer, whole
u32 s_cmd_words = 0;
// Every frame before s_done has finished on the GPU (frames count from 0 as P.frame does). One
// after the other: the frame being made has waited for all of them. Overlapping: at least the frame
// before last, as the frame before was sent only once that one had finished.
u32 s_done = 0;
// a clear the frame asked for (overlapping): the memory fill goes into citro3d's queue before the
// frame's commands, when they are sent, as one after the other it went in before them
struct PendingClear {
    C3D_RenderTarget* target;
    u32 color, depth;
};
PendingClear s_clears[2];
u32 s_nclears = 0;

// NOMA_TOP_SSAA=1: the top screen drawn at twice its size each way (a 480x800 target) and box-
// filtered down to the screen by the display transfer (GX_TRANSFER_SCALE_XY): smoother edges and
// text, and the frame's copies to textures (the Mii faces) made from twice the samples each way;
// four times the pixels for the GPU to fill and 3 MB of VRAM. Off by default (the plan's P2
// experiment, to time on a console); without the VRAM it falls back to 400x240.
u32 s_top_scale = 1;
#define DISPLAY_TRANSFER_FLAGS                                                                  \
    (GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |           \
     GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | \
     GX_TRANSFER_SCALING(s_top_scale == 2 ? GX_TRANSFER_SCALE_XY : GX_TRANSFER_SCALE_NO))
// the bottom screen's frame buffer is the log console's: RGB565. NOMA_BOTTOM_SSAA=1: the bottom
// screen drawn at twice its size each way (a 480x640 target, 2.4 MB of VRAM) and filtered down,
// as NOMA_TOP_SSAA does for the top one (the two together don't fit beside the island's shadow map)
u32 s_bottom_scale = 1;
#define BOTTOM_TRANSFER_FLAGS                                                                     \
    (GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |             \
     GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB565) | \
     GX_TRANSFER_SCALING(s_bottom_scale == 2 ? GX_TRANSFER_SCALE_XY : GX_TRANSFER_SCALE_NO))

using PVertex = GxTnlVertex;  // the vertex shader's inputs (tnl.h)
static_assert(sizeof(PVertex) == 64, "PVertex layout");

// ---- the display-list cache: a list's draws, with their vertices in the pool, found again by
// the list and the draw command's offset in it; valid while the draw's bytes, the vertex arrays
// they index, the CP registers they were decoded with and what the shader wants of a vertex (the
// recipe) stay the same. A list the game writes again every frame (two in the room: 132 draws)
// mostly keeps its draws' bytes: they are compared when the list changed. The arrays are watched
// by their pages' write generations, and when those moved, by their bytes (gx_tnl_array_hash): in
// the room ~150 list draws a frame read arrays in pages the game flushes every frame for other
// data, and were made again every frame with the same bytes (a host census, NOMA_INPUT=4:T(160;120)).
struct DlRange {
    u32 attr, addr, size, gen;
    u64 hash;  // the elements the draw reads, as they were at `gen`
};
struct DlRecord {
    u32 offset = 0;            // the draw command's offset in the list
    u32 first = 0, count = 0;  // pool vertices
    u32 alt = 0;               // a second place for them (overlapping: 0 none), see the cache's lookup
    u32 prim = 0, vat = 0;
    u32 regs[5] = {};          // VCD lo/hi, the VAT's A/B/C
    u32 nranges = 0;
    DlRange ranges[12];
    u32 arrays[12][2];         // base, stride
    u8 recipe[12] = {};
    // the slots all its vertices were given (fixed-matrix draws) in each of the palette slots'
    // buffers (one after the other only the first), where valid (bit per buffer); the buffer it
    // was last drawn with (its slots there: the next frame's hint)
    GxTnlSlots slots[2] = {};
    u8 slots_valid = 0, slots_last = 0;
    u32 slots_frame = 0;       // the frame it was last drawn in (0: never)
    u64 data_hash = 0;         // its vertex data's bytes in the list ...
    u32 list_gen = 0;          // ... as they were at this write generation of the list
};
// an offset's records at most, drawn with other registers or arrays: in the room a list of 9 draws
// is drawn about 6 times a frame with other arrays (12 sets of them in a run); with 4 kept a draw,
// ~20 draws a frame were made again every frame
const u32 DL_VARIANTS = 16;
struct DlEntry {
    u32 addr = 0, size = 0;  // the list (0: free)
    u32 gen = 0;             // its bytes' write generation when the records were made
    u32 last_frame = 0;
    std::vector<DlRecord> recs;
};
const u32 DL_ENTRIES = 1024;

struct TexEntry {
    C3D_Tex tex;
    GPU_TEXCOLOR fmt = GPU_RGBA8;
    u32 version = 0;
    u32 last_used = 0;
    float sx = 1, sy = 1;  // texture coordinate scale (image padded to a power of two)
    u32 wrap = 0xFFFF;
    bool mips = false;     // with its smaller images (texture_for)
};

struct State {
    bool ready = false;
    C3D_RenderTarget* top = nullptr;
    // The bottom screen (GxRenderer::screen): its own target, drawn on in the frames a scene puts
    // something there (the rest of the time the log console has it); what is being drawn on now
    // (SCREEN_TOP, SCREEN_BOTTOM), and for the bottom screen the frame's rectangle (EFB pixels)
    // that fills it.
    C3D_RenderTarget* bottom = nullptr;
    u32 screen = 0;
    bool bottom_drawn = false;
    f32 region[4] = {0, 0, 640, 456};
    DVLB_s* dvlb = nullptr;
    shaderProgram_s prog;
    int u_nlights = -1;         // the shader's integer uniform
    f32 clip[4][4] = {};        // GX clip space -> PICA clip space (set_xform), folded into the projection
    // the shader's uniforms as the GPU has them: the pending batch's constants, the palette,
    // and our own copy of the float uniforms c0-c94 in the GPU's word order with what changed
    // (the shader's constant c95 is citro3d's to send)
    GxTnlConsts consts;
    bool consts_valid = false;
    u32 uni[96][4];
    u64 uni_dirty_lo = 0;       // c0-c63
    u32 uni_dirty_hi = 0;       // c64-c95
    bool uni_resend = true;     // everything again (the start, after an applet had the GPU)
    bool bools_dirty = true, ints_dirty = true;
    // citro3d's own state (combiners, textures, depth, scissor, culling, the frame buffer)
    // changed: the next draw call goes through C3D_DrawElements, which binds it
    bool c3d_touched = true;
    u32 consts_plan = 0;        // the serial of the plan the constants came from (0: none)
    u32 setup_serial = 1, clip_serial = 1;  // moved on when setup / clip change (the plan cache's keys)
    GxTnlPalette pal;
    u32 cull = ~0u;             // the GX cull mode the GPU is set to
    PVertex* vbuf = nullptr;   // the pool
    // the palette slots, beside the pool: the frame's buffer (overlapping, by the frame's parity:
    // a list's vertices are drawn by the frame before with the slots it wrote)
    GxTnlSlots* slots = nullptr;
    GxTnlSlots* slot_buf[2] = {};
    u32 sbuf = 0;
    GxTnlRows* rows = nullptr;  // (CPU only: the lists' vertices' matrix rows)
    u16* ibuf = nullptr;
    // the frame's immediate vertices and indices: where the next go, the end of the room they have
    // (overlapping: the frame's half, or the other's after a spill), the first not yet flushed to
    // memory, and how many were used this frame before the cursors last moved
    u32 vused = 0, iused = 0;
    u32 vend = 0, iend = 0;
    u32 vflush = 0, iflush = 0;
    u32 vstart = 0, istart = 0;  // (where the cursors last started)
    u32 vframe = 0, iframe = 0;
    bool spilled = false;      // the frame took the other half too (overlapping)
    u32 cache_next = 0;        // the pool's first free cache vertex
    bool cache_full = false;   // (lists go the immediate way until the cache starts over)
    u32 vmax = 0, imax = 0;    // immediate vertices and indices a frame at most, since the last report
    u32 draws = 0, batches = 0, setups = 0;  // this frame
    u32 cached_draws = 0, cached_vertices = 0, built_vertices = 0;  // this frame
    u32 rebuilt_arrays = 0, rebuilt_regs = 0, drawn_twice = 0, no_room = 0;  // why list draws were built
    u32 kept_by_bytes = 0;     // list draws whose arrays' pages were written, their bytes the same
    u32 cache_made = 0, lists_rewritten = 0, lists_new = 0;  // pool vertices given out; lists whose bytes changed, new
    // the frame's times (ticks): its commands' making (from its start to its sending), the waits
    // for the GPU (at its start one after the other, at its end overlapping); since the last report:
    // their sums, frames, spills, the most command words a frame
    u64 rec_t0 = 0, rec_ticks = 0, wait_ticks = 0, wait_begin = 0;
    u64 rec_sum = 0, wait_sum = 0;
    u32 timed_frames = 0, spills = 0, cmd_words_max = 0;
    u32 cmd_words = 0;         // this frame's command words sent so far
    // what the GPU must see of memory the CPU wrote this frame: slot and cache vertex ranges
    u32 slots_lo = ~0u, slots_hi = 0, cache_lo = ~0u, cache_hi = 0;
    DlEntry* dl = nullptr;     // the display-list cache
    u32 dl_addr = 0, dl_offset = ~0u;  // the list call being drawn (its generation checked)
    bool in_frame = false;
    bool resume = false;  // the frame goes on (its first part went to the GPU before an applet)
    u32 frame = 0;
    std::unordered_map<u32, TexEntry> textures;  // by GxTexture id
    // textures replaced, freed once the GPU has finished the frame they were replaced in
    struct Dead {
        C3D_Tex tex;
        u32 frame;
    };
    std::vector<Dead> dead;
    u32 warned = 0;
    u32 evicted = 0, failed = 0;                 // textures dropped for memory; not made
    // The PICA state a draw needs comes from GX registers: a part of it is worked out again only
    // after one of its registers changed (the decoder's g_gx.bp_dirty, the viewport's serial),
    // and the draws in between go to the GPU as one draw, as do draws after a change that leaves
    // the PICA state as it was.
    u32 viewport_serial = ~0u;  // g_gx.xf_viewport_serial the transform was set up for
    bool all_dirty = true;      // every part of the state (a new frame, after a clear)
    u32 batch_first = 0, batch_count = 0;  // indices drawn with it, not submitted yet
    GxDrawSetup setup;                     // its texture coordinates
    // what the PICA has now (env_valid false: unknown, set again in full)
    bool env_valid = false;
    C3D_TexEnv env[6];
    u32 buf_rgb = 0, buf_a = 0;
    C3D_Tex* tex[3] = {};
    C3D_Tex tex_was[3];  // as they were bound (a texture made again or rewrapped is bound again)
    // the command list being written has native draws, and draws through citro3d (which marks the
    // list as drawn to: its end then writes the frame buffer's cache out)
    bool native_drew = false, c3d_drew = false;
};
State P;

// linear memory to free once the GPU has finished the command lists so far (pica_free_after_frame;
// a package is released on the GPU's thread, or on the CPU's while the GPU's waits): with the
// frame being made then, freed once it has finished
struct FreeLater {
    void* p;
    u32 frame;
};
LightLock s_free_lock;
std::vector<FreeLater> s_free_later;
__attribute__((constructor(103))) void init_free_lock() { LightLock_Init(&s_free_lock); }

// the pixel pipeline's state as last set (see "The pixel pipeline's state written directly")
struct Direct {
    u32 alpha_test = 0, stencil = 0xFF000010, stencil_op = 0, depth = 0;  // FRAGOP_ALPHA_TEST..DEPTH_COLOR_MASK
    u32 blend = 0, logic = 0, fragop = 0xE40100;
    u32 scissor[3] = {};
    u32 texenv_buf = 0;
    u32 cull = GPU_CULL_BACK_CCW;
    C3D_Tex* bound[3] = {};
    C3D_Tex* c3d_bound[3] = {};  // what citro3d's own context holds per unit (bound through it)
} D;
// NOMA_PICA_C3D=1 (env.txt): every draw and state change through citro3d, none written directly
// (to tell the direct writes from the rest when the GPU misbehaves)
const bool s_c3d_only = getenv("NOMA_PICA_C3D") != nullptr;
inline bool direct() { return !P.c3d_touched && !s_c3d_only; }

// ---- A real console's GPU stopped where the emulator's went on (the first frame, never
// finished: the shader left outputs unwritten): where the renderer waits for the GPU is noted, and
// a wait of more than 3 s is logged (pica_watchdog, from the I/O thread). NOMA_DUMP_CMDLIST=N: the first N
// frames' command lists to cmdlist.bin, each before it goes to the GPU, after a record of the
// buffers' physical addresses (tools/pica_cmdlist.py reads it).
std::atomic<const char*> s_waiting{nullptr};
std::atomic<u64> s_wait_since{0};
std::atomic<u32> s_wait_frame{0};
struct GpuWait {
    explicit GpuWait(const char* what) {
        s_wait_since.store(svcGetSystemTick());
        s_waiting.store(what);
    }
    ~GpuWait() { s_waiting.store(nullptr); }
};
const int s_cmd_frames = getenv("NOMA_DUMP_CMDLIST") ? atoi(getenv("NOMA_DUMP_CMDLIST")) : 0;
FILE* s_cmd_dump;
u32* s_frame_list;  // where the open frame's commands start

// The base the vertex and index buffers' offsets count from (GPUREG_ATTRIBBUFFERS_LOC): the lowest of
// the buffers. The offsets have 28 bits; citro3d's base, 0x18000000 (VRAM), reaches only the FCRAM
// below 0x28000000, and a New 3DS's 178 MB mode puts the linear heap above it (the emulator doesn't
// mask the offsets).
u32 s_buf_base = 0x18000000;

// ---- NOMA_GX_TIMERS=N (env.txt): the GPU thread's time in each part of the draw path, as
// milliseconds per frame over N frames, with counts (svcGetSystemTick around every part costs
// something itself, so only when asked); the profiler's scopes cover the same parts for free.
// LYT is the native layouts' packets' own time (PicaRenderer::lyt: their vertices and indices, the
// projection, the palette); the parts of their state are counted in their own sections (tev,
// textures, upload, bind, pixel, consts, flush, xform), so that the sections add up to the frame.
// WAIT is the waits for the GPU (to finish the frame before: at a frame's start one after the
// other, at its end overlapping), out of the sections they happen in (end, readback, lyt).
struct Timers {
    enum { PLAN, CONSTS, VERTICES, SLOTS, SUBMIT, FLUSH, TEV, TEXTURES, UPLOAD, BIND, PIXEL, XFORM, FRAME, END, CLEAR, READBACK, COPY, LYT, WAIT, N };
    u64 ticks[N] = {};
    u32 calls[N] = {};
    u64 frame_ticks[N] = {};  // the sections' ticks when the frame started (a slow frame is logged on its own)
    u32 spike_ms = 100;       // NOMA_GX_SPIKE_MS
    u32 uploads = 0, upload_texels = 0, copies = 0;  // this frame
    u64 frame_t0 = 0, wall = 0;
    u32 frames = 0;
    u32 consts_vectors = 0, pal_slots = 0, pieces = 0, tev_misses = 0, plan_misses0 = 0;
    GxDecodeCounts dec0 = {};
    GxTnlPalStats pal0 = {};
    u32 cache_made = 0, vary_draws = 0, slot_rewrites = 0, slot_vertices = 0;
    // the native layouts: packets, their runs of quads, the quads drawn, the batches they went to
    // the GPU in, quads left out (no room in the vertex pool)
    u32 lyt_packets = 0, lyt_runs = 0, lyt_quads = 0, lyt_batches = 0, lyt_dropped = 0;
    int every = -2;
} T;
inline bool timers_on() {
    if (T.every == -2)
        T.every = getenv("NOMA_GX_TIMERS") ? std::max(1, atoi(getenv("NOMA_GX_TIMERS"))) : 0;
    if (T.every > 0 && getenv("NOMA_GX_SPIKE_MS"))
        T.spike_ms = (u32)std::max(1, atoi(getenv("NOMA_GX_SPIKE_MS")));
    return T.every > 0;
}
struct Timed {
    int k;
    u64 t0, wait0;
    explicit Timed(int which)
        : k(which), t0(timers_on() ? svcGetSystemTick() : 0), wait0(T.ticks[Timers::WAIT]) {}
    ~Timed() {
        if (!t0)
            return;
        T.ticks[k] += svcGetSystemTick() - t0, T.calls[k]++;
        if (k != Timers::WAIT && k != Timers::LYT)  // (a wait inside is WAIT's; LYT takes off all it nests)
            T.ticks[k] -= T.ticks[Timers::WAIT] - wait0;
    }
};
static const char* const k_timer_names[Timers::N] = {"plan", "consts", "vertices", "slots", "submit", "flush", "tev",
                                                     "textures", "upload", "bind", "pixel", "xform", "frame", "end", "clear",
                                                     "readback", "copy", "lyt", "wait"};

// the time in every section but LYT so far (what a packet spent in them comes off its own)
inline u64 timers_but_lyt() {
    u64 t = 0;
    for (int k = 0; k < Timers::N; k++)
        if (k != Timers::LYT)
            t += T.ticks[k];
    return t;
}

// at a frame's end: the frame's wall time; every N frames the averages
void timers_frame_end() {
    if (!timers_on())
        return;
    u64 now = svcGetSystemTick();
    if (T.frame_t0) {
        T.wall += now - T.frame_t0, T.frames++;
        // a slow frame: where its own time went
        const double fms = (double)(now - T.frame_t0) * 1000.0 / SYSCLOCK_ARM11;
        if (fms >= (double)T.spike_ms) {
            char line[512];
            int n = snprintf(line, sizeof(line), "gx spike: frame %u took %.0f ms:", P.frame, fms);
            for (int k = 0; k < Timers::N; k++) {
                double ms = (double)(T.ticks[k] - T.frame_ticks[k]) * 1000.0 / SYSCLOCK_ARM11;
                if (ms >= 1.0)
                    n += snprintf(line + n, sizeof(line) - n, " %s %.0f", k_timer_names[k], ms);
            }
            snprintf(line + n, sizeof(line) - n, "; %u textures made (%u KB of texels), %u EFB copies", T.uploads,
                     T.upload_texels >> 10, T.copies);
            log("%s", line);
        }
    }
    T.frame_t0 = now;
    memcpy(T.frame_ticks, T.ticks, sizeof(T.ticks));
    T.uploads = T.upload_texels = T.copies = 0;
    if (T.frames < (u32)T.every)
        return;
    const double ms = 1000.0 / SYSCLOCK_ARM11 / T.frames;
    char line[1536];
    int n = snprintf(line, sizeof(line), "gx timers: %u frames, %.1f ms each:", T.frames, (double)T.wall * ms);
    u64 sum = 0;
    for (int k = 0; k < Timers::N; k++) {
        sum += T.ticks[k];
        if (T.calls[k])
            n += snprintf(line + n, sizeof(line) - n, " %s %.2f (%u)", k_timer_names[k], (double)T.ticks[k] * ms,
                          T.calls[k] / T.frames);
    }
    GxDecodeCounts d = gx_decode_counts();
    GxTnlPalStats ps = gx_tnl_pal_stats();
    n += snprintf(line + n, sizeof(line) - n, "; rest %.2f | per frame: %u draws %u vertices %u bp %u xf %u cp %u lists, %u const vectors, %u palette slots, %u pieces, %u tev misses, %u plan misses",
             (double)(T.wall > sum ? T.wall - sum : 0) * ms, (d.draws - T.dec0.draws) / T.frames,
             (d.vertices - T.dec0.vertices) / T.frames, (d.bp - T.dec0.bp) / T.frames, (d.xf - T.dec0.xf) / T.frames,
             (d.cp - T.dec0.cp) / T.frames, (d.dl - T.dec0.dl) / T.frames, T.consts_vectors / T.frames,
             T.pal_slots / T.frames, T.pieces / T.frames, T.tev_misses / T.frames,
             (gx_tnl_plan_misses() - T.plan_misses0) / T.frames);
    n += snprintf(line + n, sizeof(line) - n, "; palette %u fills %u checks (%u stale) %u searches %u reused; %u list vertices made; "
                  "cached draws: %u with matrices per vertex, %u fixed with slots rewritten (%u vertices)",
                  (ps.fills - T.pal0.fills) / T.frames, (ps.checks - T.pal0.checks) / T.frames,
                  (ps.stale - T.pal0.stale) / T.frames, (ps.scans - T.pal0.scans) / T.frames,
                  (ps.reused - T.pal0.reused) / T.frames, T.cache_made / T.frames,
                  T.vary_draws / T.frames, T.slot_rewrites / T.frames, T.slot_vertices / T.frames);
    snprintf(line + n, sizeof(line) - n, "; layouts: %u packets, %u runs, %u quads in %u batches (%u quads in all left out for room)",
             T.lyt_packets / T.frames, T.lyt_runs / T.frames, T.lyt_quads / T.frames, T.lyt_batches / T.frames,
             T.lyt_dropped);
    T.pal0 = ps;
    T.cache_made = T.vary_draws = T.slot_rewrites = T.slot_vertices = 0;
    T.lyt_packets = T.lyt_runs = T.lyt_quads = T.lyt_batches = T.lyt_dropped = 0;
    log("%s", line);
    memset(T.ticks, 0, sizeof(T.ticks));
    memset(T.calls, 0, sizeof(T.calls));
    memset(T.frame_ticks, 0, sizeof(T.frame_ticks));
    T.wall = 0, T.frames = 0;
    T.consts_vectors = T.pal_slots = T.pieces = T.tev_misses = 0;
    T.dec0 = d;
    T.plan_misses0 = gx_tnl_plan_misses();
}

// a float uniform vector into our copy (the GPU takes the components w first)
inline void set_fvec(int reg, const f32* v) {
    u32* d = P.uni[reg];
    memcpy(&d[3], &v[0], 4), memcpy(&d[2], &v[1], 4), memcpy(&d[1], &v[2], 4), memcpy(&d[0], &v[3], 4);
    if (reg < 64)
        P.uni_dirty_lo |= 1ull << reg;
    else
        P.uni_dirty_hi |= 1u << (reg - 64);
}

// ---- Draw calls of our own. citro3d's C3D_DrawElements runs its whole context update every
// time (each of its flags, both shaders' 96 uniform dirty flags); ours writes the GPU's draw
// commands directly, after the uniforms it knows have changed. citro3d's state is still bound
// through its context: a draw after any of it changed goes through C3D_DrawElements once (which
// also marks the frame as drawn to, for its end-of-frame flush).
void upload_uniforms() {
    if (P.uni_resend) {
        P.uni_dirty_lo = ~0ull;
        P.uni_dirty_hi = (1u << (TNL_PAL_BASE + TNL_PAL_SLOTS - 64)) - 1;
        P.bools_dirty = P.ints_dirty = true;
        P.uni_resend = false;
    }
    // runs of changed vectors (c0-c94: c95 is citro3d's), found a run at a time
    u64 lo = P.uni_dirty_lo;
    u32 hi = P.uni_dirty_hi & ((1u << (95 - 64)) - 1);
    while (lo | hi) {
        u32 i = lo ? (u32)__builtin_ctzll(lo) : 64 + (u32)__builtin_ctz(hi);
        // the run's end: the first clear bit from i on
        u32 j;
        if (i < 64) {
            u64 ones = ~lo & (~0ull << i);
            j = ones ? (u32)__builtin_ctzll(ones) : 64;
            lo &= j < 64 ? ~0ull << j : 0;
            if (j == 64) {  // (a run on into c64 on)
                u32 more = ~hi;
                u32 k = more ? (u32)__builtin_ctz(more) : 32;
                j = 64 + k;
                hi &= k < 32 ? ~0u << k : 0;
            }
        } else {
            u32 b = i - 64;
            u32 ones = ~hi & (~0u << b);
            u32 k = ones ? (u32)__builtin_ctz(ones) : 32;
            j = 64 + k;
            hi &= k < 32 ? ~0u << k : 0;
        }
        GPUCMD_AddWrite(GPUREG_VSH_FLOATUNIFORM_CONFIG, 0x80000000 | i);
        GPUCMD_AddWrites(GPUREG_VSH_FLOATUNIFORM_DATA, &P.uni[i][0], 4 * (j - i));
    }
    P.uni_dirty_lo = 0, P.uni_dirty_hi = 0;
    if (P.bools_dirty) {
        GPUCMD_AddWrite(GPUREG_VSH_BOOLUNIFORM, 0x7FFF0000 | (P.consts.bools & 0xFFFF));
        P.bools_dirty = false;
    }
    if (P.ints_dirty) {
        u32 nl = P.consts.nlights ? P.consts.nlights - 1 : 0;
        GPUCMD_AddWrite(GPUREG_VSH_INTUNIFORM_I0, nl | 4u << 16);
        P.ints_dirty = false;
    }
}

// NOMA_CHECK_PICA_STATE=1: every draw's pixel-pipeline registers as the renderer meant them; at a
// command list's end the list is decoded as the GPU will read it and the registers at each draw
// compared (the state written directly must be what citro3d would have written)
struct PicaSnap {
    u32 tests[4], blend, logic, fragop, cull, scissor[3], texenv[6][5], texenv_buf, tex_addr[3];
    bool direct;
    bool native;  // a native draw's (g3d_pica.cpp): its state is the backend's, not compared
};
std::vector<PicaSnap> s_snaps;
int s_check_state = -1;
u32 s_state_draws, s_state_bad;
bool check_state_on() {
    if (s_check_state < 0)
        s_check_state = getenv("NOMA_CHECK_PICA_STATE") ? 1 : 0;
    return s_check_state > 0;
}
void snap_state() {
    PicaSnap n;
    n.tests[0] = D.alpha_test, n.tests[1] = D.stencil, n.tests[2] = D.stencil_op, n.tests[3] = D.depth;
    n.blend = D.blend, n.logic = D.logic, n.fragop = D.fragop & 0xFFFFFF, n.cull = D.cull;
    memcpy(n.scissor, D.scissor, sizeof(n.scissor));
    for (int i = 0; i < 6; i++)
        memcpy(n.texenv[i], &P.env[i], sizeof(n.texenv[i]));
    n.texenv_buf = D.texenv_buf & 0xFF00;
    n.direct = direct();
    n.native = false;
    for (int u = 0; u < 3; u++)
        n.tex_addr[u] = P.tex[u] && P.env_valid ? osConvertVirtToPhys(P.tex[u]->data) >> 3 : 0;
    s_snaps.push_back(n);
}
// the command list since the last split, decoded; the snapshots compared at each DRAWELEMENTS
void check_state_list() {
    static u32 reg[0x300];
    u32 *buf, size, off;
    GPUCMD_GetBuffer(&buf, &size, &off);
    size_t k = 0;
    for (u32 i = 0; i + 1 < off;) {
        const u32 hdr = buf[i + 1];
        u32 id = hdr & 0xFFFF;
        const u32 nib = (hdr >> 16) & 0xF, extra = (hdr >> 20) & 0x7FF;
        const bool seq = hdr >> 31;
        const u32 mask = (nib & 1 ? 0xFF : 0) | (nib & 2 ? 0xFF00 : 0) | (nib & 4 ? 0xFF0000 : 0) | (nib & 8 ? 0xFF000000u : 0);
        for (u32 j = 0; j <= extra; j++) {
            const u32 v = j == 0 ? buf[i] : buf[i + 1 + j];
            if (id < 0x300)
                reg[id] = (reg[id] & ~mask) | (v & mask);
            if (id == GPUREG_DRAWELEMENTS && k < s_snaps.size() && s_snaps[k].native) {
                k++;  // (a native draw)
            } else if (id == GPUREG_DRAWELEMENTS && k < s_snaps.size()) {
                const PicaSnap& n = s_snaps[k++];
                s_state_draws++;
                u32 bad = 0;
                static u8 s_reported[0x300];  // (each register's first difference is logged)
                auto cmp = [&](u32 r, u32 want, u32 m = ~0u) {
                    if ((reg[r] & m) == (want & m))
                        return;
                    bad++;
                    if (!s_reported[r]++)
                        log("pica state check: frame %u draw %u: register %03X is %08X, meant %08X (c3d %s)", P.frame,
                            (u32)k - 1, r, reg[r] & m, want & m, n.direct ? "skipped" : "used");
                };
                for (int t = 0; t < 4; t++)
                    cmp(GPUREG_FRAGOP_ALPHA_TEST + t, n.tests[t]);
                cmp(GPUREG_BLEND_FUNC, n.blend);
                cmp(GPUREG_LOGIC_OP, n.logic, 0xF);
                cmp(GPUREG_COLOR_OPERATION, n.fragop, 0xFFFFFF);
                cmp(GPUREG_FACECULLING_CONFIG, n.cull, 3);
                cmp(GPUREG_SCISSORTEST_MODE, n.scissor[0], 3);
                if (n.scissor[0] != GPU_SCISSOR_DISABLE)
                    cmp(GPUREG_SCISSORTEST_MODE + 1, n.scissor[1]), cmp(GPUREG_SCISSORTEST_MODE + 2, n.scissor[2]);
                for (int e = 0; e < 6; e++)
                    for (int w = 0; w < 5; w++)
                        cmp(GPUREG_TEXENV0_SOURCE + (e >= 4 ? e + 2 : e) * 8 + w, n.texenv[e][w]);
                cmp(GPUREG_TEXENV_UPDATE_BUFFER, n.texenv_buf, 0xFF00);
                static const u32 k_addr[3] = {GPUREG_TEXUNIT0_ADDR1, GPUREG_TEXUNIT1_ADDR, GPUREG_TEXUNIT2_ADDR};
                for (int u = 0; u < 3; u++)
                    if (n.tex_addr[u])
                        cmp(k_addr[u], n.tex_addr[u], 0x0FFFFFFF);
                s_state_bad += bad != 0;
            }
            if (seq)
                id++;
        }
        i += 2 + extra;
        i += i & 1;  // (commands start at 8-byte boundaries)
    }
    s_snaps.clear();
    if (P.frame % 600 == 2)
        log("pica state check: %u draws checked, %u with registers other than meant", s_state_draws, s_state_bad);
}

void draw_elements(u32 count, const u16* indices) {
    if (check_state_on())
        snap_state();
    upload_uniforms();
    if (P.c3d_touched || s_c3d_only) {
        C3D_DrawElements(GPU_TRIANGLES, count, C3D_UNSIGNED_SHORT, indices);
        P.c3d_touched = false;
        P.c3d_drew = true;
        return;
    }
    // (as C3D_DrawElements writes them for GPU_TRIANGLES)
    u32 pa = osConvertVirtToPhys(indices);
    GPUCMD_AddMaskedWrite(GPUREG_PRIMITIVE_CONFIG, 2, GPU_GEOMETRY_PRIM);
    GPUCMD_AddWrite(GPUREG_RESTART_PRIMITIVE, 1);
    GPUCMD_AddWrite(GPUREG_INDEXBUFFER_CONFIG, (pa - s_buf_base) | (1u << 31));
    GPUCMD_AddWrite(GPUREG_NUMVERTICES, count);
    GPUCMD_AddWrite(GPUREG_VERTEX_OFFSET, 0);
    GPUCMD_AddMaskedWrite(GPUREG_GEOSTAGE_CONFIG, 2, 0x100);
    GPUCMD_AddMaskedWrite(GPUREG_GEOSTAGE_CONFIG2, 2, 0x100);
    GPUCMD_AddMaskedWrite(GPUREG_START_DRAW_FUNC0, 1, 0);
    GPUCMD_AddWrite(GPUREG_DRAWELEMENTS, 1);
    GPUCMD_AddMaskedWrite(GPUREG_START_DRAW_FUNC0, 1, 1);
    GPUCMD_AddMaskedWrite(GPUREG_GEOSTAGE_CONFIG, 2, 0);
    GPUCMD_AddMaskedWrite(GPUREG_GEOSTAGE_CONFIG2, 2, 0);
    GPUCMD_AddWrite(GPUREG_VTX_FUNC, 1);
    GPUCMD_AddMaskedWrite(GPUREG_PRIMITIVE_CONFIG, 0x8, 0);
    GPUCMD_AddMaskedWrite(GPUREG_PRIMITIVE_CONFIG, 0x8, 0);
}

// an applet had the GPU: citro3d binds its own state again, we send our uniforms again and set
// the pixel pipeline's state again through citro3d (what it has may be older than the GPU had:
// see the state written directly below)
void apt_hook(APT_HookType type, void*) {
    if (type == APTHOOK_ONRESTORE)
        P.uni_resend = true, P.c3d_touched = true, P.all_dirty = true, P.env_valid = false, P.cull = ~0u;
}
aptHookCookie s_apt_cookie;

// ---- The pixel pipeline's state written directly. citro3d's setters only store a value and mark
// it, and C3D_DrawElements then runs its whole context update (every flag, both shaders' 96 uniform
// flags): ~150 times a frame. Each frame's state is first set through citro3d (P.c3d_touched: the
// frame's first draw, after a clear, a read-back or an applet); after that a change goes to the
// GPU as citro3d would write it, and citro3d isn't told (its next full update comes with the next
// frame, which sets everything through it again).

void emit_tests() {
    const u32 w[4] = {D.alpha_test, D.stencil, D.stencil_op, D.depth};
    GPUCMD_AddIncrementalWrites(GPUREG_FRAGOP_ALPHA_TEST, w, 4);
    GPUCMD_AddMaskedWrite(GPUREG_GAS_DELTAZ_DEPTH, 0x8, (u32)GPU_MAKEGASDEPTHFUNC((D.depth >> 4) & 7) << 24);
}
void st_depth(bool enable, GPU_TESTFUNC f, GPU_WRITEMASK mask) {
    D.depth = (u32)enable | ((f & 7) << 4) | ((u32)mask << 8);
    if (direct())
        emit_tests();
    else
        C3D_DepthTest(enable, f, mask);
}
void st_alpha_test(bool enable, GPU_TESTFUNC f, int ref) {
    D.alpha_test = (u32)enable | ((f & 7) << 4) | ((u32)ref << 8);
    if (direct())
        emit_tests();
    else
        C3D_AlphaTest(enable, f, ref);
}
void st_blend(GPU_BLENDEQUATION ce, GPU_BLENDEQUATION ae, GPU_BLENDFACTOR sc, GPU_BLENDFACTOR dc, GPU_BLENDFACTOR sa,
              GPU_BLENDFACTOR da) {
    D.blend = ce | (ae << 8) | (sc << 16) | (dc << 20) | ((u32)sa << 24) | ((u32)da << 28);
    D.fragop = (D.fragop & ~0xFF00u) | 0x0100;
    if (direct()) {
        GPUCMD_AddWrite(GPUREG_BLEND_FUNC, D.blend);
        GPUCMD_AddMaskedWrite(GPUREG_COLOR_OPERATION, 7, D.fragop);
    } else {
        C3D_AlphaBlend(ce, ae, sc, dc, sa, da);
    }
}
void st_logic_op(GPU_LOGICOP op) {
    D.logic = op;
    D.fragop &= ~0xFF00u;
    if (direct()) {
        GPUCMD_AddWrite(GPUREG_LOGIC_OP, D.logic);
        GPUCMD_AddMaskedWrite(GPUREG_COLOR_OPERATION, 7, D.fragop);
    } else {
        C3D_ColorLogicOp(op);
    }
}
void st_cull(GPU_CULLMODE mode) {
    D.cull = mode & 3;
    if (direct())
        GPUCMD_AddWrite(GPUREG_FACECULLING_CONFIG, mode & 3);
    else
        C3D_CullFace(mode);
}
void st_scissor(GPU_SCISSORMODE mode, u32 left, u32 top, u32 right, u32 bottom) {
    D.scissor[0] = mode;
    if (mode != GPU_SCISSOR_DISABLE)
        D.scissor[1] = (top << 16) | (left & 0xFFFF), D.scissor[2] = ((bottom - 1) << 16) | ((right - 1) & 0xFFFF);
    if (direct())
        GPUCMD_AddIncrementalWrites(GPUREG_SCISSORTEST_MODE, D.scissor, 3);
    else
        C3D_SetScissor(mode, left, top, right, bottom);
}
void st_texenv(int id, C3D_TexEnv* env) {
    if (direct())
        GPUCMD_AddIncrementalWrites(GPUREG_TEXENV0_SOURCE + (id >= 4 ? id + 2 : id) * 8, (u32*)env,
                                    sizeof(C3D_TexEnv) / sizeof(u32));
    else
        C3D_SetTexEnv(id, env);
}
void st_texenv_buf(u32 rgb_mask, u32 a_mask) {
    D.texenv_buf = (rgb_mask & 0xF) << 8 | (a_mask & 0xF) << 12;
    if (direct()) {
        GPUCMD_AddMaskedWrite(GPUREG_TEXENV_UPDATE_BUFFER, 0x7, D.texenv_buf);
        GPUCMD_AddWrite(GPUREG_TEXENV_BUFFER_COLOR, 0xFFFFFFFF);
        GPUCMD_AddWrite(GPUREG_FOG_COLOR, 0);
    } else {
        C3D_TexEnvBufUpdate(C3D_RGB, rgb_mask);
        C3D_TexEnvBufUpdate(C3D_Alpha, a_mask);
    }
}
// a texture for unit u; st_tex_done() after a setup's textures (the units' configuration)
void st_tex(int u, C3D_Tex* t) {
    D.bound[u] = t;
    if (!direct()) {
        C3D_TexBind(u, t);
        D.c3d_bound[u] = t;
        return;
    }
    const u32 reg[5] = {t->border, t->dim, t->param, t->lodParam, osConvertVirtToPhys(t->data) >> 3};
    static const u32 k_border[3] = {GPUREG_TEXUNIT0_BORDER_COLOR, GPUREG_TEXUNIT1_BORDER_COLOR, GPUREG_TEXUNIT2_BORDER_COLOR};
    static const u32 k_type[3] = {GPUREG_TEXUNIT0_TYPE, GPUREG_TEXUNIT1_TYPE, GPUREG_TEXUNIT2_TYPE};
    GPUCMD_AddIncrementalWrites(k_border[u], reg, 5);
    GPUCMD_AddWrite(k_type[u], t->fmt);
}
void st_tex_done() {
    if (!direct())
        return;
    u32 units = 0;
    for (int u = 0; u < 3; u++)
        if (D.bound[u])
            units |= 1u << u;
    // (as citro3d: the units on and the texture cache cleared, then the cache bit again)
    GPUCMD_AddMaskedWrite(GPUREG_TEXUNIT_CONFIG, 0xB, (1u << 12) | units | (1u << 16));
    GPUCMD_AddMaskedWrite(GPUREG_TEXUNIT_CONFIG, 0x4, 1u << 16);
    GPUCMD_AddWrite(GPUREG_TEXUNIT0_SHADOW, 1);
}

// the shader's constants for the next draws: the groups that changed go to the GPU (the lights
// are most of them and change the least)
void set_consts(const GxTnlConsts& c) {
    Timed timed(Timers::CONSTS);
    const f32* a = (const f32*)&c;
    const f32* b = (const f32*)&P.consts;
    static const u32 k_groups[][2] = {{0, 4}, {4, 6}, {10, 2}, {12, 32}, {44, 9}};  // first vector, count
    for (const u32* g : k_groups) {
        u32 first = g[0], n = g[1];
        if (first == 12)  // the lights: the ones in use
            n = std::min<u32>(std::max(c.nlights, P.consts_valid ? P.consts.nlights : 8u), 8u) * 4;
        if (!P.consts_valid || memcmp(a + 4 * first, b + 4 * first, n * 16) != 0) {
            for (u32 i = first; i < first + n; i++)
                set_fvec((int)i, a + 4 * i);
            T.consts_vectors += n;
        }
    }
    if (!P.consts_valid || c.nlights != P.consts.nlights)
        P.ints_dirty = true;
    if (!P.consts_valid || c.bools != P.consts.bools)
        P.bools_dirty = true;
    P.consts = c;
    P.consts_valid = true;
}

// the palette rows written since the last upload
void upload_palette() {
    for (u64 d = P.pal.dirty; d; d &= d - 1) {
        u32 slot = (u32)__builtin_ctzll(d);
        set_fvec((int)(TNL_PAL_BASE + slot), P.pal.rows[slot]);
        T.pal_slots++;
    }
    P.pal.dirty = 0;
}

// NOMA_DUMP_BATCHES=N (env.txt): frame N's draw calls go to sdmc:/3ds/noma/batches.bin as the GPU
// gets them (uniforms, vertices, indices), for tools/pica_replay on the host
FILE* s_dump;
int s_dump_frame = -2;
void write_batch(FILE* f, u32 nverts, const PVertex* verts, const GxTnlSlots* slots, u32 nidx, const u16* idx, u32 base) {
    u32 hdr[8] = {0x42415443, nverts, nidx, P.consts.bools, P.consts.nlights, P.cull, base, 0};
    fwrite(hdr, 4, 8, f);
    for (int i = 0; i < 96; i++) {  // (our copy is in the GPU's order, w first)
        u32 v[4] = {P.uni[i][3], P.uni[i][2], P.uni[i][1], P.uni[i][0]};
        fwrite(v, 4, 4, f);
    }
    for (u32 i = 0; i < nverts; i++) {  // (the vertex with its slots, as tools/pica_replay reads them)
        fwrite(&verts[i], sizeof(PVertex), 1, f);
        fwrite(&slots[i], sizeof(GxTnlSlots), 1, f);
    }
    fwrite(idx, 2, nidx, f);
    fflush(f);  // (a GPU that hangs on the frame leaves the file open)
}
void dump_batch(u32 nverts, const PVertex* verts, const GxTnlSlots* slots, u32 nidx, const u16* idx, u32 base) {
    if (s_dump_frame == -2)
        s_dump_frame = getenv("NOMA_DUMP_BATCHES") ? atoi(getenv("NOMA_DUMP_BATCHES")) : -1;
    if ((int)P.frame != s_dump_frame)
        return;
    if (!s_dump && !(s_dump = fopen("sdmc:/3ds/noma/batches.bin", "wb")))
        return;
    write_batch(s_dump, nverts, verts, slots, nidx, idx, base);
}

// ---- NOMA_PICA_STEP=N (env.txt): the first N frames go to the GPU a piece at a time, each waited
// for (2 s at most): the clear, a command list of one register write, each draw, the display
// transfer. A GPU that hangs then names what it hung on: logged with the draw's state, its batch
// written to sdmc:/3ds/noma/hang.bin (batches.bin's layout: tools/pica_replay runs it through the
// shader). The PICA keeps its registers from one command list to the next, so the frame's state
// goes on as the renderer set it; only the screen waits for the frame's end.
const int s_step_frames = getenv("NOMA_PICA_STEP") ? atoi(getenv("NOMA_PICA_STEP")) : 0;
inline bool stepping() { return (int)P.frame < s_step_frames; }
enum StepKind { STEP_CLEAR, STEP_LIST, STEP_DRAW, STEP_TRANSFER };
struct Step {
    double ms[4] = {};  // by StepKind, this frame
    double worst = 0;   // the slowest draw
    u32 pieces = 0, draws = 0, worst_draw = 0;
    // the last batch drawn (a draw's piece): its indices, the vertices they reach, its GX draw
    u32 count = 0, first = 0, lo = 0, hi = 0, draw = 0;
} S;

// the draws with the current state so far, as one; the palette's slots are free again after
void flush_batch() {
    if (P.batch_count) {
        NOMA_PROF_SCOPE("(gx flush)");
        Timed timed(Timers::FLUSH);
        upload_palette();
        if ((int)P.frame == s_dump_frame || s_dump_frame == -2 || stepping()) {
            // the vertices the indices reach (one segment: indices are relative to its start)
            u32 lo = ~0u, hi = 0;
            for (u32 i = 0; i < P.batch_count; i++)
                lo = std::min<u32>(lo, P.ibuf[P.batch_first + i]), hi = std::max<u32>(hi, P.ibuf[P.batch_first + i]);
            if (lo <= hi)
                dump_batch(hi - lo + 1, P.vbuf + lo, P.slots + lo, P.batch_count, P.ibuf + P.batch_first, lo);
            S.count = P.batch_count, S.first = P.batch_first, S.lo = lo, S.hi = hi, S.draw = P.draws;
        }
        draw_elements(P.batch_count, P.ibuf + P.batch_first);
        P.batch_count = 0;
        P.batches++;
    }
    P.pal.flushed();
}

// the attribute buffers: the pool (position, normal, color, texgen sources) and the frame's slots
void bind_vertices() {
    C3D_BufInfo* bi = C3D_GetBufInfo();
    BufInfo_Init(bi);
    bi->base_paddr = s_buf_base;  // (C3D_DrawElements and C3D_DrawArrays count from it too)
    BufInfo_Add(bi, P.vbuf, sizeof(PVertex), 6, 0x654210);
    BufInfo_Add(bi, P.slots, sizeof(GxTnlSlots), 1, 0x3);
}

inline void slots_written(u32 first, u32 n) {
    P.slots_lo = std::min(P.slots_lo, first), P.slots_hi = std::max(P.slots_hi, first + n);
}

// what the CPU wrote this frame goes to memory for the GPU (citro3d's C3D_FrameEnd(0) would
// flush the whole linear heap: the guest's memory is in it)
void frame_list_starts() {
    u32 *buf, size, off;
    GPUCMD_GetBuffer(&buf, &size, &off);
    s_frame_list = buf + off;
    s_wait_frame.store(P.frame);
}

// cmdlist.bin: little-endian words. "PHYS" n, then n x (name[12], va, pa, size); "LIST" frame,
// seq, va, pa, words, then the words (the frame's commands so far: a frame read back in the middle
// is two lists, and a clear's split leaves a FINALIZE inside)
void dump_word(u32 w) { fwrite(&w, 4, 1, s_cmd_dump); }
void dump_phys(const char* name, const void* va, u32 size) {
    char n[12] = {};
    strncpy(n, name, sizeof(n) - 1);
    fwrite(n, 1, sizeof(n), s_cmd_dump);
    dump_word((u32)va), dump_word(va ? osConvertVirtToPhys(va) : 0), dump_word(size);
}
void dump_open() {
    if (!s_cmd_frames || !(s_cmd_dump = fopen("sdmc:/3ds/noma/cmdlist.bin", "wb")))
        return;
    dump_word(0x53594850), dump_word(P.slot_buf[1] ? 8 : 7);
    dump_phys("linear", (void*)__ctru_linear_heap, __ctru_linear_heap_size);
    dump_phys("color", P.top->frameBuf.colorBuf, 240 * 400 * 4 * s_top_scale * s_top_scale);
    dump_phys("depth", P.top->frameBuf.depthBuf, 240 * 400 * 4 * s_top_scale * s_top_scale);
    dump_phys("vbuf", P.vbuf, s_pool_vertices * sizeof(PVertex));
    dump_phys("slots", P.slot_buf[0], s_pool_vertices * sizeof(GxTnlSlots));
    if (P.slot_buf[1])
        dump_phys("slots1", P.slot_buf[1], s_pool_vertices * sizeof(GxTnlSlots));
    dump_phys("ibuf", P.ibuf, IBUF_INDICES * sizeof(u16));
    dump_phys("cmdbuf", s_cmd_base, s_cmd_words * 4);
    fflush(s_cmd_dump);
    log("pica: the first %d frames' command lists go to cmdlist.bin", s_cmd_frames);
}
void dump_frame_list() {
    if (!s_cmd_dump)
        return;
    if ((int)P.frame >= s_cmd_frames) {
        fclose(s_cmd_dump);
        s_cmd_dump = nullptr;
        return;
    }
    static u32 s_seq;
    u32 *buf, size, off;
    GPUCMD_GetBuffer(&buf, &size, &off);
    u32 words = (u32)(buf + off - s_frame_list);
    dump_word(0x5453494C), dump_word(P.frame), dump_word(s_seq++), dump_word((u32)s_frame_list);
    dump_word(osConvertVirtToPhys(s_frame_list)), dump_word(words);
    fwrite(s_frame_list, 4, words, s_cmd_dump);
    fflush(s_cmd_dump);
    log("pica: frame %u's command list (%u words) dumped", P.frame, words);
}

void flush_geometry() {
    if (P.vflush < P.vused)
        GSPGPU_FlushDataCache(P.vbuf + P.vflush, (P.vused - P.vflush) * sizeof(PVertex));
    if (P.cache_lo < P.cache_hi)
        GSPGPU_FlushDataCache(P.vbuf + P.cache_lo, (P.cache_hi - P.cache_lo) * sizeof(PVertex));
    if (P.slots_lo < P.slots_hi)
        GSPGPU_FlushDataCache(P.slots + P.slots_lo, (P.slots_hi - P.slots_lo) * sizeof(GxTnlSlots));
    if (P.iflush < P.iused)
        GSPGPU_FlushDataCache(P.ibuf + P.iflush, (P.iused - P.iflush) * sizeof(u16));
    P.vflush = P.vused, P.iflush = P.iused;
    P.cache_lo = P.slots_lo = ~0u, P.cache_hi = P.slots_hi = 0;
}

// The immediate vertices and indices start again at the start of half h of their space
// (overlapping; one after the other the whole of it), what was used so far this frame counted.
u32 imm_half_vertices() { return s_overlap ? s_imm_vertices / 2 : s_imm_vertices; }
u32 imm_half_indices() { return s_overlap ? IBUF_INDICES / 2 : IBUF_INDICES; }
void imm_restart(u32 h) {
    P.vframe += P.vused - P.vstart, P.iframe += P.iused - P.istart;
    const u32 vh = imm_half_vertices(), ih = imm_half_indices();
    P.vused = P.vflush = P.vstart = h * vh, P.vend = (h + 1) * vh;
    P.iused = P.iflush = P.istart = h * ih, P.iend = (h + 1) * ih;
}
// the half the frame's immediate vertices, indices and commands are in now (overlapping: by the
// frame's parity, the other after a spill; one after the other the whole space is the first)
u32 frame_half() { return s_overlap ? (P.frame & 1) ^ (P.spilled ? 1u : 0u) : 0; }

// the frame's commands since the last split are finished: the draws batched, the frame buffer's
// cache written out after native draws, the checks and dumps that read the list, what the CPU
// wrote for it flushed to memory
void end_commands() {
    flush_batch();
    // a list drawn to only by native draws: citro3d doesn't know it was, so its end wouldn't write
    // the frame buffer's cache out before the display transfer reads it
    if (P.native_drew && !P.c3d_drew) {
        GPUCMD_AddWrite(GPUREG_FRAMEBUFFER_FLUSH, 1);
        GPUCMD_AddWrite(GPUREG_FRAMEBUFFER_INVALIDATE, 1);
    }
    P.native_drew = P.c3d_drew = false;
    if (check_state_on())
        check_state_list();
    flush_geometry();
    dump_frame_list();
    u32 *buf, size, off;
    GPUCMD_GetBuffer(&buf, &size, &off);
    P.cmd_words += off;
}

// citro3d's frame begun: it waits for its queue, all that went to the GPU before (the time
// counted for the frame's line and in Timers' WAIT)
void begin_c3d_frame(const char* what) {
    NOMA_PROF_SCOPE("(gx wait)");
    GpuWait w(what);
    Timed timed(Timers::WAIT);
    const u64 t0 = svcGetSystemTick();
    C3D_FrameBegin(0);
    P.wait_ticks += svcGetSystemTick() - t0;
}

// the render targets citro3d transfers to the screens at its frame's end (C3D_FrameDrawOn marks
// one; overlapping it is marked here, outside citro3d's frame)
void draw_on(C3D_RenderTarget* t) {
    if (!s_overlap) {
        C3D_FrameDrawOn(t);
        return;
    }
    t->used = true;
    C3D_SetFrameBuf(&t->frameBuf);
    C3D_SetViewport(0, 0, t->frameBuf.width, t->frameBuf.height);
}

// The display transfers' interrupts (PPF) libctru's GSP event thread has handled, and the transfers
// sent. citro3d swaps the screens' buffers in its queue's callback, which that thread calls for the
// queue's last interrupt after marking the queue finished: C3D_FrameBegin's wait can end before the
// swap, and a frame sent at once would then take the buffer about to be shown for its transfer
// (gfxGetFramebuffer) and have its swap request (needSwapTop) cleared by the frame before's
// callback. A PPF callback of ours runs after citro3d's for the same interrupt.
std::atomic<u32> s_ppf_done{0};
u32 s_ppf_sent = 0;
void on_ppf(void*) { s_ppf_done.fetch_add(1, std::memory_order_release); }

// (overlapping, after citro3d's frame begun) the transfers sent so far handled, swaps included.
// An interrupt that doesn't come in 20 ms (the swap follows the queue's end by microseconds) was lost
// or never counted (a sleep, a transfer sent without s_ppf_sent): the count starts again from what
// came. Eight misses in a row mean they can't be counted on: logged, and not waited for again.
bool s_swaps_counted = true;
u32 s_swap_misses = 0;
void swaps_done() {
    const u64 t0 = svcGetSystemTick();
    while (s_swaps_counted && (s32)(s_ppf_done.load(std::memory_order_acquire) - s_ppf_sent) < 0) {
        if (svcGetSystemTick() - t0 > 20 * (SYSCLOCK_ARM11 / 1000)) {
            const int missing = (int)(s_ppf_sent - s_ppf_done.load());
            s_ppf_sent = s_ppf_done.load();
            if (++s_swap_misses >= 8) {
                log("pica: display transfers' interrupts not seen 8 times in a row: the screens' swaps aren't "
                    "waited for from now on");
                s_swaps_counted = false;
            } else if (s_swap_misses == 1) {
                log("pica: %d display transfers' interrupts not seen in 20 ms (counted again from here)", missing);
            }
            return;
        }
        svcSleepThread(20 * 1000);
    }
    s_swap_misses = 0;
}

// Overlapping: the frame's commands so far to the GPU, once all that went before has finished (the
// frame before, mostly finished by now): citro3d's frame begun, the clears the frame asked for
// queued, the commands split off as its list, and its end (the screens' transfers when `show`,
// the queue run). The frame's commands go on after the list in the same buffer.
void send_commands(bool show) {
    u32 *buf, size, off;
    GPUCMD_GetBuffer(&buf, &size, &off);
    const bool top = P.top->used, bottom = P.bottom && P.bottom->used;
    if (!show) {  // (a part of the frame: its screens get it at its end)
        P.top->used = false;
        if (P.bottom)
            P.bottom->used = false;
    }
    begin_c3d_frame("for the frame before (C3D_FrameBegin)");
    swaps_done();
    s_done = P.frame;
    for (u32 i = 0; i < s_nclears; i++)
        C3D_RenderTargetClear(s_clears[i].target, C3D_CLEAR_ALL, s_clears[i].color, s_clears[i].depth);
    s_nclears = 0;
    GPUCMD_SetBuffer(buf, size, off);
    C3D_FrameSplit(GX_CMDLIST_FLUSH);
    GPUCMD_GetBuffer(&buf, &size, &off);
    s_ppf_sent += (P.top->used ? 1 : 0) + (P.bottom && P.bottom->used ? 1 : 0);
    C3D_FrameEnd(GX_CMDLIST_FLUSH);  // (no list left to split: the transfers, the queue's run)
    GPUCMD_SetBuffer(buf, size, 0);
    if (!show) {
        P.top->used = top;
        if (P.bottom)
            P.bottom->used = bottom;
    }
}

// Overlapping: the GPU has finished all that was sent to it (the frame being made isn't sent):
// citro3d's frame begun waits for its queue, and its end has no list and transfers nothing
void wait_gpu() {
    u32 *buf, size, off;
    GPUCMD_GetBuffer(&buf, &size, &off);
    const bool top = P.top->used, bottom = P.bottom && P.bottom->used;
    P.top->used = false;
    if (P.bottom)
        P.bottom->used = false;
    begin_c3d_frame("for the GPU (C3D_FrameBegin)");
    s_done = P.frame;
    C3D_FrameEnd(GX_CMDLIST_FLUSH);
    GPUCMD_SetBuffer(buf, size, off);
    P.top->used = top;
    if (P.bottom)
        P.bottom->used = bottom;
}

// the frame's commands so far to the GPU: at its end shown on the screens, else (a read-back, an
// applet) only drawn
void end_frame(bool show = true) {
    end_commands();
    if (s_overlap)
        send_commands(show);
    else
        C3D_FrameEnd(GX_CMDLIST_FLUSH);
}

// ---- NOMA_PICA_STEP (see above): the pieces
const char* const k_step_names[4] = {"the clear", "a command list of one register write", "a draw",
                                     "the display transfer"};

// the GPU's queue finished within `ms` (citro3d then has a new frame open); polled, as the GPU's
// thread has nothing else to do meanwhile
bool step_wait(u32 ms) {
    const u64 t0 = svcGetSystemTick(), limit = (u64)ms * (SYSCLOCK_ARM11 / 1000);
    while (!C3D_FrameBegin(C3D_FRAME_NONBLOCK)) {
        if (svcGetSystemTick() - t0 >= limit)
            return false;
        svcSleepThread(100 * 1000);
    }
    return true;
}

// what the GPU hung on, as the log can say it; then the wait goes on, as a frame's start would
void step_hung(StepKind k) {
    log("pica step: frame %u: %s (the frame's piece %u) hasn't finished in 2 s: the GPU hangs on it", P.frame,
        k_step_names[k], S.pieces);
    if (k == STEP_CLEAR || k == STEP_TRANSFER) {
        u16 fw = 0, fh = 0;
        void* fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, &fw, &fh);
        log("pica step: color buffer pa %08lX, depth buffer pa %08lX, 240x400; the screen's buffer va %p pa %08lX, %ux%u",
            osConvertVirtToPhys(P.top->frameBuf.colorBuf), osConvertVirtToPhys(P.top->frameBuf.depthBuf), fb,
            osConvertVirtToPhys(fb), fw, fh);
    }
    if (k == STEP_DRAW && S.count) {
        // its vertices: ranges and values that aren't numbers
        float lo[4][3], hi[4][3];
        for (int a = 0; a < 4; a++)
            for (int c = 0; c < 3; c++)
                lo[a][c] = 1e30f, hi[a][c] = -1e30f;
        u32 bad = 0, smax[4] = {};
        for (u32 i = S.lo; i <= S.hi; i++) {
            const PVertex& v = P.vbuf[i];
            const float* src[4] = {v.pos, v.tc[0], v.tc[1], v.tc[2]};
            for (int a = 0; a < 4; a++)
                for (int c = 0; c < 3; c++) {
                    float x = src[a][c];
                    if (!std::isfinite(x)) {
                        bad++;
                        continue;
                    }
                    lo[a][c] = std::min(lo[a][c], x), hi[a][c] = std::max(hi[a][c], x);
                }
            for (int c = 0; c < 3; c++)
                bad += !std::isfinite(v.nrm[c]);
            for (int s = 0; s < 4; s++)
                smax[s] = std::max<u32>(smax[s], P.slots[i].s[s]);
        }
        log("pica step: GX draw %u: %u indices at ibuf+%u, vertices %u-%u; position x %g..%g y %g..%g z %g..%g; "
            "texgen 0 %g..%g %g..%g %g..%g; %u values not finite; slots up to %u %u %u %u; bools %04lX, %lu lights",
            S.draw, S.count, S.first, S.lo, S.hi, lo[0][0], hi[0][0], lo[0][1], hi[0][1], lo[0][2], hi[0][2], lo[1][0],
            hi[1][0], lo[1][1], hi[1][1], lo[1][2], hi[1][2], bad, smax[0], smax[1], smax[2], smax[3],
            (unsigned long)P.consts.bools, (unsigned long)P.consts.nlights);
        char tex[192] = "";
        int n = 0;
        for (int u = 0; u < 3; u++)
            if (P.tex[u])
                n += snprintf(tex + n, sizeof(tex) - n, " unit %d: format %u %ux%u pa %08lX;", u, (u32)P.tex[u]->fmt,
                              P.tex[u]->width, P.tex[u]->height, osConvertVirtToPhys(P.tex[u]->data));
        log("pica step: textures%s blend %08lX depth %08lX alpha test %08lX cull %lu scissor %lu %08lX %08lX",
            n ? tex : " none;", (unsigned long)D.blend, (unsigned long)D.depth, (unsigned long)D.alpha_test,
            (unsigned long)D.cull, (unsigned long)D.scissor[0], (unsigned long)D.scissor[1],
            (unsigned long)D.scissor[2]);
        if (FILE* f = fopen("sdmc:/3ds/noma/hang.bin", "wb")) {
            write_batch(f, S.hi - S.lo + 1, P.vbuf + S.lo, P.slots + S.lo, S.count, P.ibuf + S.first, S.lo);
            fclose(f);
            log("pica step: the draw's batch is in hang.bin (tools/pica_replay)");
        }
    }
    GpuWait w("for a step (C3D_FrameBegin)");
    C3D_FrameBegin(0);
}

// the commands since the last piece (none for the clear), alone on the GPU; the frame then goes on
void step_piece(StepKind k) {
    if (k == STEP_LIST)
        GPUCMD_AddWrite(GPUREG_VTX_FUNC, 1);  // (clears the post-vertex cache: harmless anywhere)
    if (k == STEP_DRAW) {  // (as citro3d ends a list that drew: the frame buffer's cache written out)
        GPUCMD_AddWrite(GPUREG_FRAMEBUFFER_FLUSH, 1);
        GPUCMD_AddWrite(GPUREG_FRAMEBUFFER_INVALIDATE, 1);
    }
    if (check_state_on())
        check_state_list();
    flush_geometry();
    dump_frame_list();
    C3D_RenderTargetDetachOutput(P.top);  // (the screen gets the frame at its end)
    S.pieces++;
    const u64 t0 = svcGetSystemTick();
    C3D_FrameEnd(GX_CMDLIST_FLUSH);
    if (!step_wait(2000))
        step_hung(k);
    const double ms = (double)(svcGetSystemTick() - t0) * 1000.0 / SYSCLOCK_ARM11;
    S.ms[k] += ms;
    if (k == STEP_DRAW) {
        S.draws++;
        if (ms > S.worst)
            S.worst = ms, S.worst_draw = S.draw;
    }
    C3D_RenderTargetSetOutput(P.top, GFX_TOP, GFX_LEFT, DISPLAY_TRANSFER_FLAGS);
    C3D_FrameDrawOn(P.top);
    frame_list_starts();
}

// after end_frame: the display transfer alone (its list had nothing left), and the frame's line
void step_transfer() {
    S.pieces++;
    const u64 t0 = svcGetSystemTick();
    if (!step_wait(2000))
        step_hung(STEP_TRANSFER);
    S.ms[STEP_TRANSFER] = (double)(svcGetSystemTick() - t0) * 1000.0 / SYSCLOCK_ARM11;
    C3D_FrameEnd(GX_CMDLIST_FLUSH);  // (closes the empty frame step_wait opened)
    log("pica step: frame %u on the GPU a piece at a time: clear %.2f ms, a command list %.2f ms, %u draws %.2f ms "
        "(longest %.2f ms: GX draw %u), display transfer %.2f ms",
        P.frame, S.ms[STEP_CLEAR], S.ms[STEP_LIST], S.draws, S.ms[STEP_DRAW], S.worst, S.worst_draw,
        S.ms[STEP_TRANSFER]);
    S = Step();
}

// ---------------------------------------------------------------- textures
u32 next_pow2(u32 v) {
    u32 p = 8;
    while (p < v && p < 1024)
        p <<= 1;
    return p;
}

GPU_TEXTURE_WRAP_PARAM gx_wrap(u32 m) {
    switch (m) {
    case 1: return GPU_REPEAT;
    case 2: return GPU_MIRRORED_REPEAT;
    default: return GPU_CLAMP_TO_EDGE;
    }
}

// The PICA format a GX texture is kept in: the smallest that holds it exactly (intensity and
// intensity-alpha as luminance-alpha, L = A for intensity; RGB565 as itself), CMPR's colors at
// 5 bits for green too; the rest RGBA8. The guest's whole memory is in the linear heap with the
// textures (platform/3ds/main.cpp), so their size counts: a font sheet in I4 takes a quarter of
// its RGBA8 size as LA4.
GPU_TEXCOLOR pica_format(const GxTexture* gt) {
    u32 f = gt->fmt;
    if (f == 8 || f == 9 || f == 10)  // paletted: the palette's format (IA8, RGB565, RGB5A3)
        return gt->tlut_fmt == 0 ? GPU_LA8 : gt->tlut_fmt == 1 ? GPU_RGB565 : GPU_RGBA8;
    switch (f) {
    case 0: return GPU_L4;  // I4, I8: luminance only, the combiners read their alpha from red
    case 1: return GPU_L8;
    case 2: return GPU_LA4;  // IA4, IA8
    case 3: return GPU_LA8;
    case 4: return GPU_RGB565;
    case 14: return GPU_RGBA5551;  // CMPR
    default: return GPU_RGBA8;
    }
}

u32 texel_bits(GPU_TEXCOLOR f) {
    switch (f) {
    case GPU_L4:
    case GPU_ETC1: return 4;
    case GPU_LA4:
    case GPU_L8:
    case GPU_ETC1A4: return 8;
    case GPU_LA8:
    case GPU_RGB565:
    case GPU_RGBA5551: return 16;
    default: return 32;
    }
}

// A texture about to be freed: no unit may keep pointing at it. Its C3D_Tex lives in P.textures'
// node, so a pointer kept after the erase would read freed memory, and citro3d (which binds the
// units it holds again at its next context update) or the GPU's own registers would keep the
// texture's freed address: on hardware, whatever that memory holds next (black). citro3d takes no
// texture for unit 0 only: for units 1 and 2 C3D_TexBind asks the texture its type first (a data
// abort at 0xC with none, on the island as the native g3d made room). Those get the white texture,
// which is never freed; without one (no memory at the start) citro3d keeps what it had.
extern C3D_Tex s_white;
void forget_texture(const C3D_Tex* t) {
    for (int u = 0; u < 3; u++) {
        if (P.tex[u] == t)
            P.tex[u] = nullptr, P.env_valid = false;
        if (D.bound[u] == t)
            D.bound[u] = nullptr, P.env_valid = false;
        if (D.c3d_bound[u] == t) {
            if (u == 0)
                C3D_TexBind(0, nullptr), D.c3d_bound[0] = nullptr;
            else if (s_white.data)
                C3D_TexBind(u, &s_white), D.c3d_bound[u] = &s_white;
        }
    }
}

// drop the least recently used texture that no frame the GPU may still be drawing drew with (last
// drawn before s_done); false when there is none
bool evict_texture() {
    auto victim = P.textures.end();
    for (auto it = P.textures.begin(); it != P.textures.end(); ++it)
        if (it->second.tex.data && it->second.last_used < s_done &&
            (victim == P.textures.end() || it->second.last_used < victim->second.last_used))
            victim = it;
    if (victim == P.textures.end())
        return false;
    forget_texture(&victim->second.tex);
    C3D_TexDelete(&victim->second.tex);
    P.textures.erase(victim);
    P.evicted++;
    return true;
}

void free_finished();

// texture memory, from the linear heap: a little stays free for screenshots and read-backs;
// older textures make room
bool tex_init(C3D_Tex* t, u32 w, u32 h, GPU_TEXCOLOR f, bool mips = false) {
    const u32 MARGIN = 512 << 10;
    u32 need = w * h * texel_bits(f) / 8 * (mips ? 4 : 3) / 3;
    auto init = [&] { return mips ? C3D_TexInitMipmap(t, w, h, f) : C3D_TexInit(t, w, h, f); };
    for (;;) {
        if (linearSpaceFree() >= need + MARGIN && init())
            return true;
        if (evict_texture())
            continue;
        // overlapping, the textures the frame before drew with (and those it replaced) go once the
        // GPU has finished it
        if (s_overlap && s_done < P.frame) {
            wait_gpu();
            free_finished();
            continue;
        }
        return init();  // nothing left to drop: whatever fits
    }
}

// Text and the channel's other grey pictures (GX's I4, I8, IA4 and IA8: the font's glyph sheets
// above all) are drawn much smaller than their texels: the system font's sheets at about a third,
// where sampling the full-size sheet with no smaller images drops whole strokes (the text shimmers
// and breaks up as it moves). Such a texture gets its smaller images (each a 2x2 box filter of
// the one before, down to 8x8, but on its border: pica_tex::mip_down, so that a pane sampling it
// clamped far past its edge still gets the full-size edge, as GX does) and is sampled between them
// (GPU_LINEAR mipmapping), which the PICA picks by the size it is drawn at; drawn at full size it
// samples the texture as before.
// Only whole power-of-two images that don't repeat and are filtered (not padded, not resampled,
// not pixel art); NOMA_TEX_MIPS=0: none.
bool tex_mips_on() {
    static int s_on = -1;
    if (s_on < 0) {
        const char* v = getenv("NOMA_TEX_MIPS");
        s_on = !v || atoi(v) != 0;
    }
    return s_on > 0;
}

pica_tex::Fmt tex_fmt(GPU_TEXCOLOR f);

void build_mips(C3D_Tex* t, const u8* raw, const GxTexture* gt, GPU_TEXCOLOR fmt) {
    static std::vector<u32> a, b;
    u32 w = gt->width, h = gt->height;
    a.resize((size_t)w * h);
    gx_decode_texture_tlut(raw, gt->fmt, w, h, gt->tlut_addr, gt->tlut_fmt, a.data());
    for (int level = 1; level <= (int)t->maxLevel; level++) {
        const u32 lw = w >> 1, lh = h >> 1;
        b.resize((size_t)lw * lh);
        pica_tex::mip_down(a.data(), w, h, b.data());
        u8* out = (u8*)C3D_Tex2DGetImagePtr(t, level, nullptr);
        pica_tex::from_rgba(b.data(), lw, lh, out, lw, lh, tex_fmt(fmt), false);
        std::swap(a, b);
        w = lw, h = lh;
    }
    if (a.size() > (64u << 10))
        std::vector<u32>().swap(a), std::vector<u32>().swap(b);
}

// NOMA_ETC1: CMPR textures as ETC1 (default: on the Old 3DS only, and a New 3DS run as one,
// NOMA_OLD3DS)
bool etc1_on() {
    static int s_on = -1;
    if (s_on < 0) {
        const char* v = getenv("NOMA_ETC1");
        bool n3ds = false;
        APT_CheckNew3DS(&n3ds);
        s_on = v ? atoi(v) != 0 : !n3ds || input_old3ds();
        log("pica: CMPR textures as %s", s_on ? "ETC1" : "RGBA5551");
    }
    return s_on > 0;
}

// the PICA format as pica_tex.h names it
pica_tex::Fmt tex_fmt(GPU_TEXCOLOR f) {
    switch (f) {
    case GPU_LA4: return pica_tex::LA4;
    case GPU_LA8: return pica_tex::LA8;
    case GPU_RGB565: return pica_tex::RGB565;
    case GPU_RGBA5551: return pica_tex::RGBA5551;
    case GPU_L8: return pica_tex::L8;
    case GPU_L4: return pica_tex::L4;
    case GPU_ETC1: return pica_tex::ETC1;
    case GPU_ETC1A4: return pica_tex::ETC1A4;
    default: return pica_tex::RGBA8;
    }
}

// the PICA texture for a GX texture (raw: its bytes in GX format, gx_texture_raw) sampled as the
// SETMODE0 value `mode` says; scale: what to multiply its coordinates with
C3D_Tex* texture_for(const GxTexture* gt, const u8* raw, u32 mode, float* sx, float* sy) {
    if (!gt || !gt->width)
        return nullptr;
    u32 ws = bits(mode, 0, 2), wt = bits(mode, 2, 2);
    bool linear = bits(mode, 4, 1);
    TexEntry& e = P.textures[gt->id];
    const u32 drawn = e.last_used;
    e.last_used = P.frame;
    bool repeat = ws != 0 || wt != 0;
    u32 W = next_pow2(gt->width), H = next_pow2(gt->height);
    if (e.version != gt->version || !e.tex.data) {
        NOMA_PROF_SCOPE("(gx texture upload)");
        Timed timed(Timers::UPLOAD);
        T.uploads++, T.upload_texels += W * H;
        if (!raw)
            return nullptr;
        GPU_TEXCOLOR fmt = pica_format(gt);
        // CMPR as ETC1 (ETC1A4 with transparent texels): a quarter (half) of RGBA5551, lossy; on by
        // default where memory is tightest, the Old 3DS (NOMA_ETC1=0/1)
        if (gt->fmt == 14 && etc1_on() && !(repeat && (W != gt->width || H != gt->height)))
            fmt = pica_tex::cmpr_has_alpha(raw, gt->width, gt->height) ? GPU_ETC1A4 : GPU_ETC1;
        const bool mips = tex_mips_on() && linear && !repeat && gt->fmt <= 3 && W == gt->width &&
                          H == gt->height && W >= 64 && H >= 64;
        // a new image elsewhere (the old one freed once the GPU has finished with it) when it doesn't
        // fit the old one's place, or overlapping, when a frame the GPU may still be drawing drew it
        if (e.tex.data && (e.tex.width != W || e.tex.height != H || e.fmt != fmt || e.mips != mips ||
                           (s_overlap && drawn >= s_done))) {
            P.dead.push_back({e.tex, P.frame});
            e.tex.data = nullptr;
            e.wrap = 0xFFFF;
        }
        if (!e.tex.data && !tex_init(&e.tex, W, H, fmt, mips)) {
            P.failed++;
            if (P.warned++ < 4)
                log("pica: no memory for a %ux%u texture (%u KB linear heap free)", W, H,
                    (u32)(linearSpaceFree() >> 10));
            return nullptr;
        }
        e.fmt = fmt;
        u8* out = (u8*)e.tex.data;
        // the GX bytes straight into the PICA's tiles; a repeating texture resampled to a power of
        // two (and a format pica_tex doesn't convert) through an RGBA8 image
        const u32 w = gt->width, h = gt->height;
        const bool resample = repeat && (W != w || H != h);
        if (resample || !pica_tex::from_gx(raw, gt->fmt, w, h, gx_tmem(), GX_TMEM_MASK, gt->tlut_addr, gt->tlut_fmt,
                                           out, W, H, tex_fmt(fmt))) {
            static std::vector<u32> rgba;
            rgba.resize((size_t)w * h);
            gx_decode_texture_tlut(raw, gt->fmt, w, h, gt->tlut_addr, gt->tlut_fmt, rgba.data());
            pica_tex::from_rgba(rgba.data(), w, h, out, W, H, tex_fmt(fmt), repeat);
            if (rgba.size() > (64u << 10))
                std::vector<u32>().swap(rgba);
        }
        e.mips = mips && e.tex.maxLevel > 0;
        if (e.mips)
            build_mips(&e.tex, raw, gt, fmt);
        C3D_TexFlush(&e.tex);
        e.version = gt->version;
        e.sx = repeat ? 1.0f : (float)gt->width / (float)W;
        e.sy = repeat ? 1.0f : (float)gt->height / (float)H;
    }
    u32 wrap = ws | wt << 2 | (linear ? 16 : 0);
    if (wrap != e.wrap) {
        e.wrap = wrap;
        C3D_TexSetWrap(&e.tex, gx_wrap(ws), gx_wrap(wt));
        GPU_TEXTURE_FILTER_PARAM f = linear ? GPU_LINEAR : GPU_NEAREST;
        C3D_TexSetFilter(&e.tex, f, f);
        if (e.mips)
            C3D_TexSetFilterMipmap(&e.tex, GPU_LINEAR);
    }
    *sx = e.sx;
    *sy = e.sy;
    return &e.tex;
}

// ... for texture map `map` as the BP registers describe it
C3D_Tex* texture_for(u32 map, float* sx, float* sy) {
    const u8* raw = nullptr;
    const GxTexture* gt = gx_texture_raw(map, &raw);
    return texture_for(gt, raw, g_gx.bp[bp_tex(BP_TX_SETMODE0, map)], sx, sy);
}

// what a unit the combiners read samples when its texture couldn't be made (no memory, no image):
// white, so the draw shows its other inputs, not the unit's previous texture or freed memory
C3D_Tex s_white;
bool white_init() {
    if (!C3D_TexInit(&s_white, 8, 8, GPU_RGBA8))
        return false;
    memset(s_white.data, 0xFF, s_white.size);
    C3D_TexFlush(&s_white);
    C3D_TexSetFilter(&s_white, GPU_NEAREST, GPU_NEAREST);
    C3D_TexSetWrap(&s_white, GPU_REPEAT, GPU_REPEAT);
    return true;
}

// a unit's texture `t` for map `map`, or white (scale 1) when it couldn't be made
C3D_Tex* or_white(C3D_Tex* t, int map, float* sx, float* sy) {
    if (t || !s_white.data)
        return t;
    static u32 s_logged;
    if (s_logged++ < 4)
        log("pica: texture map %d of a draw couldn't be made: white instead", map);
    *sx = *sy = 1.0f;
    return &s_white;
}

// what waited for the GPU to finish the frames that may use it, freed once it has
void free_finished() {
    size_t kept = 0;
    for (State::Dead& d : P.dead) {
        if (d.frame < s_done)
            C3D_TexDelete(&d.tex);
        else
            P.dead[kept++] = d;
    }
    P.dead.resize(kept);
    LightLock_Lock(&s_free_lock);
    kept = 0;
    for (const FreeLater& f : s_free_later) {
        if (f.frame < s_done)
            linearFree(f.p);
        else
            s_free_later[kept++] = f;
    }
    s_free_later.resize(kept);
    LightLock_Unlock(&s_free_lock);
}

// at a frame's start: what the GPU has finished with, and textures no frame drew for 2 s
void collect_textures() {
    free_finished();
    for (auto it = P.textures.begin(); it != P.textures.end();) {
        if (P.frame - it->second.last_used > 120) {
            forget_texture(&it->second.tex);
            C3D_TexDelete(&it->second.tex);
            it = P.textures.erase(it);
        } else {
            ++it;
        }
    }
}

// ---------------------------------------------------------------- TEV -> texture combiners
// The translation is engine/gx/tev_combiner's (the native g3d's materials go through it too, so a
// material looks the same either way): GX's d +- lerp(a, b, c) with bias and scale as one or two
// PICA stages, register constants and konst colours in each stage's constant colour, results kept
// across stages in the combiner buffer. Here: its input from the registers the stream set, its
// stages as citro3d's texenvs.
namespace tev = engine::gx;

u32 s_unsupported;  // TEV configurations approximated (the translation's count, summed)

// the TEV registers as the command stream set them
tev::TevRegs tev_regs_now() {
    const u32* bp = g_gx.bp;
    tev::TevRegs r;
    r.n_stages = bits(bp[BP_GENMODE], 10, 4) + 1;
    for (u32 i = 0; i < r.n_stages; i++)
        r.color_env[i] = bp[BP_TEV_COLOR_ENV + 2 * i], r.alpha_env[i] = bp[BP_TEV_COLOR_ENV + 2 * i + 1];
    for (u32 i = 0; i < 8; i++)
        r.tref[i] = bp[BP_TREF + i], r.ksel[i] = bp[BP_TEV_KSEL + i];
    memcpy(r.reg, g_gx.tev_reg, sizeof(r.reg));
    memcpy(r.konst, g_gx.tev_konst, sizeof(r.konst));
    return r;
}

// a combiner stage as citro3d's texenv (C3D_TexEnvInit, then its setters)
void to_texenv(const tev::CombinerStage& s, C3D_TexEnv* e) {
    e->srcRgb = GPU_TEVSOURCES(s.src_rgb[0], s.src_rgb[1], s.src_rgb[2]);
    e->srcAlpha = GPU_TEVSOURCES(s.src_a[0], s.src_a[1], s.src_a[2]);
    e->opAll = 0;
    e->opRgb = GPU_TEVOPERANDS(s.op_rgb[0], s.op_rgb[1], s.op_rgb[2]);
    e->opAlpha = GPU_TEVOPERANDS(s.op_a[0], s.op_a[1], s.op_a[2]);
    e->funcRgb = s.func_rgb, e->funcAlpha = s.func_a;
    e->color = s.konst;
    e->scaleRgb = s.scale_rgb, e->scaleAlpha = s.scale_a;
}

// The PICA texture combiners for a TEV configuration: the stages (both forms), the combiner
// buffer masks and the (map, coord) pair each texture unit samples.
void translate_tev(const tev::TevRegs& regs, tev::Combiners* comb, C3D_TexEnv env[6]) {
    tev::tev_to_combiners(regs, *comb);
    s_unsupported += comb->unsupported;
    for (int i = 0; i < 6; i++)
        to_texenv(comb->stage[i], &env[i]);
}

// The combiners as a draw's textures need them: a texture kept as luminance only (L4, L8: GX's I4
// and I8) samples with alpha 1, where GX's intensity texture has alpha = intensity: what reads its
// alpha reads its red instead (tev::remap_luminance)
void combiners_for(const tev::Combiners& comb, const C3D_TexEnv env[6], C3D_Tex* const tex[3], C3D_TexEnv out[6]) {
    u32 lum = 0;  // units with a luminance texture
    for (int u = 0; u < 3; u++)
        if (tex[u] && (tex[u]->fmt == GPU_L4 || tex[u]->fmt == GPU_L8))
            lum |= 1u << u;
    if (!lum) {
        memcpy(out, env, sizeof(C3D_TexEnv) * 6);
        return;
    }
    tev::CombinerStage st[6];
    memcpy(st, comb.stage, sizeof(st));
    tev::remap_luminance(st, lum);
    for (int i = 0; i < 6; i++)
        to_texenv(st[i], &out[i]);
}

// ---------------------------------------------------------------- combiner setups, kept
// The layouts switch between a few materials all the time: each TEV configuration is
// translated once and kept, found again by the registers the translation reads.
struct TevSetup {
    u32 key[1 + 32 + 16 + 32];  // stages; their color and alpha stages; TREF and KSEL; registers
    u32 nkey = 0;
    tev::Combiners comb;
    C3D_TexEnv env[6];          // comb's stages as texenvs
};
TevSetup s_tev_setups[256];  // by a hash of the key

// the setup kept for the key words [key, key + n), or made now from the TEV registers regs()
// gives (gathered only then)
template <typename Regs>
const TevSetup& tev_find(const u32* key, u32 n, Regs regs) {
    u32 h = 2166136261u;
    for (u32 i = 0; i < n; i++)
        h = (h ^ key[i]) * 16777619u;
    TevSetup& e = s_tev_setups[(h ^ h >> 16) & 255];
    if (e.nkey == n && memcmp(e.key, key, n * 4) == 0)
        return e;
    T.tev_misses++;
    translate_tev(regs(), &e.comb, e.env);
    memcpy(e.key, key, n * 4);
    e.nkey = n;
    return e;
}

// the setup for the TEV registers as the command stream set them
const TevSetup& tev_setup() {
    Timed timed(Timers::TEV);
    const u32* bp = g_gx.bp;
    u32 key[sizeof(TevSetup::key) / 4];
    u32 n = 0, num = bits(bp[BP_GENMODE], 10, 4) + 1;
    key[n++] = num;
    for (u32 i = 0; i < 2 * num; i++)
        key[n++] = bp[BP_TEV_COLOR_ENV + i];
    for (u32 i = 0; i < (num + 1) / 2; i++)
        key[n++] = bp[BP_TREF + i], key[n++] = bp[BP_TEV_KSEL + i];
    memcpy(key + n, g_gx.tev_reg, sizeof(g_gx.tev_reg));
    n += 16;
    memcpy(key + n, g_gx.tev_konst, sizeof(g_gx.tev_konst));
    n += 16;
    return tev_find(key, n, tev_regs_now);
}

// ... for TEV registers given as values (a native layout's run): the same key words in the same
// order as the registers holding them would give, so that either path finds what the other made
const TevSetup& tev_setup(const tev::TevRegs& r) {
    Timed timed(Timers::TEV);
    u32 key[sizeof(TevSetup::key) / 4];
    u32 n = 0, num = r.n_stages < 1 ? 1 : r.n_stages > 16 ? 16 : r.n_stages;  // (as the translation takes it)
    key[n++] = num;
    for (u32 i = 0; i < num; i++)
        key[n++] = r.color_env[i], key[n++] = r.alpha_env[i];
    for (u32 i = 0; i < (num + 1) / 2; i++)
        key[n++] = r.tref[i], key[n++] = r.ksel[i];
    memcpy(key + n, r.reg, sizeof(r.reg));
    n += 16;
    memcpy(key + n, r.konst, sizeof(r.konst));
    n += 16;
    return tev_find(key, n, [&]() -> const tev::TevRegs& { return r; });
}

// the combiners `env` (comb's, as the units' textures need them) and the textures `tex` are what
// the PICA has: nothing to bind (units the combiners don't read don't matter)
bool combiners_bound(const tev::Combiners& comb, const C3D_TexEnv env[6], C3D_Tex* const tex[3]) {
    bool same = P.env_valid && memcmp(P.env, env, sizeof(P.env)) == 0 && P.buf_rgb == comb.buf_rgb &&
                P.buf_a == comb.buf_a;
    for (int u = 0; u < 3 && same; u++)
        same = comb.unit[u].map < 0 ||
               (tex[u] == P.tex[u] && (!tex[u] || memcmp(tex[u], &P.tex_was[u], sizeof(C3D_Tex)) == 0));
    return same;
}

// ... bound: the draws so far go to the GPU first, then the stages, buffer masks and units that
// changed
void bind_combiners(const tev::Combiners& comb, const C3D_TexEnv env[6], C3D_Tex* const tex[3]) {
    flush_batch();
    NOMA_PROF_SCOPE("(gx bind)");
    Timed timed(Timers::BIND);
    P.setups++;
    for (int i = 0; i < 6; i++)
        if (!P.env_valid || memcmp(&P.env[i], &env[i], sizeof(C3D_TexEnv)) != 0) {
            P.env[i] = env[i];
            st_texenv(i, &P.env[i]);
        }
    if (!P.env_valid || P.buf_rgb != comb.buf_rgb || P.buf_a != comb.buf_a)
        st_texenv_buf(comb.buf_rgb, comb.buf_a);
    P.buf_rgb = comb.buf_rgb, P.buf_a = comb.buf_a;
    bool bound = false;
    for (int u = 0; u < 3; u++) {
        if (comb.unit[u].map >= 0 && tex[u] &&
            (!P.env_valid || tex[u] != P.tex[u] || memcmp(tex[u], &P.tex_was[u], sizeof(C3D_Tex)) != 0))
            st_tex(u, tex[u]), bound = true;
        P.tex[u] = tex[u];
        if (tex[u])
            P.tex_was[u] = *tex[u];
    }
    if (bound)
        st_tex_done();
    P.env_valid = true;
}
// ---------------------------------------------------------------- pixel state
GPU_TESTFUNC test_func(u32 f) {
    static const GPU_TESTFUNC map[8] = {GPU_NEVER, GPU_LESS, GPU_EQUAL, GPU_LEQUAL,
                                        GPU_GREATER, GPU_NOTEQUAL, GPU_GEQUAL, GPU_ALWAYS};
    return map[f & 7];
}

GPU_BLENDFACTOR blend_factor(u32 f, bool src) {
    switch (f) {
    case 0: return GPU_ZERO;
    case 1: return GPU_ONE;
    case 2: return src ? GPU_DST_COLOR : GPU_SRC_COLOR;
    case 3: return src ? GPU_ONE_MINUS_DST_COLOR : GPU_ONE_MINUS_SRC_COLOR;
    case 4: return GPU_SRC_ALPHA;
    case 5: return GPU_ONE_MINUS_SRC_ALPHA;
    case 6: return GPU_DST_ALPHA;
    default: return GPU_ONE_MINUS_DST_ALPHA;
    }
}

// The pixel pipeline's state as the GX registers ZMODE, CMODE0, ZCONTROL and ALPHACOMPARE ask for
// it, in the PICA's terms (pixel_setup; zeroed first, so that two compare with memcmp), and set
// (pixel_state)
struct PixelSetup {
    u32 depth_func, mask;          // GPU_TESTFUNC (the test is always on: the PICA writes depth only
                                   // with it), GPU_WRITEMASK
    u32 logic;                     // a logic op (logic_op) instead of blending (eq, src, dst)
    u32 eq, src, dst, logic_op;    // GPU_BLENDEQUATION, GPU_BLENDFACTOR (colour and alpha alike), GPU_LOGICOP
    u32 alpha_test, alpha_func, alpha_ref;
};

PixelSetup pixel_setup(u32 zmode, u32 cm, u32 zcontrol, u32 ac) {
    PixelSetup s;
    memset(&s, 0, sizeof(s));
    u32 pixfmt = bits(zcontrol, 0, 3);
    int mask = 0;
    if (cm & 0x08)
        mask |= GPU_WRITE_RED | GPU_WRITE_GREEN | GPU_WRITE_BLUE;  // GPU_WRITE_COLOR has alpha too
    if ((cm & 0x10) && pixfmt == 1)
        mask |= GPU_WRITE_ALPHA;
    bool ztest = zmode & 1;
    if (ztest && (zmode & 0x10))
        mask |= GPU_WRITE_DEPTH;
    s.depth_func = ztest ? test_func(bits(zmode, 1, 3)) : GPU_ALWAYS;
    s.mask = (u32)mask;

    if (cm & 1) {
        if (cm & 0x800)
            s.eq = GPU_BLEND_REVERSE_SUBTRACT, s.src = s.dst = GPU_ONE;
        else
            s.eq = GPU_BLEND_ADD, s.src = blend_factor(bits(cm, 8, 3), true), s.dst = blend_factor(bits(cm, 5, 3), false);
    } else if (cm & 2) {
        static const GPU_LOGICOP ops[16] = {GPU_LOGICOP_CLEAR, GPU_LOGICOP_AND, GPU_LOGICOP_AND_REVERSE,
                                           GPU_LOGICOP_COPY, GPU_LOGICOP_AND_INVERTED, GPU_LOGICOP_NOOP,
                                           GPU_LOGICOP_XOR, GPU_LOGICOP_OR, GPU_LOGICOP_NOR,
                                           GPU_LOGICOP_EQUIV, GPU_LOGICOP_INVERT, GPU_LOGICOP_OR_REVERSE,
                                           GPU_LOGICOP_COPY_INVERTED, GPU_LOGICOP_OR_INVERTED,
                                           GPU_LOGICOP_NAND, GPU_LOGICOP_SET};
        s.logic = 1, s.logic_op = ops[bits(cm, 12, 4)];
    } else {
        s.eq = GPU_BLEND_ADD, s.src = GPU_ONE, s.dst = GPU_ZERO;
    }

    // alpha test: the PICA has one comparison; keep the one that isn't "always"
    u32 c0 = bits(ac, 16, 3), c1 = bits(ac, 19, 3), op = bits(ac, 22, 2);
    u32 r0 = bits(ac, 0, 8), r1 = bits(ac, 8, 8);
    s.alpha_test = 1;
    if (c0 == 7 && c1 == 7)
        s.alpha_test = 0, s.alpha_func = GPU_ALWAYS, s.alpha_ref = 0;
    else if (op == 0 && c1 == 7)
        s.alpha_func = test_func(c0), s.alpha_ref = r0;
    else if (op == 0 && c0 == 7)
        s.alpha_func = test_func(c1), s.alpha_ref = r1;
    else if (op == 1 && c1 == 0)
        s.alpha_func = test_func(c0), s.alpha_ref = r0;
    else if (op == 1 && c0 == 0)
        s.alpha_func = test_func(c1), s.alpha_ref = r1;
    else
        s.alpha_func = test_func(c0), s.alpha_ref = r0;
    return s;
}

void pixel_state(const PixelSetup& s) {
    Timed timed(Timers::PIXEL);
    st_depth(true, (GPU_TESTFUNC)s.depth_func, (GPU_WRITEMASK)s.mask);
    if (s.logic)
        st_logic_op((GPU_LOGICOP)s.logic_op);
    else
        st_blend((GPU_BLENDEQUATION)s.eq, (GPU_BLENDEQUATION)s.eq, (GPU_BLENDFACTOR)s.src, (GPU_BLENDFACTOR)s.dst,
                 (GPU_BLENDFACTOR)s.src, (GPU_BLENDFACTOR)s.dst);
    st_alpha_test(s.alpha_test != 0, (GPU_TESTFUNC)s.alpha_func, (int)s.alpha_ref);
}

// ---------------------------------------------------------------- geometry
// GX clip space -> PICA clip space: viewport into the 640x456 frame, scaled to 400x240, rotated
// for the 240x400 render target (citro3d's Mtx_OrthoTilt convention), GX depth range. The shader
// gets it folded into the projection (gx_tnl_plan). On the bottom screen (P.screen) the frame's
// rectangle P.region fills its 320x240 (its 240x320 target) instead.
void set_xform() {
    Timed timed(Timers::XFORM);
    float vsx = xf_f32(XF_VIEWPORT + 0), vsy = xf_f32(XF_VIEWPORT + 1), vsz = xf_f32(XF_VIEWPORT + 2);
    float vox = xf_f32(XF_VIEWPORT + 3), voy = xf_f32(XF_VIEWPORT + 4), voz = xf_f32(XF_VIEWPORT + 5);
    const bool bottom = P.screen == SCREEN_BOTTOM;
    const float SW = bottom ? 320.0f : 400.0f;  // (the screen's width; both are 240 tall)
    float kx = 400.0f / FRAME_W, ky = 240.0f / FRAME_H, ox = 0, oy = 0;
    if (bottom)
        kx = 320.0f / P.region[2], ky = 240.0f / P.region[3], ox = P.region[0], oy = P.region[1];
    f32 m[4][4];
    memset(m, 0, sizeof(m));
    // x' = 1 - Y/120, Y = (y/w * vsy + voy - 342 - oy) * ky
    m[0][1] = -vsy * ky / 120.0f;
    m[0][3] = 1.0f - (voy - 342.0f - oy) * ky / 120.0f;
    // y' = 1 - X/(SW/2), X = (x/w * vsx + vox - 342 - ox) * kx
    m[1][0] = -vsx * kx / (SW / 2);
    m[1][3] = 1.0f - (vox - 342.0f - ox) * kx / (SW / 2);
    // Keep the GX depth, but complement clip z before the PICA divides z by w. The new z/w is
    // GX depth - 1, close to zero at the far plane; adding one in the depth map recovers the GX
    // depth without dividing two nearly equal 24-bit values. The native g3d renderer uses the
    // same mapping (g3d_pica.cpp).
    m[2][2] = vsz / 16777215.0f;
    m[2][3] = voz / 16777215.0f - 1.0f;
    m[3][3] = 1.0f;
    if (memcmp(m, P.clip, sizeof(m)) != 0) {
        memcpy(P.clip, m, sizeof(m));
        P.clip_serial++;
    }

    // scissor, in the rotated frame buffer
    const u32* bp = g_gx.bp;
    float x0 = (float)bits(bp[BP_SCISSOR_TL], 12, 11) - 342.0f - ox, y0 = (float)bits(bp[BP_SCISSOR_TL], 0, 11) - 342.0f - oy;
    float x1 = (float)bits(bp[BP_SCISSOR_BR], 12, 11) - 342.0f - ox + 1, y1 = (float)bits(bp[BP_SCISSOR_BR], 0, 11) - 342.0f - oy + 1;
    auto clampf = [](float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; };
    float X0 = clampf(x0 * kx, 0, SW), X1 = clampf(x1 * kx, 0, SW);
    float Y0 = clampf(y0 * ky, 0, 240), Y1 = clampf(y1 * ky, 0, 240);
    const float S = (float)(bottom ? s_bottom_scale : s_top_scale);  // (the target's pixels a screen pixel)
    st_scissor(GPU_SCISSOR_NORMAL, (u32)((240 - Y1) * S), (u32)((SW - X1) * S), (u32)std::ceil((240 - Y0) * S),
                   (u32)std::ceil((SW - X0) * S));
}

// The piece of a draw's vertices that can be drawn when only `n` of those left were built (the
// palette was full): whole primitives; a strip's piece has an even count, so that the next piece,
// starting two vertices back, keeps the winding (fans aren't split).
u32 piece(u32 prim, u32 n) {
    switch (prim) {
    case 0x80:
    case 0x88: return n & ~3u;
    case 0x90: return n - n % 3;
    case 0x98: return n < 4 ? 0 : n & ~1u;
    default: return 0;
    }
}

// a render target cleared at the frame's start (overlapping: when its commands are sent)
void clear_target(C3D_RenderTarget* t, u32 color, u32 depth) {
    if (!s_overlap) {
        C3D_RenderTargetClear(t, C3D_CLEAR_ALL, color, depth);
        return;
    }
    if (s_nclears < 2)
        s_clears[s_nclears++] = {t, color, depth};
}

// One after the other the frame starts once the GPU has finished the frame before. Overlapping
// it starts at once (the GPU drawing the frame before meanwhile), in its own half of the buffers,
// unless the frame before took this half too (a spill), or an applet had the GPU (the frame's
// first part was sent and it goes on from there): the GPU finishes first then. No wait for the
// display's vblank (C3D_FRAME_SYNCDRAW) either way: the game paces its frames by the emulated
// retrace already, and a slow frame shouldn't wait for the next vblank too.
void begin_frame() {
    if (P.in_frame)
        return;
    P.wait_ticks = 0;
    if (!s_overlap) {
        begin_c3d_frame("for the last frame (C3D_FrameBegin)");
        s_done = P.frame;
    } else if (P.spilled || P.resume) {
        wait_gpu();
    }
    NOMA_PROF_SCOPE("(gx frame)");
    Timed timed(Timers::FRAME);
    P.rec_t0 = svcGetSystemTick();
    P.wait_begin = P.wait_ticks;
    P.cmd_words = 0;
    collect_textures();
    P.vmax = std::max(P.vmax, P.vframe + P.vused - P.vstart);
    P.imax = std::max(P.imax, P.iframe + P.iused - P.istart);
    P.spilled = false;
    imm_restart(frame_half());
    P.vframe = P.iframe = 0;
    if (s_overlap) {
        GPUCMD_SetBuffer(s_cmd_half[frame_half()], s_cmd_half_words, 0);
        P.sbuf = frame_half();
        P.slots = P.slot_buf[P.sbuf];
        bind_vertices();
    }
    const u32* bp = g_gx.bp;
    u32 ar = bp[BP_CLEAR_AR], gb = bp[BP_CLEAR_GB];
    u32 color = (ar & 255) << 24 | ((gb >> 8) & 255) << 16 | (gb & 255) << 8 | 0xFF;
    if (!P.resume)
        clear_target(P.top, color, bp[BP_CLEAR_Z] & 0xFFFFFF);
    P.resume = false;
    P.screen = SCREEN_TOP;
    P.bottom_drawn = false;
    draw_on(P.top);
    frame_list_starts();
    C3D_DepthMap(true, 1.0f, 1.0f);
    P.c3d_touched = true;
    if (stepping())  // (NOMA_PICA_STEP: the clear alone, then a command list of one register write)
        step_piece(STEP_CLEAR), step_piece(STEP_LIST);
    P.draws = P.batches = P.setups = 0;
    P.cached_draws = P.cached_vertices = P.built_vertices = 0;
    P.rebuilt_arrays = P.rebuilt_regs = P.drawn_twice = P.no_room = P.kept_by_bytes = 0;
    P.cache_made = P.lists_rewritten = P.lists_new = 0;
    P.batch_count = 0;
    P.all_dirty = true;
    P.env_valid = false;
    P.cull = ~0u;
    // every frame's palette starts the same: a frame drawing what the last one drew then gives its
    // matrices the same slots, and the list cache's draws keep the slots their vertices have
    P.pal = GxTnlPalette();
    // a full cache starts over once the GPU has finished every frame that drew from it
    if (P.cache_full) {
        if (s_done < P.frame)
            wait_gpu();
        for (u32 i = 0; i < DL_ENTRIES; i++)
            P.dl[i] = DlEntry();
        P.cache_next = s_imm_vertices;
        P.cache_full = false;
    }
    P.dl_offset = ~0u;
    P.in_frame = true;
}

// Overlapping, a frame that needs more than its half of the immediate vertices, the indices or the
// command buffer goes on in the other half once the frame before, whose half it is, has finished:
// its commands so far are sent (that waits for it; a list is one piece of memory), its state set
// again through citro3d as after a read-back, and the next frame waits for it at its start. One
// after the other, or a second time in a frame: false.
bool spill() {
    if (!s_overlap || P.spilled)
        return false;
    end_frame(false);
    P.spilled = true;
    P.spills++;
    imm_restart(frame_half());
    GPUCMD_SetBuffer(s_cmd_half[frame_half()], s_cmd_half_words, 0);
    draw_on(P.screen == SCREEN_BOTTOM && P.bottom ? P.bottom : P.top);
    frame_list_starts();
    P.c3d_touched = true;
    P.all_dirty = true;
    P.env_valid = false;
    P.cull = ~0u;
    static u32 s_logged;
    if (s_logged++ < 4)
        log("pica: frame %u took the other half of the buffers too (%u immediate vertices, %u indices, %u command "
            "words): the next frame waits for it", P.frame, P.vframe, P.iframe, P.cmd_words);
    return true;
}

// room for `nv` immediate vertices and `ni` indices in the frame's space (a spill makes it when it can)
bool imm_room(u32 nv, u32 ni) {
    if (P.vused + nv <= P.vend && P.iused + ni <= P.iend)
        return true;
    if (nv > imm_half_vertices() || ni > imm_half_indices())
        return false;  // (no half holds it: a spill would only cost the frame its overlap)
    return spill() && P.vused + nv <= P.vend && P.iused + ni <= P.iend;
}

// Room for `words` more commands in the frame's half of the command buffer (overlapping; one after
// the other a frame has the whole of it): a GX draw's take at most a few thousand words; a native
// g3d packet stops drawing items once fewer than 67,584 words are left (ITEM_WORDS + GX_RESERVE_WORDS,
// g3d_pica.cpp), and the island's largest measured took 62,000: its room is both and a margin for
// what the island gains (the water's effects)
const u32 CMD_DRAW_WORDS = 16u << 10, CMD_NATIVE_WORDS = 160u << 10;
void cmd_room(u32 words) {
    if (!s_overlap)
        return;
    u32 *buf, size, off;
    GPUCMD_GetBuffer(&buf, &size, &off);
    if (size - off < words)
        spill();
}

// ---------------------------------------------------------------- the display-list cache
// the entry for a list, made (or taken from the oldest of its neighbours) if there is none
DlEntry* dl_entry(u32 addr, u32 size) {
    u32 h = ((addr * 2654435761u) ^ (size * 40503u)) >> 10;
    DlEntry* victim = nullptr;
    for (u32 k = 0; k < 8; k++) {
        DlEntry& e = P.dl[(h + k) & (DL_ENTRIES - 1)];
        if (e.addr == addr && e.size == size)
            return &e;
        if (!victim || !e.addr || (victim->addr && e.last_frame < victim->last_frame))
            victim = &e;
    }
    *victim = DlEntry();
    victim->addr = addr, victim->size = size;
    P.lists_new++;
    return victim;
}

void dl_recipe(const GxTnlPlan& plan, u8 out[12]) {
    memset(out, 0, 12);
    out[0] = plan.need_nrm;
    for (int k = 0; k < 3; k++)
        out[1 + 3 * k] = plan.tc[k].src, out[2 + 3 * k] = plan.tc[k].tex, out[3 + 3 * k] = plan.tc[k].z;
}

// a draw's vertex data in its list (the indices, mostly: a few bytes), 64 bits of it
u64 dl_data_hash(const u8* p, u32 n) {
    u64 h = 14695981039346656037ull ^ n;
    u32 w;
    for (; n >= 4; p += 4, n -= 4) {
        memcpy(&w, p, 4);
        h = (h ^ w) * 1099511628211ull;
    }
    for (; n; p++, n--)
        h = (h ^ *p) * 1099511628211ull;
    return h ^ (h >> 29);
}

// The record still describes the draw: registers, arrays' places, recipe, and the arrays' and the
// draw's own bytes unchanged (0), or what changed: 1 registers, arrays' places or recipe (another
// variant's, cheapest first: an offset may have DL_VARIANTS), 2 the bytes. An array whose pages
// were written since is compared by the bytes the draw reads (then watched from the new
// generation on).
int dl_record_stale(DlRecord& r, u32 vat, const GxTnlPlan& plan, u32 list_gen, const u8* data, u32 count) {
    const u32* cp = g_gx.cp;
    if (r.vat != vat || r.regs[0] != cp[CP_VCD_LO] || r.regs[1] != cp[CP_VCD_HI] || r.regs[2] != cp[CP_VAT_A + vat] ||
        r.regs[3] != cp[CP_VAT_B + vat] || r.regs[4] != cp[CP_VAT_C + vat])
        return 1;
    for (u32 i = 0; i < r.nranges; i++) {
        const DlRange& g = r.ranges[i];
        if (cp[CP_ARRAY_BASE + g.attr] != r.arrays[i][0] || cp[CP_ARRAY_STRIDE + g.attr] != r.arrays[i][1])
            return 1;
    }
    u8 recipe[12];
    dl_recipe(plan, recipe);
    if (memcmp(recipe, r.recipe, sizeof(recipe)) != 0)
        return 1;
    if (r.list_gen != list_gen) {  // the list was written again: the draw's own bytes
        if (count != r.count || dl_data_hash(data, count * plan.fmt->size) != r.data_hash)
            return 2;
        r.list_gen = list_gen;
    }
    bool rehashed = false;
    for (u32 i = 0; i < r.nranges; i++) {
        DlRange& g = r.ranges[i];
        const u32 gen = mem_generation(g.addr, g.size);
        if (gen == g.gen)
            continue;
        if (gx_tnl_array_hash(*plan.fmt, data, count, g.attr) != g.hash)
            return 2;
        g.gen = gen;
        rehashed = true;
    }
    P.kept_by_bytes += rehashed;
    return 0;
}

// ---- NOMA_CHECK_GX_CACHE=1 (env.txt): every list draw drawn from the cache is built again beside
// it and compared with what the GPU will read for it: the kept vertices, the palette slots in the
// frame's buffer of them and the matrix rows those slots hold in the palette (the check's own build
// takes its slots from a copy of the palette, so that the frame's isn't moved). The first
// differences are logged with the draw's list and record, and a count every 600 frames: for a
// draw that comes out wrong in an emulator or on a console, whether the cache's keeping is why.
struct CacheCheck {
    int on = -1;
    u32 draws = 0, bad = 0, logged = 0;
    std::vector<PVertex> v;
    std::vector<GxTnlSlots> s;
    std::vector<GxTnlRows> rows;
} s_cc;
bool check_cache_on() {
    if (s_cc.on < 0)
        s_cc.on = getenv("NOMA_CHECK_GX_CACHE") && atoi(getenv("NOMA_CHECK_GX_CACHE")) != 0;
    return s_cc.on > 0;
}

// the rows a palette slot's matrix takes (from slot s on), as the shader reads them
bool same_rows(const GxTnlPalette& a, u32 sa, const GxTnlPalette& b, u32 sb, u32 n) {
    if (sa + n > TNL_PAL_SLOTS || sb + n > TNL_PAL_SLOTS)
        return false;
    return memcmp(a.rows[sa], b.rows[sb], n * sizeof(a.rows[0])) == 0;
}

// vertices [done, done + n) of a cached draw whose vertices are at pool index `first`, its slots
// set in the frame's buffer of them
void check_cached(const GxTnlPlan& plan, const u8* data, u32 first, u32 done, u32 n, u32 prim, const DlRecord& r) {
    s_cc.draws++;
    s_cc.v.resize(n), s_cc.s.resize(n), s_cc.rows.resize(n);
    GxTnlPalette pal = P.pal;
    u32 built = gx_tnl_vertices(plan, data, done, n, pal, s_cc.v.data(), s_cc.s.data(), s_cc.rows.data());
    if (built < n) {  // (the batch's slots fill the copy too: an empty palette has room)
        pal = GxTnlPalette();
        built = gx_tnl_vertices(plan, data, done, n, pal, s_cc.v.data(), s_cc.s.data(), s_cc.rows.data());
    }
    const u32 pos_rows = plan.nrm_eye ? 6 : 3;
    for (u32 i = 0; i < built; i++) {
        const PVertex& kept = P.vbuf[first + done + i];
        const GxTnlSlots& ks = P.slots[first + done + i];
        const GxTnlSlots& cs = s_cc.s[i];
        const char* what = nullptr;
        if (memcmp(&kept, &s_cc.v[i], sizeof(PVertex)) != 0)
            what = "its vertex";
        else if (!same_rows(P.pal, ks.s[0], pal, cs.s[0], pos_rows))
            what = "its position matrix";
        for (u32 k = 0; k < 3 && !what; k++)
            if (plan.tc[k].src != TNL_SRC_ZERO && !same_rows(P.pal, ks.s[1 + k], pal, cs.s[1 + k], plan.tc[k].stq ? 3 : 2))
                what = "a texgen's matrix";
        if (!what)
            continue;
        s_cc.bad++;
        if (s_cc.logged++ < 24)
            log("pica cache check: frame %u: list %08X+%X, draw of %u vertices (primitive %02X) kept at %u (second place "
                "%u, last drawn in frame %u): vertex %u: %s differs (kept: position %g %g %g, slots %u %u %u %u; built: "
                "position %g %g %g, slots %u %u %u %u)",
                P.frame, P.dl_addr, r.offset, r.count, prim, r.first, r.alt, r.slots_frame, done + i, what, kept.pos[0],
                kept.pos[1], kept.pos[2], ks.s[0], ks.s[1], ks.s[2], ks.s[3], s_cc.v[i].pos[0], s_cc.v[i].pos[1],
                s_cc.v[i].pos[2], cs.s[0], cs.s[1], cs.s[2], cs.s[3]);
        break;  // (a draw's first difference)
    }
    static u32 s_reported;
    if (P.frame / 600 != s_reported) {
        s_reported = P.frame / 600;
        log("pica cache check: %u cached draws checked, %u differ", s_cc.draws, s_cc.bad);
        s_cc.draws = s_cc.bad = 0;
    }
}

}  // namespace

bool PicaRenderer::init() {
    if (!C3D_Init(C3D_DEFAULT_CMDBUF_SIZE * 8))
        return false;
    // citro3d's command buffer, where a frame of its starts its list (an empty frame: no list, no
    // transfers, no targets yet)
    {
        u32 off;
        C3D_FrameBegin(0);
        GPUCMD_GetBuffer(&s_cmd_base, &s_cmd_words, &off);
        C3D_FrameEnd(GX_CMDLIST_FLUSH);
    }
    if (const char* v = getenv("NOMA_PICA_OVERLAP"))
        s_overlap = atoi(v) != 0;
    else
        s_overlap = true;
    if (s_overlap && s_step_frames > 0) {
        s_overlap = false;
        log("pica: frames one after the other for NOMA_PICA_STEP");
    }
    if (const char* v = getenv("NOMA_TOP_SSAA"); v && atoi(v) != 0) {
        s_top_scale = 2;
        if (!(P.top = C3D_RenderTargetCreate(480, 800, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8))) {
            log("pica: no VRAM for the top screen at twice its size: 400x240");
            s_top_scale = 1;
        } else {
            log("pica: the top screen drawn at 800x480, filtered down (NOMA_TOP_SSAA)");
        }
    }
    if (!P.top && !(P.top = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8)))
        return false;
    C3D_RenderTargetSetOutput(P.top, GFX_TOP, GFX_LEFT, DISPLAY_TRANSFER_FLAGS);
    if (const char* v = getenv("NOMA_BOTTOM_SSAA"); v && atoi(v) != 0) {
        s_bottom_scale = 2;
        if (!(P.bottom = C3D_RenderTargetCreate(480, 640, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8))) {
            log("pica: no VRAM for the bottom screen at twice its size: 320x240");
            s_bottom_scale = 1;
        } else {
            log("pica: the bottom screen drawn at 640x480, filtered down (NOMA_BOTTOM_SSAA)");
        }
    }
    if (!P.bottom)
        P.bottom = C3D_RenderTargetCreate(240, 320, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
    if (P.bottom)
        C3D_RenderTargetSetOutput(P.bottom, GFX_BOTTOM, GFX_LEFT, BOTTOM_TRANSFER_FLAGS);
    else
        log("pica: no render target for the bottom screen (VRAM)");
    P.dvlb = DVLB_ParseFile((u32*)gx_shbin, (u32)gx_shbin_size);
    shaderProgramInit(&P.prog);
    shaderProgramSetVsh(&P.prog, &P.dvlb->DVLE[0]);
    C3D_BindProgram(&P.prog);
    // the uniforms where tnl.h lays them out
    int u_proj = shaderInstanceGetUniformLocation(P.prog.vertexShader, "proj");
    int u_pal = shaderInstanceGetUniformLocation(P.prog.vertexShader, "pal");
    P.u_nlights = shaderInstanceGetUniformLocation(P.prog.vertexShader, "nlights");
    if (u_proj != 0 || u_pal != (int)TNL_PAL_BASE || P.u_nlights < 0) {
        log("pica: the shader's uniforms are not where tnl.h expects them (proj %d, pal %d, nlights %d)", u_proj,
            u_pal, P.u_nlights);
        return false;
    }
    C3D_AttrInfo* attr = C3D_GetAttrInfo();
    AttrInfo_Init(attr);
    AttrInfo_AddLoader(attr, 0, GPU_FLOAT, 3);          // position (model space)
    AttrInfo_AddLoader(attr, 1, GPU_FLOAT, 3);          // normal
    AttrInfo_AddLoader(attr, 2, GPU_UNSIGNED_BYTE, 4);  // color 0
    AttrInfo_AddLoader(attr, 3, GPU_UNSIGNED_BYTE, 4);  // palette slots
    AttrInfo_AddLoader(attr, 4, GPU_FLOAT, 3);          // texgen sources 0-2
    AttrInfo_AddLoader(attr, 5, GPU_FLOAT, 3);
    AttrInfo_AddLoader(attr, 6, GPU_FLOAT, 3);
    // the pool and its slots (overlapping: its larger immediate part, two buffers of slots, two
    // halves of the command buffer; without the memory for them, one after the other)
    auto alloc_pool = [&] {
        s_imm_vertices = s_overlap ? IMM_OVERLAP : IMM_SERIAL;
        s_pool_vertices = s_imm_vertices + CACHE_VERTICES;
        P.vbuf = (PVertex*)linearAlloc(s_pool_vertices * sizeof(PVertex));
        for (u32 b = 0; b < (s_overlap ? 2u : 1u); b++)
            P.slot_buf[b] = (GxTnlSlots*)linearAlloc(s_pool_vertices * sizeof(GxTnlSlots));
        P.rows = (GxTnlRows*)malloc(s_pool_vertices * sizeof(GxTnlRows));
        return P.vbuf && P.slot_buf[0] && (!s_overlap || P.slot_buf[1]) && P.rows;
    };
    if (!alloc_pool() && s_overlap) {
        linearFree(P.vbuf), linearFree(P.slot_buf[0]), linearFree(P.slot_buf[1]), free(P.rows);
        P.vbuf = nullptr, P.slot_buf[0] = P.slot_buf[1] = nullptr, P.rows = nullptr;
        s_overlap = false;
        log("pica: no memory for the frames to overlap the GPU: one after the other");
        alloc_pool();
    }
    P.ibuf = (u16*)linearAlloc(IBUF_INDICES * sizeof(u16));
    P.slots = P.slot_buf[0];
    P.sbuf = 0;
    P.cache_next = s_imm_vertices;
    if (P.vbuf && P.slots && P.ibuf) {
        s_buf_base = std::min({osConvertVirtToPhys(P.vbuf), osConvertVirtToPhys(P.slots), osConvertVirtToPhys(P.ibuf)});
        if (P.slot_buf[1])
            s_buf_base = std::min(s_buf_base, osConvertVirtToPhys(P.slot_buf[1]));
        s_buf_base &= ~0xFu;
    }
    log("pica: vertex and index buffers from physical %08lX", (unsigned long)s_buf_base);
    P.dl = new DlEntry[DL_ENTRIES];
    if (!P.vbuf || !P.slots || !P.rows || !P.ibuf)
        return false;
    s_cmd_half_words = (s_cmd_words / 2) & ~3u;  // (a list starts 16-byte aligned)
    s_cmd_half[0] = s_cmd_base, s_cmd_half[1] = s_cmd_base + s_cmd_half_words;
    if (s_overlap)
        log("pica: frames overlap the GPU: a frame's commands are made while the GPU draws the frame before "
            "(NOMA_PICA_OVERLAP=0: one after the other); %u immediate vertices and %u KB of commands a frame",
            s_imm_vertices / 2, s_cmd_half_words >> 8);
    else
        log("pica: frames one after the other: a frame's commands are made once the GPU has drawn the frame before");
    if (!white_init())
        log("pica: no memory for the white texture: a texture that can't be made leaves its unit as it was");
    bind_vertices();
    C3D_CullFace(GPU_CULL_NONE);
    aptHook(&s_apt_cookie, apt_hook, nullptr);
    gspSetEventCallback(GSPGPU_EVENT_PPF, on_ppf, nullptr, false);
    P.ready = true;
    log("pica: renderer ready%s", s_c3d_only ? " (everything through citro3d: NOMA_PICA_C3D)" : "");
    dump_open();
    return true;
}

void PicaRenderer::draw(u32 prim, u32 vat, u32 count, const u8* data) {
    if (!P.ready || !count)
        return;
    // triangles: quads, strips and fans as indices (lines and points not drawn yet)
    u32 max_indices;
    switch (prim) {
    case 0x80:
    case 0x88: max_indices = count / 4 * 6; break;
    case 0x90: max_indices = count / 3 * 3; break;
    case 0x98:
    case 0xA0: max_indices = count >= 3 ? (count - 2) * 3 : 0; break;
    default: return;
    }
    const u32 cull = bits(g_gx.bp[BP_GENMODE], 14, 2);
    if (!max_indices || count >= imm_half_vertices() || cull == 3)  // (3: all culled)
        return;
    begin_frame();
    if (stepping() && P.batch_count) {  // (NOMA_PICA_STEP: the draw before this one, alone)
        flush_batch();
        step_piece(STEP_DRAW);
    }
    // (a strip drawn in pieces builds two vertices more per piece: some room beyond count)
    const u32 room = count + 2 * (count / 4 + 1);
    cmd_room(CMD_DRAW_WORDS);
    if (!imm_room(room, max_indices)) {
        if (P.warned++ < 4)
            log("pica: vertex buffer full");
        return;
    }
    P.draws++;

    // state: the parts whose registers changed since the last draw
    if (g_gx.bp_dirty || P.all_dirty || P.viewport_serial != g_gx.xf_viewport_serial) {
        NOMA_PROF_SCOPE("(gx state)");
        u32 d = P.all_dirty ? ~0u : g_gx.bp_dirty;
        if (P.viewport_serial != g_gx.xf_viewport_serial)
            d |= GX_BP_SCISSOR;
        g_gx.bp_dirty = 0;
        P.all_dirty = false;
        if (d & (GX_BP_TEV | GX_BP_TEX)) {
            // combiners and textures: the batch goes on if they come out the same
            const TevSetup& e = tev_setup();
            GxDrawSetup su;
            C3D_Tex* tex[3] = {};
            {
                NOMA_PROF_SCOPE("(gx textures)");
                Timed timed(Timers::TEXTURES);
                for (int u = 0; u < 3; u++) {
                    const int map = e.comb.unit[u].map;
                    if (map < 0)
                        continue;
                    tex[u] = or_white(texture_for((u32)map, &su.sx[u], &su.sy[u]), map, &su.sx[u], &su.sy[u]);
                    su.texgen[u] = (s8)e.comb.unit[u].coord;
                }
            }
            C3D_TexEnv env[6];
            combiners_for(e.comb, e.env, tex, env);
            bool same = combiners_bound(e.comb, env, tex);
            for (int u = 0; u < 3 && same; u++)  // (and the texture coordinates the constants were planned for)
                same = e.comb.unit[u].map < 0 ||
                       (su.texgen[u] == P.setup.texgen[u] && su.sx[u] == P.setup.sx[u] && su.sy[u] == P.setup.sy[u]);
            if (!same) {
                bind_combiners(e.comb, env, tex);
                bool same_setup = true;
                for (int u = 0; u < 3; u++)
                    same_setup &= su.texgen[u] == P.setup.texgen[u] && su.sx[u] == P.setup.sx[u] && su.sy[u] == P.setup.sy[u];
                if (!same_setup) {
                    P.setup = su;
                    P.setup_serial++;
                }
            }
        }
        if (d & GX_BP_PIXEL) {
            flush_batch();
            const u32* bp = g_gx.bp;
            pixel_state(pixel_setup(bp[BP_ZMODE], bp[BP_CMODE0], bp[BP_ZCONTROL], bp[BP_ALPHACOMPARE]));
        }
        if (d & GX_BP_SCISSOR) {
            flush_batch();
            set_xform();
            P.viewport_serial = g_gx.xf_viewport_serial;
        }
    }

    // the draw as the shader takes it: its constants (a change ends the batch), the GPU's culling
    // (GX front faces are counter-clockwise on the screen, like the PICA's)
    const GxTnlPlan* planp;
    {
        NOMA_PROF_SCOPE("(gx plan)");
        Timed timed(Timers::PLAN);
        planp = &gx_tnl_plan(vat, P.setup, P.setup_serial, P.clip, P.clip_serial);
    }
    const GxTnlPlan& plan = *planp;
    if (cull != P.cull) {
        flush_batch();
        static const GPU_CULLMODE k_cull[3] = {GPU_CULL_NONE, GPU_CULL_FRONT_CCW, GPU_CULL_BACK_CCW};
        st_cull(k_cull[cull]);
        P.cull = cull;
    }
    // the constants: the same plan as the batch's means the same constants
    if (!P.consts_valid || plan.serial != P.consts_plan) {
        NOMA_PROF_SCOPE("(gx consts)");
        if (!P.consts_valid || memcmp(&plan.consts, &P.consts, sizeof(P.consts)) != 0) {
            flush_batch();
            set_consts(plan.consts);
        }
        P.consts_plan = plan.serial;
    }

    // the indices of a piece [done, done + n) of the draw, its vertices at pool index `base`
    auto submit = [&](u32 base, u32 n) {
        NOMA_PROF_SCOPE("(gx submit)");
        Timed timed(Timers::SUBMIT);
        u16* idx = P.ibuf + P.iused;
        u16* const idx0 = idx;
        auto tri = [&](u32 a, u32 b, u32 c) {
            idx[0] = (u16)(base + a), idx[1] = (u16)(base + b), idx[2] = (u16)(base + c);
            idx += 3;
        };
        switch (prim) {
        case 0x80:
        case 0x88:
            for (u32 i = 0; i + 3 < n; i += 4)
                tri(i, i + 1, i + 2), tri(i, i + 2, i + 3);
            break;
        case 0x90:
            for (u32 i = 0; i + 2 < n; i += 3)
                tri(i, i + 1, i + 2);
            break;
        case 0x98:
            for (u32 i = 0; i + 2 < n; i++) {
                if (i & 1)
                    tri(i + 1, i, i + 2);
                else
                    tri(i, i + 1, i + 2);
            }
            break;
        case 0xA0:
            for (u32 i = 1; i + 1 < n; i++)
                tri(0, i, i + 1);
            break;
        }
        u32 ni = (u32)(idx - idx0);
        if (ni) {
            if (!P.batch_count)
                P.batch_first = P.iused;
            P.batch_count += ni;
            P.iused += ni;
        }
        return ni;
    };
    // the next piece of a draw after one of n vertices from `done`, or count when it is over
    auto next_piece = [&](u32 done, u32 n) -> u32 {
        if (done + n >= count)
            return count;
        if (prim == 0xA0) {  // (a fan's later vertices would need its first again)
            if (P.warned++ < 4)
                log("pica: a fan's %u vertices don't fit the palette at once: %u drawn", count, n);
            return count;
        }
        return done + (prim == 0x98 ? n - 2 : n);
    };

    // ---- a display list's draw: its vertices from the cache, or into it (NOMA_PICA_LIST_CACHE=0:
    // every list draw the immediate way, to tell the cache's part in a draw that comes out wrong)
    static const bool s_list_cache = !getenv("NOMA_PICA_LIST_CACHE") || atoi(getenv("NOMA_PICA_LIST_CACHE")) != 0;
    if (gx_in_display_list() && !P.cache_full && s_list_cache) {
        const u32 addr = gx_display_list_addr(), size = gx_display_list_size(), offset = gx_display_list_offset();
        DlEntry* e = dl_entry(addr, size);
        // a new call of a list: its bytes' generation decides whether its records still hold
        if (addr != P.dl_addr || offset <= P.dl_offset) {
            u32 gen = mem_generation(addr, size);
            if (e->gen != gen) {
                // its draws are checked against their bytes as they come; records no draw of the
                // last frames used (draws at other offsets now) go
                P.lists_rewritten += !e->recs.empty();
                e->gen = gen;
                e->recs.erase(std::remove_if(e->recs.begin(), e->recs.end(),
                                             [](const DlRecord& x) { return x.slots_frame + 2 < P.frame; }),
                              e->recs.end());
            }
            P.dl_addr = addr;
        }
        P.dl_offset = offset;
        e->last_frame = P.frame;
        // The draw's record: the one for this command with these registers, arrays and recipe (a
        // list drawn with others in the same frame, a second pass or another model's arrays, has a
        // record of its own, up to DL_VARIANTS; past them the least recently drawn is taken over).
        // A record made again (its arrays' bytes changed, or taken over) puts its vertices back in
        // their place when no frame the GPU may still be drawing drew them; else, overlapping, into
        // its second place (DlRecord::alt), whose last frame was before the frame that moved it
        // away from there, so finished; else into new pool vertices (drawn earlier this frame, or
        // the first time it moves while in flight: its old place becomes its second).
        DlRecord* r = nullptr;
        DlRecord* oldest = nullptr;
        u32 variants = 0;
        auto remake = [&](DlRecord& x) {
            const u32 keep_first = x.first, keep_count = x.count, keep_alt = x.alt, drawn = x.slots_frame;
            x = DlRecord();
            x.offset = offset;
            if (keep_count != count || drawn == P.frame)
                return;
            if (drawn < s_done)
                x.first = keep_first, x.alt = keep_alt, x.count = ~0u;
            else if (keep_alt)
                x.first = keep_alt, x.alt = keep_first, x.count = ~0u;
            else
                x.alt = keep_first;
        };
        for (DlRecord& x : e->recs) {
            if (x.offset != offset)
                continue;
            variants++;
            if (!oldest || x.slots_frame < oldest->slots_frame)
                oldest = &x;
            int stale = x.count ? dl_record_stale(x, vat, plan, e->gen, data, count) : 1;
            if (stale == 1)
                continue;
            r = &x;
            if (stale == 2) {
                P.rebuilt_arrays++;
                remake(x);
            }
            break;
        }
        if (!r && variants >= DL_VARIANTS) {
            // registers or arrays that keep changing (arrays written anew each frame): the oldest
            // record taken over when no frame the GPU may still be drawing drew it, else not kept
            P.rebuilt_regs++;
            if (!oldest || oldest->slots_frame >= s_done)
                goto immediate;
            r = oldest;
            remake(*r);
        } else if (!r && variants) {
            P.rebuilt_regs++;  // (a new pass of a list: counted once, when its record is made)
        }
        u32 reuse_first = 0, reuse_count = 0;
        if (r && r->count == ~0u)
            reuse_first = r->first, reuse_count = count, r->count = 0;
        if (r && r->count == count && r->slots_frame == P.frame)
            P.drawn_twice++;
        if (r && r->count == count && r->slots_frame != P.frame) {
            // the vertices are there: only the slots, in the frame's buffer of them
            NOMA_PROF_SCOPE("(gx slots)");
            Timed timed(Timers::SLOTS);
            const u32 b = P.sbuf;
            if (gx_tnl_rows_vary(plan)) {
                T.vary_draws++;
                for (u32 done = 0; done < count;) {
                    u32 left = count - done;
                    u32 n = gx_tnl_slots(plan, P.rows + r->first + done, left, P.pal, P.slots + r->first + done);
                    if (n < left && !(n = piece(prim, n))) {
                        T.pieces++;
                        flush_batch();
                        n = gx_tnl_slots(plan, P.rows + r->first + done, left, P.pal, P.slots + r->first + done);
                        if (n < left)
                            n = piece(prim, n);
                        if (!n) {
                            if (P.warned++ < 4)
                                log("pica: a draw's matrices don't fit the palette (%u vertices, primitive %02X)", count, prim);
                            return;
                        }
                    }
                    slots_written(r->first + done, n);
                    if (check_cache_on())
                        check_cached(plan, data, r->first, done, n, prim, *r);
                    submit(r->first + done, n);
                    done = next_piece(done, n);
                }
            } else {
                GxTnlSlots fs;
                const GxTnlSlots* hint = r->slots_frame ? &r->slots[r->slots_last] : nullptr;
                if (!gx_tnl_fixed_slots(plan, P.pal, hint, &fs)) {
                    flush_batch();
                    if (!gx_tnl_fixed_slots(plan, P.pal, hint, &fs))
                        return;
                }
                if (!(r->slots_valid >> b & 1) || memcmp(&fs, &r->slots[b], sizeof(fs)) != 0) {
                    T.slot_rewrites++, T.slot_vertices += count;
                    GxTnlSlots* dst = P.slots + r->first;
                    for (u32 i = 0; i < count; i++)
                        dst[i] = fs;
                    r->slots[b] = fs;
                    r->slots_valid |= (u8)(1u << b);
                    slots_written(r->first, count);
                }
                if (check_cache_on())
                    check_cached(plan, data, r->first, 0, count, prim, *r);
                submit(r->first, count);
            }
            r->slots_last = (u8)b;
            r->slots_frame = P.frame;
            P.cached_draws++;
            P.cached_vertices += count;
            return;
        }
        if (!r || r->count != count) {
            // the vertices into the cache (a draw that doesn't fit the palette at once goes the
            // immediate way: its vertices would be made in pieces)
            bool pushed = false;
            if (!r) {
                e->recs.push_back(DlRecord());
                r = &e->recs.back();
                r->offset = offset;
                pushed = true;
            }
            const bool in_place = reuse_count == count;
            if (!in_place && P.cache_next + count > s_pool_vertices) {
                P.cache_full = true;
                P.no_room++;
                if (pushed)
                    e->recs.pop_back();
            } else {
                NOMA_PROF_SCOPE("(gx vertices)");
                Timed timed(Timers::VERTICES);
                u32 first = in_place ? reuse_first : P.cache_next;
                u32 n = gx_tnl_vertices(plan, data, 0, count, P.pal, P.vbuf + first, P.slots + first, P.rows + first);
                if (n < count) {
                    flush_batch();
                    n = gx_tnl_vertices(plan, data, 0, count, P.pal, P.vbuf + first, P.slots + first, P.rows + first);
                }
                if (n == count) {
                    if (!in_place)
                        P.cache_next += count, P.cache_made += count, T.cache_made += count;
                    P.built_vertices += count;
                    P.cache_lo = std::min(P.cache_lo, first), P.cache_hi = std::max(P.cache_hi, first + count);
                    slots_written(first, count);
                    r->first = first, r->count = count, r->prim = prim, r->vat = vat;
                    const u32* cp = g_gx.cp;
                    r->regs[0] = cp[CP_VCD_LO], r->regs[1] = cp[CP_VCD_HI];
                    r->regs[2] = cp[CP_VAT_A + vat], r->regs[3] = cp[CP_VAT_B + vat], r->regs[4] = cp[CP_VAT_C + vat];
                    GxTnlArrayRange ranges[12];
                    r->nranges = gx_tnl_array_ranges(*plan.fmt, data, count, ranges);
                    for (u32 i = 0; i < r->nranges; i++) {
                        const GxTnlArrayRange& g = ranges[i];
                        r->ranges[i] = {g.attr, g.addr, g.size, mem_generation(g.addr, g.size),
                                        gx_tnl_array_hash(*plan.fmt, data, count, g.attr)};
                        r->arrays[i][0] = cp[CP_ARRAY_BASE + g.attr], r->arrays[i][1] = cp[CP_ARRAY_STRIDE + g.attr];
                    }
                    dl_recipe(plan, r->recipe);
                    r->data_hash = dl_data_hash(data, count * plan.fmt->size), r->list_gen = e->gen;
                    r->slots_valid = (u8)(1u << P.sbuf), r->slots_last = (u8)P.sbuf;
                    if (!gx_tnl_rows_vary(plan))
                        r->slots[P.sbuf] = P.slots[first];
                    r->slots_frame = P.frame;
                    submit(first, count);
                    P.cached_draws++;
                    return;
                }
                if (pushed)
                    e->recs.pop_back();
                else
                    r->count = 0;
            }
        }
        // (a record drawn twice in a frame with other matrices, or no room: the immediate way)
    }
immediate:

    // ---- an immediate draw: the vertices for this frame, in pieces when the palette can't hold
    // all their matrices at once (the pending batch goes to the GPU between pieces)
    for (u32 done = 0; done < count;) {
        PVertex* v = P.vbuf + P.vused;
        const u32 left = count - done;
        u32 n;
        {
            NOMA_PROF_SCOPE("(gx vertices)");
            Timed timed(Timers::VERTICES);
            n = gx_tnl_vertices(plan, data, done, left, P.pal, v, P.slots + P.vused, nullptr);
            if (n < left && !(n = piece(prim, n))) {
                T.pieces++;
                flush_batch();
                n = gx_tnl_vertices(plan, data, done, left, P.pal, v, P.slots + P.vused, nullptr);
                if (n < left)
                    n = piece(prim, n);
                if (!n) {
                    if (P.warned++ < 4)
                        log("pica: a draw's matrices don't fit the palette (%u vertices, primitive %02X)", count, prim);
                    return;
                }
            }
        }
        P.built_vertices += n;
        slots_written(P.vused, n);
        if (submit(P.vused, n))
            P.vused += n;
        done = next_piece(done, n);
    }
}

// NOMA_SHOT_EVERY=N (env.txt): every Nth frame, the top screen goes to sdmc:/3ds/noma/shots/
// as a BMP (and the bottom screen beside it, platform_save_bottom_shot) (the render target untiled by the GPU, then turned upright: it is 240x400, rotated)
static void save_shot(C3D_RenderTarget* t, u32 n) {
    const u32 W = 240, H = 400, S = t == P.top ? s_top_scale : 1;  // (a twice-size target filtered down)
    u32* buf = (u32*)linearAlloc(W * H * 4);
    if (!buf)
        return;
    // (scaling, the transfer engine halves the output dimensions it is given, as citro3d gives it
    // the target's own for both)
    s_ppf_sent++;
    C3D_SyncDisplayTransfer((u32*)t->frameBuf.colorBuf, GX_BUFFER_DIM(W * S, H * S), buf, GX_BUFFER_DIM(W * S, H * S),
                            GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |
                                GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) |
                                GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA8) |
                                GX_TRANSFER_SCALING(S == 2 ? GX_TRANSFER_SCALE_XY : GX_TRANSFER_SCALE_NO));
    wait_gpu();  // (the transfer itself, as read_back waits for it)
    GSPGPU_InvalidateDataCache(buf, W * H * 4);
    mkdir("sdmc:/3ds/noma/shots", 0777);
    char path[64];
    snprintf(path, sizeof(path), "sdmc:/3ds/noma/shots/%05u.bmp", (unsigned)n);
    FILE* f = fopen(path, "wb");
    if (f) {
        const u32 SW = 400, SH = 240, row = SW * 3, size = 54 + row * SH;
        u8 hdr[54] = {'B', 'M'};
        auto put32 = [&](int at, u32 v) { memcpy(hdr + at, &v, 4); };
        put32(2, size);
        put32(10, 54);
        put32(14, 40);
        put32(18, SW);
        put32(22, SH);  // bottom-up rows
        hdr[26] = 1;
        hdr[28] = 24;
        put32(34, row * SH);
        fwrite(hdr, 1, 54, f);
        std::vector<u8> line(row);
        for (u32 y = SH; y-- > 0;) {  // BMP rows from the bottom
            for (u32 x = 0; x < SW; x++) {
                u32 p = buf[x * W + (W - 1 - y)];  // 0xRRGGBBAA
                line[x * 3 + 0] = (u8)(p >> 8);
                line[x * 3 + 1] = (u8)(p >> 16);
                line[x * 3 + 2] = (u8)(p >> 24);
            }
            fwrite(line.data(), 1, row, f);
        }
        fclose(f);
    }
    linearFree(buf);
    platform_save_bottom_shot(n);  // (the bottom screen beside it: NNNNN_bottom.bmp)
}

// ---------------------------------------------------------------- EFB copies to textures
namespace {

u32* s_readback;  // the render target after read_back(), untiled: 240x400 (x s_top_scale), 0xRRGGBBAA, rotated

// let the GPU finish what was drawn so far and read the render target back; the frame then
// goes on drawing over it (its end is not shown: the output is detached meanwhile)
bool read_back() {
    NOMA_PROF_SCOPE("(gx readback)");
    Timed timed(Timers::READBACK);
    const u32 RW = 240 * s_top_scale, RH = 400 * s_top_scale;
    if (!s_readback && !(s_readback = (u32*)linearAlloc(RW * RH * 4)))
        return false;
    bool in_frame = P.in_frame;
    if (in_frame) {
        if (!s_overlap)
            C3D_RenderTargetDetachOutput(P.top);
        end_frame(false);
        P.all_dirty = true;  // (the new frame starts without state)
        P.env_valid = false;
        P.cull = ~0u;
    }
    // outside citro3d's frame this waits for its queue, then sends the transfer. Its own wait for the
    // transfer (gspWaitForPPF) ends at once when an earlier transfer's interrupt is still signalled,
    // which nothing else clears (the screens' transfers, every frame): the transfer is then waited
    // for as the end of citro3d's queue, before the buffer is read (wait_gpu: the GPU has then
    // finished everything sent to it). (An emulator does the transfer at once: only a console reads
    // a buffer half written without it.)
    {
        GpuWait w("for a read-back (C3D_SyncDisplayTransfer)");
        s_ppf_sent++;
        C3D_SyncDisplayTransfer((u32*)P.top->frameBuf.colorBuf, GX_BUFFER_DIM(RW, RH), s_readback,
                                GX_BUFFER_DIM(RW, RH),
                                GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |
                                    GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) |
                                    GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA8) |
                                    GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));
    }
    wait_gpu();  // (s_done = P.frame)
    GSPGPU_InvalidateDataCache(s_readback, RW * RH * 4);
    if (in_frame) {
        if (s_overlap) {
            GPUCMD_SetBuffer(s_cmd_half[frame_half()], s_cmd_half_words, 0);
        } else {
            C3D_RenderTargetSetOutput(P.top, GFX_TOP, GFX_LEFT, DISPLAY_TRANSFER_FLAGS);
            C3D_FrameBegin(0);
        }
        draw_on(P.top);
        frame_list_starts();
        P.c3d_touched = true;
        // the frame's immediate vertices, indices and commands are reused from the start of their
        // space (the GPU finished with them; the lists' vertices stay)
        imm_restart(frame_half());
    }
    return true;
}

// The render target holds the frame where it goes on screen. A game that draws its frame in
// bands (this one: two passes through an anti-aliased EFB) moves each band to the top of the
// EFB with the scissor box offset; EFB coordinates are screen coordinates minus that offset.
s32 efb_off_x, efb_off_y;
// an EFB column's and row's place in the read-back buffer (for the offset they were made with)
u32 s_rb_col[1024], s_rb_row[1024];
s32 s_rb_off_x = 1 << 30, s_rb_off_y = 1 << 30;
void set_efb_offset(s32 ox, s32 oy) {
    efb_off_x = ox, efb_off_y = oy;
    if (efb_off_x == s_rb_off_x && efb_off_y == s_rb_off_y)
        return;
    s_rb_off_x = efb_off_x, s_rb_off_y = efb_off_y;
    const float SW = 400.0f * s_top_scale, SH = 240.0f * s_top_scale;  // (the target, upright)
    for (u32 v = 0; v < 1024; v++) {
        float fx = (float)((s32)v + efb_off_x) + 0.5f, fy = (float)((s32)v + efb_off_y) + 0.5f;
        u32 X = (u32)std::min(SW - 1, std::max(0.0f, fx * SW / FRAME_W));
        u32 Y = (u32)std::min(SH - 1, std::max(0.0f, fy * SH / FRAME_H));
        s_rb_col[v] = X * (u32)SH, s_rb_row[v] = (u32)SH - 1 - Y;
    }
}
void load_efb_offset() {
    u32 r = g_gx.bp[BP_SCISSOR_OFFSET];
    set_efb_offset((s32)bits(r, 0, 10) * 2 - 342, (s32)bits(r, 10, 10) * 2 - 342);
}

// an EFB pixel from the read-back render target (the frame scaled to 400x240, rotated)
void readback_rgba(u32 x, u32 y, s32* out) {
    u32 p = s_readback[s_rb_col[x & 1023] + s_rb_row[y & 1023]];
    out[0] = (s32)(p >> 24), out[1] = (s32)((p >> 16) & 255), out[2] = (s32)((p >> 8) & 255), out[3] = (s32)(p & 255);
}

// the clear that goes with a copy: a rectangle in the clear color and depth (the copy's
// source; color, alpha and depth as their update flags allow)
void clear_rect(u32 sx, u32 sy, u32 w, u32 h) {
    cmd_room(CMD_DRAW_WORDS);
    if (!imm_room(6, 0))
        return;
    const u32* bp = g_gx.bp;
    u32 ar = bp[BP_CLEAR_AR], gb = bp[BP_CLEAR_GB], cm = bp[BP_CMODE0];
    u32 pixfmt = bits(bp[BP_ZCONTROL], 0, 3);
    int mask = 0;
    if (cm & 0x08)
        mask |= GPU_WRITE_RED | GPU_WRITE_GREEN | GPU_WRITE_BLUE;
    if ((cm & 0x10) && pixfmt == 1)
        mask |= GPU_WRITE_ALPHA;
    if (bp[BP_ZMODE] & 0x10)
        mask |= GPU_WRITE_DEPTH;
    if (!mask)
        return;
    Timed timed(Timers::CLEAR);
    flush_batch();
    P.all_dirty = true;  // (its state replaces the draws')
    P.env_valid = false;
    C3D_DepthTest(true, GPU_ALWAYS, (GPU_WRITEMASK)mask);
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
    C3D_StencilTest(false, GPU_ALWAYS, 0, 0, 0);
    D.stencil = 0x10;  // (as citro3d has it now: off, GPU_ALWAYS; what the tests' direct writes send)
    P.cull = ~0u;
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    C3D_TexEnv* env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
    C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
    for (int i = 1; i < 6; i++)
        C3D_TexEnvInit(C3D_GetTexEnv(i));
    // the shader with an identity projection and matrix (palette slots 0-2) and the vertex's
    // color: corners straight in the PICA's clip space (as set_xform maps them)
    GxTnlConsts c;
    memset(&c, 0, sizeof(c));
    for (int i = 0; i < 4; i++)
        c.proj[i][i] = 1.0f, c.selmat[i] = 1.0f;
    set_consts(c);
    P.consts_plan = 0;
    P.pal = GxTnlPalette();  // (its slots are made again as the draws after need them)
    for (int i = 0; i < 3; i++)
        P.pal.rows[i][i] = 1.0f;
    P.pal.dirty = 7;
    upload_palette();
    // (clip z = GX depth - 1: begin_frame's depth map, scale 1 offset 1, adds the one back)
    float z = (float)(bp[BP_CLEAR_Z] & 0xFFFFFF) / 16777215.0f - 1.0f;
    auto clip = [&](u32 ex, u32 ey, PVertex& v) {
        float X = ((s32)ex + efb_off_x) * 400.0f / FRAME_W, Y = ((s32)ey + efb_off_y) * 240.0f / FRAME_H;
        memset(&v, 0, sizeof(v));
        v.pos[0] = 1.0f - Y / 120.0f, v.pos[1] = 1.0f - X / 200.0f, v.pos[2] = z;
        v.color[0] = (u8)ar, v.color[1] = (u8)(gb >> 8), v.color[2] = (u8)gb, v.color[3] = (u8)(ar >> 8);
    };
    PVertex* v = P.vbuf + P.vused;
    clip(sx, sy, v[0]), clip(sx + w, sy, v[1]), clip(sx + w, sy + h, v[2]);
    v[3] = v[0], v[4] = v[2], clip(sx, sy + h, v[5]);
    memset(P.slots + P.vused, 0, 6 * sizeof(GxTnlSlots));
    slots_written(P.vused, 6);
    upload_uniforms();
    C3D_DrawArrays(GPU_TRIANGLES, P.vused, 6);
    P.c3d_drew = true;
    P.c3d_touched = true;  // (the state above is citro3d's, bound by that call; the draws after set their own)
    P.vused += 6;
}

}  // namespace

void PicaRenderer::draw_done() {
    if (P.resume)
        begin_frame();  // (a frame an applet interrupted, with no draw since: it is shown too)
    if (P.in_frame) {  // the frame is complete
        if (gx_efb_copy_test_frame() == (int)P.frame && read_back()) {
            set_efb_offset(0, 0);  // the whole frame
            gx_efb_copy_test(readback_rgba, "sdmc:/3ds/noma/efbcopy_3ds");
        }
        const bool step = stepping();
        if (step && P.batch_count) {  // (NOMA_PICA_STEP: the frame's last draw alone)
            flush_batch();
            step_piece(STEP_DRAW);
        }
        {
            NOMA_PROF_SCOPE("(gx end)");
            Timed timed(Timers::END);
            end_frame();
        }
        // the frame's times: its commands' making (the waits for the GPU in it left out), its waits
        P.rec_ticks = svcGetSystemTick() - P.rec_t0 - (P.wait_ticks - P.wait_begin);
        P.rec_sum += P.rec_ticks, P.wait_sum += P.wait_ticks, P.timed_frames++;
        P.cmd_words_max = std::max(P.cmd_words_max, P.cmd_words);
        if (step)
            step_transfer();
        timers_frame_end();
        P.in_frame = false;
        if (s_dump && (int)P.frame == s_dump_frame) {
            fclose(s_dump);
            s_dump = nullptr;
            log("pica: frame %u's draw calls dumped to batches.bin", P.frame);
        }
        P.frame++;
        // the texture cache's frame (the host's renderer moves it on at its XFB copy, which this one
        // never makes): without it a texture was checked against guest memory once, ever (but after
        // an EFB copy), and kept the texels it was first seen with whatever the guest wrote there
        // later: an image made again in the same place, size and format, a palette loaded anew
        gx_texture_frame();
        static int s_shot_every = getenv("NOMA_SHOT_EVERY") ? atoi(getenv("NOMA_SHOT_EVERY")) : 0;
        if (s_shot_every > 0 && P.frame % (u32)s_shot_every == 0) {
            save_shot(P.top, P.frame);  // (its transfer waits for the GPU to finish the frame)
            s_done = P.frame;
        }
        if ((s_unsupported || gx_tnl_approximated()) && P.frame % 600 == 1)
            log("pica: %u TEV configurations and %u draws' lighting or texgens approximated", s_unsupported,
                gx_tnl_approximated());
        if (P.frame % 600 == 1) {
            u32 kb = 0, kb_fmt[16] = {};
            for (auto& kv : P.textures)
                kb += kv.second.tex.size >> 10, kb_fmt[kv.second.tex.fmt & 15] += kv.second.tex.size >> 10;
            log("pica: texture KB by format: RGBA8 %u, RGB565 %u, RGBA5551 %u (CMPR), LA8 %u, LA4 %u, L8 %u, L4 %u, ETC1 %u, "
                "ETC1A4 %u", kb_fmt[GPU_RGBA8], kb_fmt[GPU_RGB565], kb_fmt[GPU_RGBA5551], kb_fmt[GPU_LA8], kb_fmt[GPU_LA4],
                kb_fmt[GPU_L8], kb_fmt[GPU_L4], kb_fmt[GPU_ETC1], kb_fmt[GPU_ETC1A4]);
            // the texture cache's checks against guest memory since the last report (once a frame a
            // texture; its image hashed when its memory was written, and now and then anyway)
            static GxTextureStats s_ts0;
            const GxTextureStats ts = gx_texture_stats();
            log("pica: the texture cache: %u checks, %u images hashed (%u KB), %u found changed",
                ts.checks - s_ts0.checks, ts.hashes - s_ts0.hashes, (u32)((ts.hashed_bytes - s_ts0.hashed_bytes) >> 10),
                ts.changed - s_ts0.changed);
            s_ts0 = ts;
            const double ms = 1000.0 / SYSCLOCK_ARM11, f = P.timed_frames ? P.timed_frames : 1;
            log("pica: %zu textures (%u KB), %u dropped for memory, %u not made; %u KB linear heap free, "
                "heap %u KB used of %u KB; at most %u immediate vertices and %u indices a frame; last frame %u draws "
                "in %u batches, %u state setups, %u draws (%u vertices) from the list cache, %u vertices built "
                "(%u list draws made again for their arrays, %u for registers, %u drawn twice, %u without room; %u "
                "kept by their arrays' bytes), cache at %u of %u (%u vertices given out; %u lists new, %u "
                "rewritten); the GPU %.2f ms drawing the last frame (%.2f ms for its command list, %.2f ms waiting "
                "for the GPU); %s, over %u frames %.2f ms a frame making commands and %.2f ms waiting for the GPU, "
                "%u KB of commands a frame at most, %u frames taking the other half too",
                P.textures.size(), kb, P.evicted, P.failed, (u32)(linearSpaceFree() >> 10),
                (u32)(mallinfo().uordblks >> 10), __ctru_heap_size >> 10, P.vmax, P.imax, P.draws, P.batches,
                P.setups, P.cached_draws, P.cached_vertices, P.built_vertices, P.rebuilt_arrays, P.rebuilt_regs,
                P.drawn_twice, P.no_room, P.kept_by_bytes, P.cache_next - s_imm_vertices,
                s_pool_vertices - s_imm_vertices, P.cache_made, P.lists_new, P.lists_rewritten,
                (double)C3D_GetDrawingTime(), (double)P.rec_ticks * ms, (double)P.wait_ticks * ms,
                s_overlap ? "overlapping the GPU" : "one after the other", P.timed_frames, (double)P.rec_sum * ms / f,
                (double)P.wait_sum * ms / f, P.cmd_words_max >> 8, P.spills);
            P.vmax = P.imax = 0;
            P.rec_sum = P.wait_sum = 0;
            P.timed_frames = P.spills = P.cmd_words_max = 0;
        }
    }
}

// An applet (the keyboard) takes the GPU: citro3d's own APT hook waits for what was submitted
// and sets all state again afterwards, so only the open frame matters. Its draws so far go to the
// render target without being shown, and the next draw goes on with it, uncleared.
// (the I/O thread, every 100 ms) a wait for the GPU of more than 3 s: logged once. (Not with the
// GPU's registers: reading the PICA's while it was stuck froze the whole console.)
void pica_watchdog() {
    static bool s_reported;
    const char* what = s_waiting.load();
    if (!what || s_reported)
        return;
    u64 t = svcGetSystemTick() - s_wait_since.load();
    if (t < 3ull * SYSCLOCK_ARM11)
        return;
    s_reported = true;
    log("pica: waiting %llu ms %s in frame %u: the GPU hasn't finished", t / (SYSCLOCK_ARM11 / 1000), what,
        s_wait_frame.load());
}

void PicaRenderer::suspend() {
    if (!P.ready || !P.in_frame)
        return;
    if (s_overlap) {
        end_frame(false);
    } else {
        C3D_RenderTargetDetachOutput(P.top);
        end_frame();
        C3D_RenderTargetSetOutput(P.top, GFX_TOP, GFX_LEFT, DISPLAY_TRANSFER_FLAGS);
    }
    P.in_frame = false;
    P.resume = true;
}

void PicaRenderer::efb_copy(u32 value) {
    if (!P.ready)
        return;
    EfbCopy c = gx_efb_copy_params(value);
    if (c.to_xfb)  // the frame is on screen already; the next one starts with a clear
        return;
    if (P.screen != SCREEN_TOP && P.in_frame)  // (the frame's copies are of the top screen's picture)
        screen(SCREEN_TOP, 0, 0, 0, 0);
    load_efb_offset();
    if (c.depth) {
        if (P.warned++ < 4)
            log("pica: EFB depth copy to a texture (not emulated)");
    } else if (read_back()) {
        NOMA_PROF_SCOPE("(gx copy convert)");
        Timed timed(Timers::COPY);
        T.copies++;
        gx_copy_to_texture_lookup(c, s_readback, s_rb_col, s_rb_row);
    }
    if (c.clear && P.resume)
        begin_frame();
    if (c.clear && P.in_frame)
        clear_rect(c.sx, c.sy, c.w, c.h);
}

// ---------------------------------------------------------------- native draws
// The native g3d's draws land where their command is in the stream (engine/g3d/native_draw.h):
// what the GX draws before it batched goes to the GPU first, then the backend (g3d_pica.cpp) draws
// with its own shader and state, written directly, and the GX path takes nothing it kept about the
// GPU as still true. A prepare or release packet doesn't start a frame.
void PicaRenderer::native(u32 id) {
    if (P.ready)
        flush_batch();
    engine::g3d::native_draw_execute(id);
}

// What follows goes onto the other screen: what was batched for this one goes first, the bottom
// screen is cleared white at its frame's first draw (the scene draws its background over it), and
// the clip mapping and scissor are made again for the target (set_xform).
void PicaRenderer::screen(u32 screen, u32 x, u32 y, u32 w, u32 h) {
    if (!P.ready)
        return;
    begin_frame();
    flush_batch();
    const bool bottom = screen == SCREEN_BOTTOM && P.bottom;
    if (bottom) {
        if (!P.bottom_drawn) {
            clear_target(P.bottom, 0xFFFFFFFFu, g_gx.bp[BP_CLEAR_Z] & 0xFFFFFF);
            P.bottom_drawn = true;
        }
        P.region[0] = (f32)x, P.region[1] = (f32)y;
        P.region[2] = (f32)std::max((u32)1, w), P.region[3] = (f32)std::max((u32)1, h);
    }
    if (bottom == (P.screen == SCREEN_BOTTOM) && !bottom)
        return;
    P.screen = bottom ? SCREEN_BOTTOM : SCREEN_TOP;
    draw_on(bottom ? P.bottom : P.top);
    C3Di_UpdateContext();
    set_xform();
    C3Di_UpdateContext();
    P.all_dirty = true;
}

void pica_native_begin(f32 clip[4][4]) {
    begin_frame();
    flush_batch();
    cmd_room(CMD_NATIVE_WORDS);
    set_xform();
    C3Di_UpdateContext();
    memcpy(clip, P.clip, sizeof(P.clip));
}

void pica_native_target(C3D_FrameBuf* fb) {
    // (what was drawn into the target so far goes to memory before another is bound)
    GPUCMD_AddWrite(GPUREG_FRAMEBUFFER_FLUSH, 1);
    if (fb) {
        C3D_SetFrameBuf(fb);
        C3D_SetViewport(0, 0, fb->width, fb->height);  // (and no scissor)
        C3Di_UpdateContext();
        return;
    }
    // the frame's target (the screen being drawn on), its whole viewport, then the GX state's scissor
    // (set_xform; a direct write after citro3d's, or citro3d's own)
    draw_on(P.screen == SCREEN_BOTTOM && P.bottom ? P.bottom : P.top);
    C3Di_UpdateContext();
    set_xform();
    C3Di_UpdateContext();
}

void pica_native_end(shaderProgram_s* used) {
    // citro3d: the GX shader program configured again at the next draw (its code too: the native
    // program's was loaded over it), with its attribute layout and buffers. Its bool uniforms are
    // the GX path's own, written directly (P.bools_dirty): citro3d's copy, marked by the program's
    // binding, would follow them and replace them
    C3D_BindProgram(used);
    C3D_BindProgram(&P.prog);
    C3D_BoolUnifsDirty[GPU_VERTEX_SHADER] = false;
    C3D_GetAttrInfo();
    bind_vertices();
    // the GX path's own: its uniforms sent again in full (the native shader's overwrote them; the
    // palette keeps its slots: P.pal stays), its state worked out again and set through citro3d
    P.uni_resend = true;
    P.bools_dirty = P.ints_dirty = true;
    P.c3d_touched = true;
    P.all_dirty = true;
    P.env_valid = false;
    P.cull = ~0u;
    P.viewport_serial = ~0u;
    memset(P.tex, 0, sizeof(P.tex));
    memset(P.tex_was, 0, sizeof(P.tex_was));
    memset(D.bound, 0, sizeof(D.bound));
    P.native_drew = true;
}

void pica_native_draw_noted() {
    if (!check_state_on())
        return;
    PicaSnap n;
    memset(&n, 0, sizeof(n));
    n.native = true;
    s_snaps.push_back(n);
}

void pica_make_room(u32 bytes) {
    const u32 MARGIN = 512 << 10;  // (as tex_init keeps for screenshots and read-backs)
    const u32 free0 = (u32)linearSpaceFree();
    u32 n = 0;
    // a texture last drawn in a frame the GPU has finished (s_done), whether a frame is being made
    // now or the last one still draws
    while (linearSpaceFree() < bytes + MARGIN && evict_texture())
        n++;
    if (n)
        log("pica: %u textures dropped for %u KB (%u KB of the linear heap free, %u KB before)", (unsigned)n,
            (unsigned)(bytes >> 10), (unsigned)(linearSpaceFree() >> 10), (unsigned)(free0 >> 10));
}

void pica_free_after_frame(void* linear) {
    if (!linear)
        return;
    LightLock_Lock(&s_free_lock);
    s_free_later.push_back({linear, P.frame});
    LightLock_Unlock(&s_free_lock);
}

// ---------------------------------------------------------------- layouts drawn natively
// A layout's packet (engine/lyt/draw.h) lands where its command is in the stream and is drawn by
// the GX path's own machinery: the shader gx.v.pica, the kept TEV translations, the texture cache,
// the setters and the batches. Its vertices are in view space with their final colours and
// texture coordinates: the shader takes them through an identity matrix (palette slots 0-2) and
// the projection of the GX state at the command, as clear_rect takes its corners, and its texture
// units read the coordinates as they are, times their texture's scale. A run's combiners,
// textures and pixel state are worked out from its register values as a GX draw's are from the
// registers, and a run whose PICA state comes out as the last one's goes on in its batch. The GX
// draws after the packet work their state out again in full, and find what it left bound where
// it is what they need.
namespace {

// what the runs of a packet are drawn with, as the PICA has it now
struct LytNow {
    GxTnlConsts consts;      // the shader's constants: the projection and the vertex colour (the
                             // whole packet's), the texture units' scales (the run's)
    PixelSetup pixel;
    bool pixel_set = false;  // pixel: set by this packet (before its first run the PICA's is unknown here)
    bool consts_set = false; // consts: the PICA's (after that only the units' scales and bools change)
    s8 coord[3];             // per texture unit: the vertices' texture coordinate it takes (-1: none)
};

// the shader's projection for vertices in view space: the GX projection at this point of the
// stream (XF 0x1020-0x1026, as tnl.cpp's plans read it), then set_xform's mapping into the PICA's
// clip space
void view_projection(f32 out[4][4]) {
    const u32* xf = g_gx.xf;
    f32 pr[6];
    for (int i = 0; i < 6; i++)
        pr[i] = xf_f32(XF_PROJECTION + i);
    f32 m[4][4] = {};
    if (xf[XF_PROJECTION + 6]) {  // orthographic
        m[0][0] = pr[0], m[0][3] = pr[1];
        m[1][1] = pr[2], m[1][3] = pr[3];
        m[2][2] = pr[4], m[2][3] = pr[5];
        m[3][3] = 1.0f;
    } else {
        m[0][0] = pr[0], m[0][2] = pr[1];
        m[1][1] = pr[2], m[1][2] = pr[3];
        m[2][2] = pr[4], m[2][3] = pr[5];
        m[3][2] = -1.0f;
    }
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            out[i][j] = P.clip[i][0] * m[0][j] + P.clip[i][1] * m[1][j] + P.clip[i][2] * m[2][j] + P.clip[i][3] * m[3][j];
}

// a run's texture map as GXLoadTexObj would have loaded it: the GX path's texture for the same
// register values
C3D_Tex* lyt_texture(const engine::lyt::LytTex& t, float* sx, float* sy) {
    const u8* raw = nullptr;
    const GxTexture* gt = gx_texture_raw_regs(t.image0, t.image1, t.image2, t.image3, t.tlut, &raw);
    return texture_for(gt, raw, t.mode0, sx, sy);
}

// A run's state onto the PICA: the combiners and the textures they sample, the units' texture
// coordinates and scales (the constants), the pixel pipeline. Each part that differs from what
// the PICA has ends the batch first, as a GX draw's do.
void lyt_state(const engine::lyt::LytState& s, LytNow& now) {
    const TevSetup& e = tev_setup(s.tev);
    const u32 maps = std::min(s.n_maps, engine::lyt::LYT_MAX_MAPS);
    C3D_Tex* tex[3] = {};
    f32 sx[3] = {1, 1, 1}, sy[3] = {1, 1, 1};
    {
        NOMA_PROF_SCOPE("(gx textures)");
        Timed timed(Timers::TEXTURES);
        // the palettes first, every map's, as the decoder would have loaded them before the draw
        for (u32 m = 0; m < maps; m++)
            if (s.tex[m].tlut_load[1])
                gx_load_tlut(s.tex[m].tlut_load[0], s.tex[m].tlut_load[1]);
        for (int u = 0; u < 3; u++) {
            const int map = e.comb.unit[u].map;
            if (map < 0)
                continue;
            // (a map the run loaded no texture into samples white)
            tex[u] = or_white((u32)map < maps ? lyt_texture(s.tex[map], &sx[u], &sy[u]) : nullptr, map, &sx[u],
                              &sy[u]);
        }
    }
    C3D_TexEnv env[6];
    combiners_for(e.comb, e.env, tex, env);
    if (!combiners_bound(e.comb, env, tex))
        bind_combiners(e.comb, env, tex);

    // Unit u samples at the texture coordinate its stage names, times its texture's scale; a unit
    // the combiners don't read, or one whose coordinate the vertices don't carry, as a GX draw has
    // it: at 0 (units 1 and 2 not computed by the shader).
    GxTnlConsts& c = now.consts;
    const u32 coords = std::min(s.n_texgens, engine::lyt::LYT_MAX_COORDS);
    c.bools = 0;
    for (int u = 0; u < 3; u++) {
        const int k = e.comb.unit[u].coord;
        const bool on = e.comb.unit[u].map >= 0 && k >= 0 && (u32)k < coords;
        f32* scale = &c.tcscale[u >> 1][(u & 1) * 2];
        scale[0] = on ? sx[u] : 0.0f, scale[1] = on ? sy[u] : 0.0f;
        now.coord[u] = on ? (s8)k : -1;
        if (on && u > 0)
            c.bools |= u == 1 ? TNL_TG1 : TNL_TG2;
    }
    if (!now.consts_set ? !P.consts_valid || memcmp(&c, &P.consts, sizeof(c)) != 0
                        : memcmp(c.tcscale, P.consts.tcscale, sizeof(c.tcscale)) != 0 || c.bools != P.consts.bools) {
        flush_batch();
        set_consts(c);
    }
    now.consts_set = true;

    // (ZCONTROL's pixel format is the stream's: a layout doesn't set it)
    const PixelSetup px = pixel_setup(s.zmode, s.cmode0, g_gx.bp[BP_ZCONTROL], s.alpha_compare);
    if (!now.pixel_set || memcmp(&px, &now.pixel, sizeof(px)) != 0) {
        flush_batch();
        pixel_state(px);
        now.pixel = px;
        now.pixel_set = true;
    }
}

}  // namespace

void PicaRenderer::lyt(const engine::lyt::LytPacket& p) {
    if (!P.ready || !p.n_runs)
        return;
    begin_frame();
    Timed timed(Timers::LYT);
    const u64 nested0 = timers_on() ? timers_but_lyt() : 0;
    // (NOMA_PICA_STEP: each run goes to the GPU alone, as each GX draw does; the GX draws before the
    // packet first)
    auto step = [] {
        if (stepping() && P.batch_count) {
            flush_batch();
            step_piece(STEP_DRAW);
        }
    };
    step();
    flush_batch();
    // room for the packet's quads and its states' commands (a spill makes it when it can; before
    // the GX state at the command is set, which a spill would set again through citro3d)
    u32 packet_quads = 0;
    for (const engine::lyt::LytRun *r = p.runs(), *end = r + p.n_runs; r < end; r++)
        packet_quads += r->count / 4;
    cmd_room(CMD_DRAW_WORDS + 512 * p.n_states);
    imm_room(4 * packet_quads, 6 * packet_quads);
    const u32 batches0 = P.batches;
    T.lyt_packets++;

    // The GX state at the command: the viewport, the scissor and the screen (set_xform) and the
    // projection. The vertex colour is the rasterized colour as it is; the palette holds an identity
    // matrix in slots 0-2 for every vertex (the GX draws after the packet fill it again as they
    // need: nothing of it is taken as theirs). No culling: GX's cull mode 0, as P.cull keeps it.
    set_xform();
    LytNow now;
    memset(&now.consts, 0, sizeof(now.consts));
    view_projection(now.consts.proj);
    for (int i = 0; i < 4; i++)
        now.consts.selmat[i] = 1.0f;
    P.pal = GxTnlPalette();
    for (int i = 0; i < 3; i++)
        P.pal.rows[i][i] = 1.0f;
    P.pal.dirty = 7;
    upload_palette();
    if (P.cull != 0) {
        st_cull(GPU_CULL_NONE);
        P.cull = 0;
    }

    const engine::lyt::LytState* states = p.states();
    const engine::lyt::LytVertex* vertices = p.vertices();
    u32 state = ~0u;
    for (const engine::lyt::LytRun *r = p.runs(), *end = r + p.n_runs; r < end; r++) {
        if (r->state >= p.n_states || r->first > p.n_vertices || r->count > p.n_vertices - r->first)
            continue;
        // the quads the immediate vertices and the indices have room for (a GX draw that doesn't
        // fit isn't drawn either)
        u32 quads = r->count / 4;
        const u32 room = std::min((P.vend - P.vused) / 4, (P.iend - P.iused) / 6);
        if (quads > room) {
            T.lyt_dropped += quads - room;
            if (P.warned++ < 4)
                log("pica: vertex buffer full: %u of a layout's quads left out", quads - room);
            quads = room;
        }
        if (!quads)
            continue;
        step();
        if (r->state != state) {
            NOMA_PROF_SCOPE("(lyt pica state)");
            lyt_state(states[r->state], now);
            state = r->state;
        }
        NOMA_PROF_SCOPE("(lyt pica quads)");
        T.lyt_runs++, T.lyt_quads += quads;
        // the vertices as the shader takes them (slots 0: the identity), each quad's corners (top
        // left, top right, bottom right, bottom left) as two triangles
        const engine::lyt::LytVertex* v = vertices + r->first;
        const u32 nv = 4 * quads;
        PVertex* o = P.vbuf + P.vused;
        for (u32 i = 0; i < nv; i++, v++, o++) {
            o->pos[0] = v->pos[0], o->pos[1] = v->pos[1], o->pos[2] = v->pos[2];
            o->nrm[0] = o->nrm[1] = o->nrm[2] = 0.0f;
            memcpy(o->color, v->color, 4);
            for (int u = 0; u < 3; u++) {
                const int k = now.coord[u];
                o->tc[u][0] = k >= 0 ? v->tc[k][0] : 0.0f;
                o->tc[u][1] = k >= 0 ? v->tc[k][1] : 0.0f;
                o->tc[u][2] = k >= 0 ? 1.0f : 0.0f;
            }
        }
        memset(P.slots + P.vused, 0, nv * sizeof(GxTnlSlots));
        slots_written(P.vused, nv);
        u16* idx = P.ibuf + P.iused;
        for (u32 q = 0, b = P.vused; q < quads; q++, b += 4, idx += 6) {
            idx[0] = (u16)b, idx[1] = (u16)(b + 1), idx[2] = (u16)(b + 2);
            idx[3] = (u16)b, idx[4] = (u16)(b + 2), idx[5] = (u16)(b + 3);
        }
        if (!P.batch_count)
            P.batch_first = P.iused;
        P.batch_count += 6 * quads;
        P.iused += 6 * quads;
        P.vused += nv;
    }
    // the last batch now: the palette's slots are the GX draws' again
    flush_batch();
    T.lyt_batches += P.batches - batches0;

    // The GX draws after the packet work their state out again in full (all_dirty) and their
    // constants from their plans (consts_plan); what the packet left bound, set and uploaded
    // (P.env, P.tex, P.cull, P.consts, D) is what the PICA has, so what they find the same isn't
    // sent again. P.setup stays theirs.
    P.all_dirty = true;
    P.consts_plan = 0;
    if (timers_on())  // (Timed adds the whole packet's time as it goes out of scope: LYT keeps its own)
        T.ticks[Timers::LYT] -= timers_but_lyt() - nested0;
}

}  // namespace guest
