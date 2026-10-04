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

extern "C" u32 __ctru_heap_size;
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
    snprintf(line + n, sizeof(line) - n, "; palette %u fills %u checks (%u stale) %u searches; %u list vertices made; "
             "cached draws: %u with matrices per vertex, %u fixed with slots rewritten (%u vertices)",
             (ps.fills - T.pal0.fills) / T.frames, (ps.checks - T.pal0.checks) / T.frames,
             (ps.stale - T.pal0.stale) / T.frames, (ps.scans - T.pal0.scans) / T.frames, T.cache_made / T.frames,
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

void draw_elements(u32 count, const u16* indices) {
    upload_uniforms();
    if (P.c3d_touched) {
        C3D_DrawElements(GPU_TRIANGLES, count, C3D_UNSIGNED_SHORT, indices);
        P.c3d_touched = false;
        return;
    }
    // (as C3D_DrawElements writes them for GPU_TRIANGLES)
    u32 pa = osConvertVirtToPhys(indices);
    GPUCMD_AddMaskedWrite(GPUREG_PRIMITIVE_CONFIG, 2, GPU_GEOMETRY_PRIM);
    GPUCMD_AddWrite(GPUREG_RESTART_PRIMITIVE, 1);
    GPUCMD_AddWrite(GPUREG_INDEXBUFFER_CONFIG, (pa - 0x18000000) | (1u << 31));
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

// an applet had the GPU: citro3d binds its own state again, we send our uniforms again
void apt_hook(APT_HookType type, void*) {
    if (type == APTHOOK_ONRESTORE)
        P.uni_resend = true, P.c3d_touched = true;
}
aptHookCookie s_apt_cookie;

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
void dump_batch(u32 nverts, const PVertex* verts, const GxTnlSlots* slots, u32 nidx, const u16* idx, u32 base) {
    if (s_dump_frame == -2)
        s_dump_frame = getenv("NOMA_DUMP_BATCHES") ? atoi(getenv("NOMA_DUMP_BATCHES")) : -1;
    if ((int)P.frame != s_dump_frame)
        return;
    if (!s_dump && !(s_dump = fopen("sdmc:/3ds/noma/batches.bin", "wb")))
        return;
    u32 hdr[8] = {0x42415443, nverts, nidx, P.consts.bools, P.consts.nlights, P.cull, base, 0};
    fwrite(hdr, 4, 8, s_dump);
    for (int i = 0; i < 96; i++) {  // (our copy is in the GPU's order, w first)
        u32 v[4] = {P.uni[i][3], P.uni[i][2], P.uni[i][1], P.uni[i][0]};
        fwrite(v, 4, 4, s_dump);
    }
    for (u32 i = 0; i < nverts; i++) {  // (the vertex with its slots, as tools/pica_replay reads them)
        fwrite(&verts[i], sizeof(PVertex), 1, s_dump);
        fwrite(&slots[i], sizeof(GxTnlSlots), 1, s_dump);
    }
    fwrite(idx, 2, nidx, s_dump);
}

// the draws with the current state so far, as one; the palette's slots are free again after
void flush_batch() {
    if (P.batch_count) {
        NOMA_PROF_SCOPE("(gx flush)");
        Timed timed(Timers::FLUSH);
        upload_palette();
        if ((int)P.frame == s_dump_frame || s_dump_frame == -2) {
            // the vertices the indices reach (one segment: indices are relative to its start)
            u32 lo = ~0u, hi = 0;
            for (u32 i = 0; i < P.batch_count; i++)
                lo = std::min<u32>(lo, P.ibuf[P.batch_first + i]), hi = std::max<u32>(hi, P.ibuf[P.batch_first + i]);
            if (lo <= hi)
                dump_batch(hi - lo + 1, P.vbuf + lo, P.slots + lo, P.batch_count, P.ibuf + P.batch_first, lo);
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
    BufInfo_Add(bi, P.vbuf, sizeof(PVertex), 6, 0x654210);
    BufInfo_Add(bi, P.slots, sizeof(GxTnlSlots), 1, 0x3);
}

inline void slots_written(u32 first, u32 n) {
    P.slots_lo = std::min(P.slots_lo, first), P.slots_hi = std::max(P.slots_hi, first + n);
}

// what the CPU wrote this frame goes to memory for the GPU (citro3d's C3D_FrameEnd(0) would
// flush the whole linear heap: the guest's memory is in it)
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
    flush_geometry();
    C3D_FrameEnd(GX_CMDLIST_FLUSH);
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
    case 0:  // I4
    case 2:  // IA4
        return GPU_LA4;
    case 1:  // I8
    case 3:  // IA8
        return GPU_LA8;
    case 4: return GPU_RGB565;
    case 14: return GPU_RGBA5551;  // CMPR
    default: return GPU_RGBA8;
    }
}

u32 texel_bytes(GPU_TEXCOLOR f) {
    switch (f) {
    case GPU_LA4: return 1;
    case GPU_LA8:
    case GPU_RGB565:
    case GPU_RGBA5551: return 2;
    default: return 4;
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
    C3D_TexDelete(&victim->second.tex);
    P.textures.erase(victim);
    P.evicted++;
    return true;
}

// texture memory, from the linear heap: a little stays free for screenshots and read-backs;
// older textures make room
bool tex_init(C3D_Tex* t, u32 w, u32 h, GPU_TEXCOLOR f) {
    const u32 MARGIN = 512 << 10;
    u32 need = w * h * texel_bytes(f);
    for (;;) {
        if (linearSpaceFree() >= need + MARGIN && C3D_TexInit(t, w, h, f))
            return true;
        if (!evict_texture())
            return C3D_TexInit(t, w, h, f);  // nothing left to drop: whatever fits
    }
}

// the PICA format as pica_tex.h names it
pica_tex::Fmt tex_fmt(GPU_TEXCOLOR f) {
    switch (f) {
    case GPU_LA4: return pica_tex::LA4;
    case GPU_LA8: return pica_tex::LA8;
    case GPU_RGB565: return pica_tex::RGB565;
    case GPU_RGBA5551: return pica_tex::RGBA5551;
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
    GPU_TEXCOLOR fmt = pica_format(gt);
    if (e.version != gt->version || !e.tex.data) {
        NOMA_PROF_SCOPE("(gx texture upload)");
        Timed timed(Timers::UPLOAD);
        T.uploads++, T.upload_texels += W * H;
        if (!raw)
            return nullptr;
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

void collect_textures() {
    for (C3D_Tex& t : P.dead)
        C3D_TexDelete(&t);
    P.dead.clear();
    for (auto it = P.textures.begin(); it != P.textures.end();) {
        if (P.frame - it->second.last_used > 120) {
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
    C3D_DepthTest(true, ztest ? test_func(bits(zmode, 1, 3)) : GPU_ALWAYS, (GPU_WRITEMASK)mask);

    if (cm & 1) {
        if (cm & 0x800)
            C3D_AlphaBlend(GPU_BLEND_REVERSE_SUBTRACT, GPU_BLEND_REVERSE_SUBTRACT, GPU_ONE, GPU_ONE, GPU_ONE,
                           GPU_ONE);
        else {
            GPU_BLENDFACTOR s = blend_factor(bits(cm, 8, 3), true), d = blend_factor(bits(cm, 5, 3), false);
            C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, s, d, s, d);
        }
    } else if (cm & 2) {
        static const GPU_LOGICOP ops[16] = {GPU_LOGICOP_CLEAR, GPU_LOGICOP_AND, GPU_LOGICOP_AND_REVERSE,
                                           GPU_LOGICOP_COPY, GPU_LOGICOP_AND_INVERTED, GPU_LOGICOP_NOOP,
                                           GPU_LOGICOP_XOR, GPU_LOGICOP_OR, GPU_LOGICOP_NOR,
                                           GPU_LOGICOP_EQUIV, GPU_LOGICOP_INVERT, GPU_LOGICOP_OR_REVERSE,
                                           GPU_LOGICOP_COPY_INVERTED, GPU_LOGICOP_OR_INVERTED,
                                           GPU_LOGICOP_NAND, GPU_LOGICOP_SET};
        C3D_ColorLogicOp(ops[bits(cm, 12, 4)]);
    } else {
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
    }

    // alpha test: the PICA has one comparison; keep the one that isn't "always"
    u32 ac = bp[BP_ALPHACOMPARE];
    u32 c0 = bits(ac, 16, 3), c1 = bits(ac, 19, 3), op = bits(ac, 22, 2);
    u32 r0 = bits(ac, 0, 8), r1 = bits(ac, 8, 8);
    if (c0 == 7 && c1 == 7)
        C3D_AlphaTest(false, GPU_ALWAYS, 0);
    else if (op == 0 && c1 == 7)
        C3D_AlphaTest(true, test_func(c0), r0);
    else if (op == 0 && c0 == 7)
        C3D_AlphaTest(true, test_func(c1), r1);
    else if (op == 1 && c1 == 0)
        C3D_AlphaTest(true, test_func(c0), r0);
    else if (op == 1 && c0 == 0)
        C3D_AlphaTest(true, test_func(c1), r1);
    else
        C3D_AlphaTest(true, test_func(c0), r0);
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
    C3D_SetScissor(GPU_SCISSOR_NORMAL, (u32)(240 - Y1), (u32)(400 - X1), (u32)std::ceil(240 - Y0),
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
    P.c3d_touched = true;
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
    P.dl = new DlEntry[DL_ENTRIES];
    if (!P.vbuf || !P.slots || !P.rows || !P.ibuf)
        return false;
    bind_vertices();
    C3D_CullFace(GPU_CULL_NONE);
    aptHook(&s_apt_cookie, apt_hook, nullptr);
    P.ready = true;
    log("pica: renderer ready");
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
                    su.texgen[u] = (s8)e.units[u].coord;
                }
            }
            bool same = P.env_valid && memcmp(P.env, e.env, sizeof(P.env)) == 0 && P.buf_rgb == e.buf_rgb &&
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
                P.c3d_touched = true;
                for (int i = 0; i < 6; i++)
                    if (!P.env_valid || memcmp(&P.env[i], &e.env[i], sizeof(C3D_TexEnv)) != 0) {
                        P.env[i] = e.env[i];
                        C3D_SetTexEnv(i, &P.env[i]);
                    }
                if (!P.env_valid || P.buf_rgb != e.buf_rgb)
                    C3D_TexEnvBufUpdate(C3D_RGB, e.buf_rgb);
                if (!P.env_valid || P.buf_a != e.buf_a)
                    C3D_TexEnvBufUpdate(C3D_Alpha, e.buf_a);
                P.buf_rgb = e.buf_rgb, P.buf_a = e.buf_a;
                for (int u = 0; u < 3; u++) {
                    if (e.units[u].map >= 0 &&
                        (!P.env_valid || tex[u] != P.tex[u] ||
                         (tex[u] && memcmp(tex[u], &P.tex_was[u], sizeof(C3D_Tex)) != 0)))
                        C3D_TexBind(u, tex[u]);
                    P.tex[u] = tex[u];
                    if (tex[u])
                        P.tex_was[u] = *tex[u];
                }
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
            P.c3d_touched = true;
        }
        if (d & GX_BP_SCISSOR) {
            flush_batch();
            set_xform();
            P.viewport_serial = g_gx.xf_viewport_serial;
            P.c3d_touched = true;
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
        C3D_CullFace(k_cull[cull]);
        P.cull = cull;
        P.c3d_touched = true;
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
    }
    // outside a frame this waits for the GPU's queue, then for the transfer
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
        {
            NOMA_PROF_SCOPE("(gx end)");
            Timed timed(Timers::END);
            end_frame();
        }
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
            u32 kb = 0;
            for (auto& kv : P.textures)
                kb += kv.second.tex.size >> 10;
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
        gx_copy_to_texture(c, readback_rgba);
    }
    if (c.clear && P.resume)
        begin_frame();
    if (c.clear && P.in_frame)
        clear_rect(c.sx, c.sy, c.w, c.h);
}

}  // namespace guest
