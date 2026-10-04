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
// EFB copies to the frame buffer need nothing (the frame is already on screen); for a copy to
// a texture the GPU finishes the frame so far, the render target is read back and converted
// like the host renderer's EFB (scaled from 400x240), and the frame goes on.
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

namespace guest {

namespace {

const float FRAME_W = 640.0f, FRAME_H = 456.0f;  // the game's frame buffer
// The vertex pool: one 16-bit index space. [0, IMM_VERTICES) holds the frame's immediate draws
// (written again every frame), the rest the display lists' vertices, kept from frame to frame
// (the display-list cache below). The palette slots are a buffer of their own beside it, so that
// a list's vertices stay untouched when its matrices get other slots.
// Sized from the room and the scenes before it, about twice what they use at most: 9K immediate
// vertices and 23K indices in a frame, 10.6K list vertices kept (linear heap is the Old 3DS's
// scarcest memory: textures, the guest's MEM1, the sound).
const u32 POOL_VERTICES = 40 << 10;  // 2.5 MB + 160 KB of slots
const u32 IMM_VERTICES = 20 << 10;
const u32 IBUF_INDICES = 128 << 10;  // 256 KB

#define DISPLAY_TRANSFER_FLAGS                                                                  \
    (GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |           \
     GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | \
     GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

using PVertex = GxTnlVertex;  // the vertex shader's inputs (tnl.h)
static_assert(sizeof(PVertex) == 64, "PVertex layout");

// ---- the display-list cache: a list's draws, with their vertices in the pool, found again by
// the list and the draw command's offset in it; valid while the draw's bytes, the vertex arrays
// they index (by write generation), the CP registers they were decoded with and what the shader
// wants of a vertex (the recipe) stay the same. A list the game writes again every frame (two in
// the room: 132 draws) mostly keeps its draws' bytes: they are compared when the list changed.
struct DlRange {
    u32 attr, addr, size, gen;
};
struct DlRecord {
    u32 offset = 0;            // the draw command's offset in the list
    u32 first = 0, count = 0;  // pool vertices
    u32 prim = 0, vat = 0;
    u32 regs[5] = {};          // VCD lo/hi, the VAT's A/B/C
    u32 nranges = 0;
    DlRange ranges[12];
    u32 arrays[12][2];         // base, stride
    u8 recipe[12] = {};
    GxTnlSlots slots = {};     // the slots all its vertices were given (fixed-matrix draws)
    u32 slots_frame = 0;       // the frame they were written in (0: never)
    u64 data_hash = 0;         // its vertex data's bytes in the list ...
    u32 list_gen = 0;          // ... as they were at this write generation of the list
};
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
};

struct State {
    bool ready = false;
    C3D_RenderTarget* top = nullptr;
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
    GxTnlSlots* slots = nullptr;
    GxTnlRows* rows = nullptr;  // (CPU only: the lists' vertices' matrix rows)
    u16* ibuf = nullptr;
    u32 vused = 0, iused = 0;  // immediate vertices and indices used this frame
    u32 cache_next = IMM_VERTICES;  // the pool's first free cache vertex
    bool cache_full = false;   // (lists go the immediate way until the cache starts over)
    u32 vmax = 0, imax = 0;    // ... at most, since the last report
    u32 draws = 0, batches = 0, setups = 0;  // this frame
    u32 cached_draws = 0, cached_vertices = 0, built_vertices = 0;  // this frame
    u32 rebuilt_arrays = 0, rebuilt_regs = 0, drawn_twice = 0, no_room = 0;  // why list draws were built
    u32 cache_made = 0, lists_rewritten = 0, lists_new = 0;  // pool vertices given out; lists whose bytes changed, new
    // what the GPU must see of memory the CPU wrote this frame: slot and cache vertex ranges
    u32 slots_lo = ~0u, slots_hi = 0, cache_lo = ~0u, cache_hi = 0;
    DlEntry* dl = nullptr;     // the display-list cache
    u32 dl_addr = 0, dl_offset = ~0u;  // the list call being drawn (its generation checked)
    bool in_frame = false;
    bool resume = false;  // the frame goes on (its first part went to the GPU before an applet)
    u32 frame = 0;
    std::unordered_map<u32, TexEntry> textures;  // by GxTexture id
    std::vector<C3D_Tex> dead;                   // freed after the GPU finished the frame
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
};
State P;

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
// something itself, so only when asked); the profiler's scopes cover the same parts for free
struct Timers {
    enum { PLAN, CONSTS, VERTICES, SLOTS, SUBMIT, FLUSH, TEV, TEXTURES, UPLOAD, BIND, PIXEL, XFORM, FRAME, END, CLEAR, READBACK, COPY, N };
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
    u64 t0;
    explicit Timed(int which) : k(which), t0(timers_on() ? svcGetSystemTick() : 0) {}
    ~Timed() {
        if (t0)
            T.ticks[k] += svcGetSystemTick() - t0, T.calls[k]++;
    }
};
static const char* const k_timer_names[Timers::N] = {"plan", "consts", "vertices", "slots", "submit", "flush", "tev",
                                                     "textures", "upload", "bind", "pixel", "xform", "frame", "end", "clear",
                                                     "readback", "copy"};

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
    char line[1024];
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
    snprintf(line + n, sizeof(line) - n, "; palette %u fills %u checks (%u stale) %u searches %u reused; %u list vertices made; "
             "cached draws: %u with matrices per vertex, %u fixed with slots rewritten (%u vertices)",
             (ps.fills - T.pal0.fills) / T.frames, (ps.checks - T.pal0.checks) / T.frames,
             (ps.stale - T.pal0.stale) / T.frames, (ps.scans - T.pal0.scans) / T.frames,
             (ps.reused - T.pal0.reused) / T.frames, T.cache_made / T.frames,
             T.vary_draws / T.frames, T.slot_rewrites / T.frames, T.slot_vertices / T.frames);
    T.pal0 = ps;
    T.cache_made = T.vary_draws = T.slot_rewrites = T.slot_vertices = 0;
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
            if (id == GPUREG_DRAWELEMENTS && k < s_snaps.size()) {
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

// the attribute buffers: the pool (position, normal, color, texgen sources) and the slots
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
    u32* cmdbuf;
    u32 size, off;
    GPUCMD_GetBuffer(&cmdbuf, &size, &off);
    dump_word(0x53594850), dump_word(7);
    dump_phys("linear", (void*)__ctru_linear_heap, __ctru_linear_heap_size);
    dump_phys("color", P.top->frameBuf.colorBuf, 240 * 400 * 4);
    dump_phys("depth", P.top->frameBuf.depthBuf, 240 * 400 * 4);
    dump_phys("vbuf", P.vbuf, POOL_VERTICES * sizeof(PVertex));
    dump_phys("slots", P.slots, POOL_VERTICES * sizeof(GxTnlSlots));
    dump_phys("ibuf", P.ibuf, IBUF_INDICES * sizeof(u16));
    dump_phys("cmdbuf", cmdbuf, size * 4);
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
    if (P.vused)
        GSPGPU_FlushDataCache(P.vbuf, P.vused * sizeof(PVertex));
    if (P.cache_lo < P.cache_hi)
        GSPGPU_FlushDataCache(P.vbuf + P.cache_lo, (P.cache_hi - P.cache_lo) * sizeof(PVertex));
    if (P.slots_lo < P.slots_hi)
        GSPGPU_FlushDataCache(P.slots + P.slots_lo, (P.slots_hi - P.slots_lo) * sizeof(GxTnlSlots));
    if (P.iused)
        GSPGPU_FlushDataCache(P.ibuf, P.iused * sizeof(u16));
    P.cache_lo = P.slots_lo = ~0u, P.cache_hi = P.slots_hi = 0;
}

void end_frame() {
    flush_batch();
    if (check_state_on())
        check_state_list();
    flush_geometry();
    dump_frame_list();
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
// texture's freed address: on hardware, whatever that memory holds next (black).
void forget_texture(const C3D_Tex* t) {
    for (int u = 0; u < 3; u++) {
        if (P.tex[u] == t)
            P.tex[u] = nullptr, P.env_valid = false;
        if (D.bound[u] == t)
            D.bound[u] = nullptr, P.env_valid = false;
        if (D.c3d_bound[u] == t) {
            C3D_TexBind(u, nullptr);
            D.c3d_bound[u] = nullptr;
        }
    }
}

// drop the least recently used texture the frame hasn't drawn with (the previous frame is
// finished: C3D_FrameBegin waited for it); false when there is none
bool evict_texture() {
    auto victim = P.textures.end();
    for (auto it = P.textures.begin(); it != P.textures.end(); ++it)
        if (it->second.tex.data && it->second.last_used != P.frame &&
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

// texture memory, from the linear heap: a little stays free for screenshots and read-backs;
// older textures make room
bool tex_init(C3D_Tex* t, u32 w, u32 h, GPU_TEXCOLOR f) {
    const u32 MARGIN = 512 << 10;
    u32 need = w * h * texel_bits(f) / 8;
    for (;;) {
        if (linearSpaceFree() >= need + MARGIN && C3D_TexInit(t, w, h, f))
            return true;
        if (!evict_texture())
            return C3D_TexInit(t, w, h, f);  // nothing left to drop: whatever fits
    }
}

// NOMA_ETC1: CMPR textures as ETC1 (default: on the Old 3DS only)
bool etc1_on() {
    static int s_on = -1;
    if (s_on < 0) {
        const char* v = getenv("NOMA_ETC1");
        bool n3ds = false;
        APT_CheckNew3DS(&n3ds);
        s_on = v ? atoi(v) != 0 : !n3ds;
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

// the PICA texture for texture map `map`; scale: what to multiply its coordinates with
C3D_Tex* texture_for(u32 map, float* sx, float* sy) {
    const u8* raw = nullptr;
    const GxTexture* gt = gx_texture_raw(map, &raw);
    if (!gt || !gt->width)
        return nullptr;
    u32 mode = g_gx.bp[bp_tex(BP_TX_SETMODE0, map)];
    u32 ws = bits(mode, 0, 2), wt = bits(mode, 2, 2);
    bool linear = bits(mode, 4, 1);
    TexEntry& e = P.textures[gt->id];
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
        if (e.tex.data && (e.tex.width != W || e.tex.height != H || e.fmt != fmt)) {
            P.dead.push_back(e.tex);
            e.tex.data = nullptr;
        }
        if (!e.tex.data && !tex_init(&e.tex, W, H, fmt)) {
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
    }
    *sx = e.sx;
    *sy = e.sy;
    return &e.tex;
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

void collect_textures() {
    for (C3D_Tex& t : P.dead)
        C3D_TexDelete(&t);
    P.dead.clear();
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
// A value a GX stage reads, as a PICA combiner source and operand.
struct In {
    enum Kind { ZERO, ONE, CONST, SRC } kind = ZERO;
    GPU_TEVSRC src = GPU_PRIMARY_COLOR;
    int op = 0;              // operand (GPU_TEVOP_RGB_* or GPU_TEVOP_A_*)
    u32 value = 0;           // CONST: 0xAABBGGRR (the combiner constant's layout)
    bool pair_prev = false;  // PREVIOUS meaning the first stage of a two-stage pair
};

u32 pack(const s32* c) {
    auto u = [](s32 v) { return (u32)(v < 0 ? 0 : v > 255 ? 255 : v); };
    return u(c[0]) | u(c[1]) << 8 | u(c[2]) << 16 | u(c[3]) << 24;
}

const s32 k_konst[8] = {255, 223, 191, 159, 128, 96, 64, 32};

struct Builder {
    C3D_TexEnv env[6];  // the combiner stages it sets up
    int stage = 0;  // next PICA stage
    bool overflow = false;
    // where the GX registers live: kind CONST (never written) or the stage that wrote them
    int reg_stage[4][2];  // [register][0 color, 1 alpha] -> PICA stage index, -1 = constant
    int prev_written = -1;  // last PICA stage that produced output
    u32 buf_rgb_mask = 0, buf_a_mask = 0;
    struct Unit {
        int map = -1, coord = -1;
    } units[3];
};

// texture unit for (map, coord), or -1
int unit_for(Builder& b, u32 map, u32 coord) {
    for (int i = 0; i < 3; i++)
        if (b.units[i].map == (int)map && b.units[i].coord == (int)coord)
            return i;
    for (int i = 0; i < 3; i++)
        if (b.units[i].map < 0) {
            b.units[i].map = map, b.units[i].coord = coord;
            return i;
        }
    return -1;
}

GPU_TEVSRC tex_src(int unit) {
    return unit == 0 ? GPU_TEXTURE0 : unit == 1 ? GPU_TEXTURE1 : GPU_TEXTURE2;
}

struct StageCtx {
    int tex_unit;   // -1: no texture
    s32 konst_c[3], konst_a;
};

// a register written by stage `s` is read by stage `at`: PREVIOUS or the combiner buffer
In reg_source(Builder& b, int s, int at, bool alpha, int channel_op) {
    In in;
    in.kind = In::SRC;
    if (s == at - 1) {
        in.src = GPU_PREVIOUS;
    } else {
        in.src = GPU_PREVIOUS_BUFFER;
        if (s <= 3) {
            if (alpha)
                b.buf_a_mask |= 1u << s;
            else
                b.buf_rgb_mask |= 1u << s;
        }
    }
    in.op = channel_op;
    return in;
}

// GX color argument -> In
In color_arg(Builder& b, u32 arg, const StageCtx& sc, int at) {
    In in;
    auto from_reg = [&](int r, bool alpha_of) -> In {
        int s = b.reg_stage[r][alpha_of ? 1 : 0];
        if (s < 0) {
            In c;
            c.kind = In::CONST;
            const s32* v = g_gx.tev_reg[r];
            s32 rgba[4] = {alpha_of ? v[3] : v[0], alpha_of ? v[3] : v[1], alpha_of ? v[3] : v[2], v[3]};
            c.value = pack(rgba);
            return c;
        }
        return reg_source(b, s, at, alpha_of, alpha_of ? GPU_TEVOP_RGB_SRC_ALPHA : GPU_TEVOP_RGB_SRC_COLOR);
    };
    switch (arg) {
    case 0: return from_reg(0, false);
    case 1: return from_reg(0, true);
    case 2: return from_reg(1, false);
    case 3: return from_reg(1, true);
    case 4: return from_reg(2, false);
    case 5: return from_reg(2, true);
    case 6: return from_reg(3, false);
    case 7: return from_reg(3, true);
    case 8:
    case 9:
        if (sc.tex_unit < 0) {
            in.kind = In::ONE;
            return in;
        }
        in.kind = In::SRC;
        in.src = tex_src(sc.tex_unit);
        in.op = arg == 8 ? GPU_TEVOP_RGB_SRC_COLOR : GPU_TEVOP_RGB_SRC_ALPHA;
        return in;
    case 10:
    case 11:
        in.kind = In::SRC;
        in.src = GPU_PRIMARY_COLOR;
        in.op = arg == 10 ? GPU_TEVOP_RGB_SRC_COLOR : GPU_TEVOP_RGB_SRC_ALPHA;
        return in;
    case 12: in.kind = In::ONE; return in;
    case 13: {
        in.kind = In::CONST;
        s32 h[4] = {128, 128, 128, 128};
        in.value = pack(h);
        return in;
    }
    case 14: {
        in.kind = In::CONST;
        s32 k[4] = {sc.konst_c[0], sc.konst_c[1], sc.konst_c[2], sc.konst_a};
        in.value = pack(k);
        return in;
    }
    default: return in;  // ZERO
    }
}

In alpha_arg(Builder& b, u32 arg, const StageCtx& sc, int at) {
    In in;
    auto from_reg = [&](int r) -> In {
        int s = b.reg_stage[r][1];
        if (s < 0) {
            In c;
            c.kind = In::CONST;
            s32 v = g_gx.tev_reg[r][3];
            s32 rgba[4] = {v, v, v, v};
            c.value = pack(rgba);
            return c;
        }
        return reg_source(b, s, at, true, GPU_TEVOP_A_SRC_ALPHA);
    };
    switch (arg) {
    case 0: return from_reg(0);
    case 1: return from_reg(1);
    case 2: return from_reg(2);
    case 3: return from_reg(3);
    case 4:
        if (sc.tex_unit < 0) {
            in.kind = In::ONE;
            return in;
        }
        in.kind = In::SRC;
        in.src = tex_src(sc.tex_unit);
        in.op = GPU_TEVOP_A_SRC_ALPHA;
        return in;
    case 5:
        in.kind = In::SRC;
        in.src = GPU_PRIMARY_COLOR;
        in.op = GPU_TEVOP_A_SRC_ALPHA;
        return in;
    case 6: {
        in.kind = In::CONST;
        s32 k[4] = {sc.konst_a, sc.konst_a, sc.konst_a, sc.konst_a};
        in.value = pack(k);
        return in;
    }
    default: return in;
    }
}

// One channel (color or alpha) of a PICA stage being built.
struct Half {
    GPU_COMBINEFUNC func = GPU_REPLACE;
    In s[3];
    GPU_TEVSCALE scale = GPU_TEVSCALE_1;
    bool used = false;
};

// a channel's part of a constant input (rgb or alpha): the stage's constant color holds both
u32 channel_mask(bool alpha) { return alpha ? 0xFF000000u : 0x00FFFFFFu; }
u32 const_value(const In& i) { return i.kind == In::ONE ? 0xFFFFFFFF : i.kind == In::ZERO ? 0 : i.value; }
bool is_zero(const In& i, bool alpha) {
    return i.kind == In::ZERO || (i.kind == In::CONST && (i.value & channel_mask(alpha)) == 0);
}
bool is_one(const In& i, bool alpha) {
    return i.kind == In::ONE || (i.kind == In::CONST && (alpha ? (i.value >> 24) == 255 : (i.value & 0xFFFFFF) == 0xFFFFFF));
}

// Plan one GX combiner (a, b, c, d, op, bias, scale) as <= 2 PICA halves. Returns the number of
// PICA stages it takes (1 or 2); the second stage reads the first as PREVIOUS.
int plan(In a, In b, In c, In d, bool sub, u32 bias, u32 scale, bool alpha, Half out[2]) {
    In prev;
    prev.kind = In::SRC;
    prev.src = GPU_PREVIOUS;
    prev.op = alpha ? GPU_TEVOP_A_SRC_ALPHA : GPU_TEVOP_RGB_SRC_COLOR;
    prev.pair_prev = true;
    // lerp(a, b, c) simplified to x (single term) or a product, or a full interpolation
    In x;
    enum { NONE, SINGLE, PRODUCT, LERP } shape;
    In p0, p1;
    if (a.kind != In::SRC && b.kind != In::SRC && c.kind != In::SRC) {
        // all constants: one constant (as GX computes it)
        u32 va = const_value(a), vb = const_value(b), vc = const_value(c), v = 0;
        for (int sh = 0; sh < 32; sh += 8) {
            u32 ka = va >> sh & 255, kb = vb >> sh & 255, kc = (vc >> sh & 255) + (vc >> sh >> 7 & 1);
            v |= (ka * (256 - kc) + kb * kc) >> 8 << sh;
        }
        x.kind = In::CONST, x.value = v;
        shape = is_zero(x, alpha) ? NONE : SINGLE;
    } else if (is_zero(c, alpha)) {
        shape = is_zero(a, alpha) ? NONE : SINGLE, x = a;
    } else if (is_one(c, alpha)) {
        shape = is_zero(b, alpha) ? NONE : SINGLE, x = b;
    } else if (is_zero(a, alpha)) {
        if (is_zero(b, alpha))
            shape = NONE;
        else if (is_one(b, alpha))
            shape = SINGLE, x = c;
        else
            shape = PRODUCT, p0 = b, p1 = c;
    } else if (is_zero(b, alpha)) {
        // a * (1 - c)
        shape = PRODUCT, p0 = a, p1 = c;
        p1.op = alpha ? (p1.op == GPU_TEVOP_A_SRC_ALPHA ? GPU_TEVOP_A_ONE_MINUS_SRC_ALPHA : p1.op)
                      : (p1.op == GPU_TEVOP_RGB_SRC_COLOR ? GPU_TEVOP_RGB_ONE_MINUS_SRC_COLOR
                                                          : GPU_TEVOP_RGB_ONE_MINUS_SRC_ALPHA);
        if (p1.kind == In::CONST) {  // 1 - constant
            u32 v = p1.value;
            p1.value = (255 - (v & 255)) | (255 - ((v >> 8) & 255)) << 8 | (255 - ((v >> 16) & 255)) << 16 |
                       (255 - (v >> 24)) << 24;
            p1.op = alpha ? GPU_TEVOP_A_SRC_ALPHA : GPU_TEVOP_RGB_SRC_COLOR;
        }
    } else {
        shape = LERP;
    }
    bool has_d = !is_zero(d, alpha);
    // A stage has one constant color: a lerp between two different constants (the year
    // picker's digits: lerp(C0, C1, texture)) takes one of them from a stage of its own first
    auto distinct_consts = [&](const In& p, const In& q) {
        return p.kind != In::SRC && q.kind != In::SRC &&
               ((const_value(p) ^ const_value(q)) & channel_mask(alpha)) != 0;
    };
    bool hoist_a = false, hoist_b = false;
    if (shape == LERP && !sub && !has_d) {
        if (distinct_consts(a, b) || distinct_consts(a, c))
            hoist_a = true;
        else if (distinct_consts(b, c))
            hoist_b = true;
    }
    GPU_TEVSCALE sc = scale == 1 ? GPU_TEVSCALE_2 : scale == 2 ? GPU_TEVSCALE_4 : GPU_TEVSCALE_1;
    int n = 0;
    auto half = [&](GPU_COMBINEFUNC f, In s0, In s1 = In(), In s2 = In()) {
        out[n].func = f;
        out[n].s[0] = s0, out[n].s[1] = s1, out[n].s[2] = s2;
        out[n].used = true;
        n++;
    };
    if (sub) {
        // d - x
        if (shape == NONE)
            half(GPU_REPLACE, d);
        else if (shape == SINGLE)
            half(GPU_SUBTRACT, d, x);
        else {
            if (shape == PRODUCT)
                half(GPU_MODULATE, p0, p1);
            else
                half(GPU_INTERPOLATE, b, a, c);
            half(GPU_SUBTRACT, d, prev);
        }
    } else if (bias == 2 && has_d && shape == SINGLE) {
        half(GPU_ADD_SIGNED, d, x);  // d + x - 0.5
        bias = 0;
    } else {
        switch (shape) {
        case NONE: half(GPU_REPLACE, d); break;
        case SINGLE:
            if (has_d)
                half(GPU_ADD, d, x);
            else
                half(GPU_REPLACE, x);
            break;
        case PRODUCT:
            if (has_d)
                half(GPU_MULTIPLY_ADD, p0, p1, d);
            else
                half(GPU_MODULATE, p0, p1);
            break;
        case LERP:
            if (hoist_a)
                half(GPU_REPLACE, a), half(GPU_INTERPOLATE, b, prev, c);
            else if (hoist_b)
                half(GPU_REPLACE, b), half(GPU_INTERPOLATE, prev, a, c);
            else
                half(GPU_INTERPOLATE, b, a, c);
            if (has_d)
                half(GPU_ADD, prev, d);
            break;
        }
    }
    if (bias == 1 || bias == 2) {  // +-0.5 that couldn't be folded: one more stage
        In h;
        h.kind = In::CONST;
        h.value = 0x80808080;
        if (n < 2)
            half(bias == 1 ? GPU_ADD : GPU_SUBTRACT, prev, h);
    }
    if (n)
        out[n - 1].scale = sc;
    return n ? n : (half(GPU_REPLACE, d), 1);
}

// the constants a half uses (ZERO and ONE included), merged into the stage's one constant
// color; false if they conflict. have: 1 = rgb set, 2 = alpha set
int used_inputs(GPU_COMBINEFUNC f) {
    switch (f) {
    case GPU_REPLACE: return 1;
    case GPU_INTERPOLATE:
    case GPU_MULTIPLY_ADD:
    case GPU_ADD_MULTIPLY: return 3;
    default: return 2;
    }
}

bool merge_consts(const Half& h, bool alpha, u32* color, u32* have) {
    for (int i = 0; i < used_inputs(h.func); i++) {
        const In& in = h.s[i];
        if (in.kind == In::SRC)
            continue;
        u32 v = const_value(in);
        u32 mask = channel_mask(alpha);
        u32 bit = alpha ? 2 : 1;
        if ((*have & bit) && ((*color ^ v) & mask))
            return false;
        *color = (*color & ~mask) | (v & mask);
        *have |= bit;
    }
    return true;
}

void apply_half(C3D_TexEnv* env, const Half& h, bool alpha) {
    GPU_TEVSRC src[3];
    int ops[3];
    for (int i = 0; i < 3; i++) {
        const In& in = h.s[i];
        if (in.kind == In::SRC) {
            src[i] = in.src;
            ops[i] = in.op;
        } else {
            // constants and ONE come from the stage's constant color; ZERO is the "one minus"
            // of a constant one... use the constant with an operand that gives 0: there is
            // none, so a zero input only appears where the function ignores it
            src[i] = GPU_CONSTANT;
            ops[i] = alpha ? GPU_TEVOP_A_SRC_ALPHA : GPU_TEVOP_RGB_SRC_COLOR;
        }
    }
    if (alpha) {
        C3D_TexEnvSrc(env, C3D_Alpha, src[0], src[1], src[2]);
        C3D_TexEnvOpAlpha(env, (GPU_TEVOP_A)ops[0], (GPU_TEVOP_A)ops[1], (GPU_TEVOP_A)ops[2]);
        C3D_TexEnvFunc(env, C3D_Alpha, h.func);
        C3D_TexEnvScale(env, C3D_Alpha, h.scale);
    } else {
        C3D_TexEnvSrc(env, C3D_RGB, src[0], src[1], src[2]);
        C3D_TexEnvOpRgb(env, (GPU_TEVOP_RGB)ops[0], (GPU_TEVOP_RGB)ops[1], (GPU_TEVOP_RGB)ops[2]);
        C3D_TexEnvFunc(env, C3D_RGB, h.func);
        C3D_TexEnvScale(env, C3D_RGB, h.scale);
    }
}

u32 s_unsupported;

// The PICA texture combiners for the current TEV configuration (in b.env, with the combiner
// buffer masks); fills units[] with the (map, coord) pair each texture unit samples.
void tev_to_texenv(Builder& b) {
    const u32* bp = g_gx.bp;
    u32 num = bits(bp[BP_GENMODE], 10, 4) + 1;
    for (int r = 0; r < 4; r++)
        b.reg_stage[r][0] = b.reg_stage[r][1] = -1;
    int last_c = -1, last_a = -1;  // the PICA stages holding the last GX stage's results
    for (u32 i = 0; i < num && !b.overflow; i++) {
        u32 ce = bp[BP_TEV_COLOR_ENV + 2 * i], ae = bp[BP_TEV_COLOR_ENV + 2 * i + 1];
        u32 tref = bp[BP_TREF + i / 2] >> ((i & 1) * 12);
        u32 ksel = bp[BP_TEV_KSEL + i / 2] >> ((i & 1) * 10);
        StageCtx sc;
        sc.tex_unit = bits(tref, 6, 1) ? unit_for(b, bits(tref, 0, 3), bits(tref, 3, 3)) : -1;
        u32 kc = bits(ksel, 4, 5), ka = bits(ksel, 9, 5);
        for (int k = 0; k < 3; k++) {
            if (kc < 8)
                sc.konst_c[k] = k_konst[kc];
            else if (kc >= 12 && kc < 16)
                sc.konst_c[k] = g_gx.tev_konst[kc - 12][k];
            else if (kc >= 16)
                sc.konst_c[k] = g_gx.tev_konst[(kc - 16) & 3][(kc - 16) >> 2];
            else
                sc.konst_c[k] = 0;
        }
        sc.konst_a = ka < 8 ? k_konst[ka] : ka >= 16 ? g_gx.tev_konst[(ka - 16) & 3][(ka - 16) >> 2] : 0;

        int at = b.stage;
        Half ch[2], ah[2];
        int nc, na;
        if (bits(ce, 16, 2) == 3 || bits(ae, 16, 2) == 3)
            s_unsupported++;  // compare modes: approximated by the regular formula
        nc = plan(color_arg(b, bits(ce, 12, 4), sc, at), color_arg(b, bits(ce, 8, 4), sc, at),
                  color_arg(b, bits(ce, 4, 4), sc, at), color_arg(b, bits(ce, 0, 4), sc, at), bits(ce, 18, 1),
                  bits(ce, 16, 2) == 3 ? 0 : bits(ce, 16, 2), bits(ce, 16, 2) == 3 ? 0 : bits(ce, 20, 2), false, ch);
        na = plan(alpha_arg(b, bits(ae, 13, 3), sc, at), alpha_arg(b, bits(ae, 10, 3), sc, at),
                  alpha_arg(b, bits(ae, 7, 3), sc, at), alpha_arg(b, bits(ae, 4, 3), sc, at), bits(ae, 18, 1),
                  bits(ae, 16, 2) == 3 ? 0 : bits(ae, 16, 2), bits(ae, 16, 2) == 3 ? 0 : bits(ae, 20, 2), true, ah);
        int n = std::max(nc, na);
        if (b.stage + n > 6) {
            b.overflow = true;
            s_unsupported++;
            break;
        }
        for (int k = 0; k < n; k++) {
            C3D_TexEnv* env = &b.env[b.stage + k];
            C3D_TexEnvInit(env);
            u32 konst = 0;
            bool have = false;
            // the half that is shorter passes its result on
            Half c = k < nc ? ch[k] : Half(), a = k < na ? ah[k] : Half();
            // (its PREVIOUS is this pair's first stage, not the stage before the pair)
            if (k >= nc) {
                c.func = GPU_REPLACE;
                c.s[0].kind = In::SRC, c.s[0].src = GPU_PREVIOUS, c.s[0].op = GPU_TEVOP_RGB_SRC_COLOR;
                c.s[0].pair_prev = true;
            }
            if (k >= na) {
                a.func = GPU_REPLACE;
                a.s[0].kind = In::SRC, a.s[0].src = GPU_PREVIOUS, a.s[0].op = GPU_TEVOP_A_SRC_ALPHA;
                a.s[0].pair_prev = true;
            }
            if (k == 1) {
                // the second stage of a pair: PREVIOUS now means the first stage's result; an
                // input that meant the stage before the pair reads it from the combiner buffer
                // (which a stage sees with one stage of delay)
                for (Half* h : {&c, &a})
                    for (In& x : h->s)
                        if (x.kind == In::SRC && x.src == GPU_PREVIOUS && !x.pair_prev) {
                            if (at - 1 >= 0 && at - 1 <= 3) {
                                x.src = GPU_PREVIOUS_BUFFER;
                                if (h == &a)
                                    b.buf_a_mask |= 1u << (at - 1);
                                else
                                    b.buf_rgb_mask |= 1u << (at - 1);
                            } else {
                                s_unsupported++;
                            }
                        }
            }
            u32 have_bits = 0;
            // (both always: a conflict in the color half mustn't leave the alpha's constant out)
            bool merged_c = merge_consts(c, false, &konst, &have_bits);
            bool merged_a = merge_consts(a, true, &konst, &have_bits);
            if (!merged_c || !merged_a)
                s_unsupported++;  // two different constants in one stage: one of them wins
            (void)have;
            apply_half(env, c, false);
            apply_half(env, a, true);
            C3D_TexEnvColor(env, konst);
        }
        b.stage += n;
        // where this GX stage's outputs went
        u32 cdest = bits(ce, 22, 2), adest = bits(ae, 22, 2);
        last_c = last_a = b.stage - 1;
        b.reg_stage[cdest][0] = last_c;
        b.reg_stage[adest][1] = last_a;
    }
    // unused stages pass the result on
    for (int s = b.stage; s < 6; s++) {
        C3D_TexEnv* env = &b.env[s];
        C3D_TexEnvInit(env);
        C3D_TexEnvSrc(env, C3D_Both, GPU_PREVIOUS, GPU_PREVIOUS, GPU_PREVIOUS);
        C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
    }
}

// ---------------------------------------------------------------- combiner setups, kept
// The layouts switch between a few materials all the time: each TEV configuration is
// translated once and kept, found again by the registers the translation reads.
struct TevSetup {
    u32 key[1 + 32 + 16 + 32];  // stages; their color and alpha stages; TREF and KSEL; registers
    u32 nkey = 0;
    C3D_TexEnv env[6];
    u32 buf_rgb = 0, buf_a = 0;
    Builder::Unit units[3];
};
TevSetup s_tev_setups[256];  // by a hash of the key

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
    u32 h = 2166136261u;
    for (u32 i = 0; i < n; i++)
        h = (h ^ key[i]) * 16777619u;
    TevSetup& e = s_tev_setups[(h ^ h >> 16) & 255];
    if (e.nkey == n && memcmp(e.key, key, n * 4) == 0)
        return e;
    T.tev_misses++;
    Builder b;
    tev_to_texenv(b);
    memcpy(e.key, key, n * 4);
    e.nkey = n;
    memcpy(e.env, b.env, sizeof(e.env));
    e.buf_rgb = b.buf_rgb_mask, e.buf_a = b.buf_a_mask;
    memcpy(e.units, b.units, sizeof(e.units));
    return e;
}

// The combiners as a draw's textures need them: a texture kept as luminance only (L4, L8: GX's I4
// and I8) samples with alpha 1, where GX's intensity texture has alpha = intensity: what reads its
// alpha reads its red instead
void remap_luminance(const C3D_TexEnv in[6], C3D_Tex* const tex[3], C3D_TexEnv out[6]) {
    memcpy(out, in, sizeof(C3D_TexEnv) * 6);
    u32 lum = 0;  // units with a luminance texture
    for (int u = 0; u < 3; u++)
        if (tex[u] && (tex[u]->fmt == GPU_L4 || tex[u]->fmt == GPU_L8))
            lum |= 1u << u;
    if (!lum)
        return;
    for (int i = 0; i < 6; i++) {
        C3D_TexEnv& e = out[i];
        u32 op_rgb = e.opRgb, op_a = e.opAlpha;
        for (int k = 0; k < 3; k++) {
            u32 src = (e.srcRgb >> (4 * k)) & 0xF;
            if (src >= GPU_TEXTURE0 && src <= GPU_TEXTURE2 && (lum >> (src - GPU_TEXTURE0) & 1)) {
                u32 op = (op_rgb >> (4 * k)) & 0xF;
                if (op == GPU_TEVOP_RGB_SRC_ALPHA || op == GPU_TEVOP_RGB_ONE_MINUS_SRC_ALPHA)
                    op_rgb = (op_rgb & ~(0xFu << (4 * k))) | ((op + 2) << (4 * k));  // -> SRC_R, ONE_MINUS_SRC_R
            }
            src = (e.srcAlpha >> (4 * k)) & 0xF;
            if (src >= GPU_TEXTURE0 && src <= GPU_TEXTURE2 && (lum >> (src - GPU_TEXTURE0) & 1)) {
                u32 op = (op_a >> (4 * k)) & 0xF;
                if (op == GPU_TEVOP_A_SRC_ALPHA || op == GPU_TEVOP_A_ONE_MINUS_SRC_ALPHA)
                    op_a = (op_a & ~(0xFu << (4 * k))) | ((op + 2) << (4 * k));  // -> SRC_R, ONE_MINUS_SRC_R
            }
        }
        e.opRgb = op_rgb, e.opAlpha = op_a;
    }
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

void pixel_state() {
    Timed timed(Timers::PIXEL);
    const u32* bp = g_gx.bp;
    u32 zmode = bp[BP_ZMODE], cm = bp[BP_CMODE0];
    u32 pixfmt = bits(bp[BP_ZCONTROL], 0, 3);
    int mask = 0;
    if (cm & 0x08)
        mask |= GPU_WRITE_RED | GPU_WRITE_GREEN | GPU_WRITE_BLUE;  // GPU_WRITE_COLOR has alpha too
    if ((cm & 0x10) && pixfmt == 1)
        mask |= GPU_WRITE_ALPHA;
    bool ztest = zmode & 1;
    if (ztest && (zmode & 0x10))
        mask |= GPU_WRITE_DEPTH;
    st_depth(true, ztest ? test_func(bits(zmode, 1, 3)) : GPU_ALWAYS, (GPU_WRITEMASK)mask);

    if (cm & 1) {
        if (cm & 0x800)
            st_blend(GPU_BLEND_REVERSE_SUBTRACT, GPU_BLEND_REVERSE_SUBTRACT, GPU_ONE, GPU_ONE, GPU_ONE, GPU_ONE);
        else {
            GPU_BLENDFACTOR s = blend_factor(bits(cm, 8, 3), true), d = blend_factor(bits(cm, 5, 3), false);
            st_blend(GPU_BLEND_ADD, GPU_BLEND_ADD, s, d, s, d);
        }
    } else if (cm & 2) {
        static const GPU_LOGICOP ops[16] = {GPU_LOGICOP_CLEAR, GPU_LOGICOP_AND, GPU_LOGICOP_AND_REVERSE,
                                           GPU_LOGICOP_COPY, GPU_LOGICOP_AND_INVERTED, GPU_LOGICOP_NOOP,
                                           GPU_LOGICOP_XOR, GPU_LOGICOP_OR, GPU_LOGICOP_NOR,
                                           GPU_LOGICOP_EQUIV, GPU_LOGICOP_INVERT, GPU_LOGICOP_OR_REVERSE,
                                           GPU_LOGICOP_COPY_INVERTED, GPU_LOGICOP_OR_INVERTED,
                                           GPU_LOGICOP_NAND, GPU_LOGICOP_SET};
        st_logic_op(ops[bits(cm, 12, 4)]);
    } else {
        st_blend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
    }

    // alpha test: the PICA has one comparison; keep the one that isn't "always"
    u32 ac = bp[BP_ALPHACOMPARE];
    u32 c0 = bits(ac, 16, 3), c1 = bits(ac, 19, 3), op = bits(ac, 22, 2);
    u32 r0 = bits(ac, 0, 8), r1 = bits(ac, 8, 8);
    if (c0 == 7 && c1 == 7)
        st_alpha_test(false, GPU_ALWAYS, 0);
    else if (op == 0 && c1 == 7)
        st_alpha_test(true, test_func(c0), r0);
    else if (op == 0 && c0 == 7)
        st_alpha_test(true, test_func(c1), r1);
    else if (op == 1 && c1 == 0)
        st_alpha_test(true, test_func(c0), r0);
    else if (op == 1 && c0 == 0)
        st_alpha_test(true, test_func(c1), r1);
    else
        st_alpha_test(true, test_func(c0), r0);
}

// ---------------------------------------------------------------- geometry
// GX clip space -> PICA clip space: viewport into the 640x456 frame, scaled to 400x240, rotated
// for the 240x400 render target (citro3d's Mtx_OrthoTilt convention), GX depth range. The shader
// gets it folded into the projection (gx_tnl_plan).
void set_xform() {
    Timed timed(Timers::XFORM);
    float vsx = xf_f32(XF_VIEWPORT + 0), vsy = xf_f32(XF_VIEWPORT + 1), vsz = xf_f32(XF_VIEWPORT + 2);
    float vox = xf_f32(XF_VIEWPORT + 3), voy = xf_f32(XF_VIEWPORT + 4), voz = xf_f32(XF_VIEWPORT + 5);
    float kx = 400.0f / FRAME_W, ky = 240.0f / FRAME_H;
    f32 m[4][4];
    memset(m, 0, sizeof(m));
    // x' = 1 - Y/120, Y = (y/w * vsy + voy - 342) * ky
    m[0][1] = -vsy * ky / 120.0f;
    m[0][3] = 1.0f - (voy - 342.0f) * ky / 120.0f;
    // y' = 1 - X/200, X = (x/w * vsx + vox - 342) * kx
    m[1][0] = -vsx * kx / 200.0f;
    m[1][3] = 1.0f - (vox - 342.0f) * kx / 200.0f;
    // the PICA keeps depth -z'/w: the GX window depth, 0-1
    m[2][2] = -vsz / 16777215.0f;
    m[2][3] = -voz / 16777215.0f;
    m[3][3] = 1.0f;
    if (memcmp(m, P.clip, sizeof(m)) != 0) {
        memcpy(P.clip, m, sizeof(m));
        P.clip_serial++;
    }

    // scissor, in the rotated frame buffer
    const u32* bp = g_gx.bp;
    float x0 = (float)bits(bp[BP_SCISSOR_TL], 12, 11) - 342.0f, y0 = (float)bits(bp[BP_SCISSOR_TL], 0, 11) - 342.0f;
    float x1 = (float)bits(bp[BP_SCISSOR_BR], 12, 11) - 342.0f + 1, y1 = (float)bits(bp[BP_SCISSOR_BR], 0, 11) - 342.0f + 1;
    auto clampf = [](float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; };
    float X0 = clampf(x0 * kx, 0, 400), X1 = clampf(x1 * kx, 0, 400);
    float Y0 = clampf(y0 * ky, 0, 240), Y1 = clampf(y1 * ky, 0, 240);
    st_scissor(GPU_SCISSOR_NORMAL, (u32)(240 - Y1), (u32)(400 - X1), (u32)std::ceil(240 - Y0),
                   (u32)std::ceil(400 - X0));
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

void begin_frame() {
    if (P.in_frame)
        return;
    {
        // no wait for the display's vblank (C3D_FRAME_SYNCDRAW): the game paces its frames by
        // the emulated retrace already, and a slow frame shouldn't wait for the next vblank too.
        // This only waits for the GPU to finish the previous frame.
        NOMA_PROF_SCOPE("(gx wait)");
        GpuWait w("for the last frame (C3D_FrameBegin)");
        C3D_FrameBegin(0);
    }
    NOMA_PROF_SCOPE("(gx frame)");
    Timed timed(Timers::FRAME);
    collect_textures();
    const u32* bp = g_gx.bp;
    u32 ar = bp[BP_CLEAR_AR], gb = bp[BP_CLEAR_GB];
    u32 color = (ar & 255) << 24 | ((gb >> 8) & 255) << 16 | (gb & 255) << 8 | 0xFF;
    if (!P.resume)
        C3D_RenderTargetClear(P.top, C3D_CLEAR_ALL, color, bp[BP_CLEAR_Z] & 0xFFFFFF);
    P.resume = false;
    C3D_FrameDrawOn(P.top);
    frame_list_starts();
    P.c3d_touched = true;
    if (stepping())  // (NOMA_PICA_STEP: the clear alone, then a command list of one register write)
        step_piece(STEP_CLEAR), step_piece(STEP_LIST);
    P.vmax = std::max(P.vmax, P.vused);
    P.imax = std::max(P.imax, P.iused);
    P.vused = P.iused = 0;
    P.draws = P.batches = P.setups = 0;
    P.cached_draws = P.cached_vertices = P.built_vertices = 0;
    P.rebuilt_arrays = P.rebuilt_regs = P.drawn_twice = P.no_room = 0;
    P.cache_made = P.lists_rewritten = P.lists_new = 0;
    P.batch_count = 0;
    P.all_dirty = true;
    P.env_valid = false;
    P.cull = ~0u;
    // every frame's palette starts the same: a frame drawing what the last one drew then gives its
    // matrices the same slots, and the list cache's draws keep the slots their vertices have
    P.pal = GxTnlPalette();
    // the GPU has finished the frame before: a full cache starts over now
    if (P.cache_full) {
        for (u32 i = 0; i < DL_ENTRIES; i++)
            P.dl[i] = DlEntry();
        P.cache_next = IMM_VERTICES;
        P.cache_full = false;
    }
    P.dl_offset = ~0u;
    P.in_frame = true;
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

// the record still describes the draw: registers, recipe, and the arrays unchanged (0), or
// what changed: 1 registers or recipe, 2 the arrays
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

int dl_record_stale(DlRecord& r, u32 vat, const GxTnlPlan& plan, u32 list_gen, const u8* data, u32 count) {
    const u32* cp = g_gx.cp;
    if (r.vat != vat || r.regs[0] != cp[CP_VCD_LO] || r.regs[1] != cp[CP_VCD_HI] || r.regs[2] != cp[CP_VAT_A + vat] ||
        r.regs[3] != cp[CP_VAT_B + vat] || r.regs[4] != cp[CP_VAT_C + vat])
        return 1;
    u8 recipe[12];
    dl_recipe(plan, recipe);
    if (memcmp(recipe, r.recipe, sizeof(recipe)) != 0)
        return 1;
    for (u32 i = 0; i < r.nranges; i++) {
        const DlRange& g = r.ranges[i];
        if (cp[CP_ARRAY_BASE + g.attr] != r.arrays[i][0] || cp[CP_ARRAY_STRIDE + g.attr] != r.arrays[i][1])
            return 1;
        if (mem_generation(g.addr, g.size) != g.gen)
            return 2;
    }
    if (r.list_gen != list_gen) {  // the list was written again: the draw's own bytes
        if (count != r.count || dl_data_hash(data, count * plan.fmt->size) != r.data_hash)
            return 2;
        r.list_gen = list_gen;
    }
    return 0;
}

}  // namespace

bool PicaRenderer::init() {
    if (!C3D_Init(C3D_DEFAULT_CMDBUF_SIZE * 8))
        return false;
    P.top = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
    if (!P.top)
        return false;
    C3D_RenderTargetSetOutput(P.top, GFX_TOP, GFX_LEFT, DISPLAY_TRANSFER_FLAGS);
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
    P.vbuf = (PVertex*)linearAlloc(POOL_VERTICES * sizeof(PVertex));
    P.slots = (GxTnlSlots*)linearAlloc(POOL_VERTICES * sizeof(GxTnlSlots));
    P.rows = (GxTnlRows*)malloc(POOL_VERTICES * sizeof(GxTnlRows));
    P.ibuf = (u16*)linearAlloc(IBUF_INDICES * sizeof(u16));
    if (P.vbuf && P.slots && P.ibuf)
        s_buf_base = std::min({osConvertVirtToPhys(P.vbuf), osConvertVirtToPhys(P.slots), osConvertVirtToPhys(P.ibuf)}) &
                     ~0xFu;
    log("pica: vertex and index buffers from physical %08lX", (unsigned long)s_buf_base);
    P.dl = new DlEntry[DL_ENTRIES];
    if (!P.vbuf || !P.slots || !P.rows || !P.ibuf)
        return false;
    if (!white_init())
        log("pica: no memory for the white texture: a texture that can't be made leaves its unit as it was");
    bind_vertices();
    C3D_CullFace(GPU_CULL_NONE);
    aptHook(&s_apt_cookie, apt_hook, nullptr);
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
    if (!max_indices || count >= IMM_VERTICES || cull == 3)  // (3: all culled)
        return;
    begin_frame();
    if (stepping() && P.batch_count) {  // (NOMA_PICA_STEP: the draw before this one, alone)
        flush_batch();
        step_piece(STEP_DRAW);
    }
    // (a strip drawn in pieces builds two vertices more per piece: some room beyond count)
    const u32 room = count + 2 * (count / 4 + 1);
    if (P.vused + room > IMM_VERTICES || P.iused + max_indices > IBUF_INDICES) {
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
                    if (e.units[u].map < 0)
                        continue;
                    tex[u] = texture_for(e.units[u].map, &su.sx[u], &su.sy[u]);
                    if (!tex[u] && s_white.data) {
                        tex[u] = &s_white, su.sx[u] = su.sy[u] = 1.0f;
                        static u32 s_logged;
                        if (s_logged++ < 4)
                            log("pica: texture map %d of a draw couldn't be made: white instead", e.units[u].map);
                    }
                    su.texgen[u] = (s8)e.units[u].coord;
                }
            }
            C3D_TexEnv env[6];
            remap_luminance(e.env, tex, env);
            bool same = P.env_valid && memcmp(P.env, env, sizeof(P.env)) == 0 && P.buf_rgb == e.buf_rgb &&
                        P.buf_a == e.buf_a;
            for (int u = 0; u < 3 && same; u++)  // (units the combiners don't read don't matter)
                same = e.units[u].map < 0 ||
                       (tex[u] == P.tex[u] && (!tex[u] || memcmp(tex[u], &P.tex_was[u], sizeof(C3D_Tex)) == 0) &&
                        su.texgen[u] == P.setup.texgen[u] && su.sx[u] == P.setup.sx[u] && su.sy[u] == P.setup.sy[u]);
            if (!same) {
                flush_batch();
                NOMA_PROF_SCOPE("(gx bind)");
                Timed timed(Timers::BIND);
                P.setups++;
                for (int i = 0; i < 6; i++)
                    if (!P.env_valid || memcmp(&P.env[i], &env[i], sizeof(C3D_TexEnv)) != 0) {
                        P.env[i] = env[i];
                        st_texenv(i, &P.env[i]);
                    }
                if (!P.env_valid || P.buf_rgb != e.buf_rgb || P.buf_a != e.buf_a)
                    st_texenv_buf(e.buf_rgb, e.buf_a);
                P.buf_rgb = e.buf_rgb, P.buf_a = e.buf_a;
                bool bound = false;
                for (int u = 0; u < 3; u++) {
                    if (e.units[u].map >= 0 && tex[u] &&
                        (!P.env_valid || tex[u] != P.tex[u] || memcmp(tex[u], &P.tex_was[u], sizeof(C3D_Tex)) != 0))
                        st_tex(u, tex[u]), bound = true;
                    P.tex[u] = tex[u];
                    if (tex[u])
                        P.tex_was[u] = *tex[u];
                }
                if (bound)
                    st_tex_done();
                P.env_valid = true;
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
            pixel_state();
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

    // ---- a display list's draw: its vertices from the cache, or into it
    if (gx_in_display_list() && !P.cache_full) {
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
        // the draw's record: the one for this command with these registers and this recipe (a
        // list drawn with other registers in the same frame, a second pass, has a record of its
        // own); one whose arrays changed is made again in its own pool vertices, unless the GPU
        // still has to read them for a draw earlier this frame
        DlRecord* r = nullptr;
        DlRecord* oldest = nullptr;
        u32 variants = 0;
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
                u32 keep_first = x.first, keep_count = x.count, drawn = x.slots_frame;
                x = DlRecord();
                x.offset = offset;
                if (keep_count == count && drawn != P.frame)
                    x.first = keep_first, x.count = ~0u;  // (its vertices are made again in place below)
            }
            break;
        }
        u32 reuse_first = 0, reuse_count = 0;
        if (r && r->count == ~0u)
            reuse_first = r->first, reuse_count = count, r->count = 0;
        if (!r && variants >= 4) {
            // registers that keep changing (arrays written anew each frame): not worth keeping
            P.rebuilt_regs++;
            r = nullptr;
            goto immediate;
        }
        if (!r && variants)
            P.rebuilt_regs++;  // (a new pass of a list: counted once, when its record is made)
        if (r && r->count == count && r->slots_frame == P.frame)
            P.drawn_twice++;
        if (r && r->count == count && r->slots_frame != P.frame) {
            // the vertices are there: only the slots
            NOMA_PROF_SCOPE("(gx slots)");
            Timed timed(Timers::SLOTS);
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
                    submit(r->first + done, n);
                    done = next_piece(done, n);
                }
            } else {
                GxTnlSlots fs;
                const GxTnlSlots* hint = r->slots_frame ? &r->slots : nullptr;
                if (!gx_tnl_fixed_slots(plan, P.pal, hint, &fs)) {
                    flush_batch();
                    if (!gx_tnl_fixed_slots(plan, P.pal, hint, &fs))
                        return;
                }
                if (!r->slots_frame || memcmp(&fs, &r->slots, sizeof(fs)) != 0) {
                    T.slot_rewrites++, T.slot_vertices += count;
                    GxTnlSlots* dst = P.slots + r->first;
                    for (u32 i = 0; i < count; i++)
                        dst[i] = fs;
                    r->slots = fs;
                    slots_written(r->first, count);
                }
                submit(r->first, count);
            }
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
            if (!in_place && P.cache_next + count > POOL_VERTICES) {
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
                        r->ranges[i] = {ranges[i].attr, ranges[i].addr, ranges[i].size, mem_generation(ranges[i].addr, ranges[i].size)};
                        r->arrays[i][0] = cp[CP_ARRAY_BASE + ranges[i].attr], r->arrays[i][1] = cp[CP_ARRAY_STRIDE + ranges[i].attr];
                    }
                    dl_recipe(plan, r->recipe);
                    r->data_hash = dl_data_hash(data, count * plan.fmt->size), r->list_gen = e->gen;
                    if (!gx_tnl_rows_vary(plan))
                        r->slots = P.slots[first];
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
// as a BMP (the render target untiled by the GPU, then turned upright: it is 240x400, rotated)
static void save_shot(C3D_RenderTarget* t, u32 n) {
    const u32 W = 240, H = 400;
    u32* buf = (u32*)linearAlloc(W * H * 4);
    if (!buf)
        return;
    C3D_SyncDisplayTransfer((u32*)t->frameBuf.colorBuf, GX_BUFFER_DIM(W, H), buf, GX_BUFFER_DIM(W, H),
                            GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |
                                GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) |
                                GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA8) |
                                GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));
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
}

// ---------------------------------------------------------------- EFB copies to textures
namespace {

u32* s_readback;  // the render target after read_back(), untiled: 240x400, 0xRRGGBBAA, rotated

// let the GPU finish what was drawn so far and read the render target back; the frame then
// goes on drawing over it (its end is not shown: the output is detached meanwhile)
bool read_back() {
    NOMA_PROF_SCOPE("(gx readback)");
    Timed timed(Timers::READBACK);
    if (!s_readback && !(s_readback = (u32*)linearAlloc(240 * 400 * 4)))
        return false;
    bool in_frame = P.in_frame;
    if (in_frame) {
        C3D_RenderTargetDetachOutput(P.top);
        end_frame();
        P.all_dirty = true;  // (the new frame starts without state)
        P.env_valid = false;
        P.cull = ~0u;
    }
    // outside a frame this waits for the GPU's queue, then for the transfer
    GpuWait w("for a read-back (C3D_SyncDisplayTransfer)");
    C3D_SyncDisplayTransfer((u32*)P.top->frameBuf.colorBuf, GX_BUFFER_DIM(240, 400), s_readback,
                            GX_BUFFER_DIM(240, 400),
                            GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |
                                GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) |
                                GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA8) |
                                GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));
    GSPGPU_InvalidateDataCache(s_readback, 240 * 400 * 4);
    if (in_frame) {
        C3D_RenderTargetSetOutput(P.top, GFX_TOP, GFX_LEFT, DISPLAY_TRANSFER_FLAGS);
        C3D_FrameBegin(0);
        C3D_FrameDrawOn(P.top);
        frame_list_starts();
        P.c3d_touched = true;
        // the frame's immediate vertices and indices are reused from the start (the GPU
        // finished with them; the lists' vertices stay)
        P.vused = P.iused = 0;
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
    for (u32 v = 0; v < 1024; v++) {
        float fx = (float)((s32)v + efb_off_x) + 0.5f, fy = (float)((s32)v + efb_off_y) + 0.5f;
        u32 X = (u32)std::min(399.0f, std::max(0.0f, fx * 400.0f / FRAME_W));
        u32 Y = (u32)std::min(239.0f, std::max(0.0f, fy * 240.0f / FRAME_H));
        s_rb_col[v] = X * 240, s_rb_row[v] = 239 - Y;
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
    if (P.vused + 6 > IMM_VERTICES)
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
    float z = -(float)(bp[BP_CLEAR_Z] & 0xFFFFFF) / 16777215.0f;
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
        static int s_shot_every = getenv("NOMA_SHOT_EVERY") ? atoi(getenv("NOMA_SHOT_EVERY")) : 0;
        if (s_shot_every > 0 && P.frame % (u32)s_shot_every == 0)
            save_shot(P.top, P.frame);
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
            log("pica: %zu textures (%u KB), %u dropped for memory, %u not made; %u KB linear heap free, "
                "heap %u KB used of %u KB; at most %u immediate vertices and %u indices a frame; last frame %u draws "
                "in %u batches, %u state setups, %u draws (%u vertices) from the list cache, %u vertices built "
                "(%u list draws made again for their arrays, %u for registers, %u drawn twice, %u without room), "
                "cache at %u of %u (%u vertices given out; %u lists new, %u rewritten); the GPU %.2f ms drawing "
                "the last frame (%.2f ms for its command list)", P.textures.size(), kb, P.evicted, P.failed,
                (u32)(linearSpaceFree() >> 10), (u32)(mallinfo().uordblks >> 10), __ctru_heap_size >> 10, P.vmax,
                P.imax, P.draws, P.batches, P.setups, P.cached_draws, P.cached_vertices, P.built_vertices,
                P.rebuilt_arrays, P.rebuilt_regs, P.drawn_twice, P.no_room, P.cache_next - IMM_VERTICES,
                POOL_VERTICES - IMM_VERTICES, P.cache_made, P.lists_new, P.lists_rewritten,
                (double)C3D_GetDrawingTime(), (double)C3D_GetProcessingTime());
            P.vmax = P.imax = 0;
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
    C3D_RenderTargetDetachOutput(P.top);
    end_frame();
    C3D_RenderTargetSetOutput(P.top, GFX_TOP, GFX_LEFT, DISPLAY_TRANSFER_FLAGS);
    P.in_frame = false;
    P.resume = true;
}

void PicaRenderer::efb_copy(u32 value) {
    if (!P.ready)
        return;
    EfbCopy c = gx_efb_copy_params(value);
    if (c.to_xfb)  // the frame is on screen already; the next one starts with a clear
        return;
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

}  // namespace guest
