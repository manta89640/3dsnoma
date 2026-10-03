// The GX renderer on the 3DS GPU (PICA200, through citro3d).
//
// The emulated transform unit (runtime/hw/gx/xform.cpp) turns each GX vertex into clip space,
// lit colors and texture coordinates on the CPU; the PICA rasterizes them. Per draw, the GX
// pixel pipeline state becomes PICA state:
//   * the GX viewport (in the 640x456 frame) folded into one matrix with the scale to the
//     400x240 top screen, the rotated render target and the GX depth range (the vertex shader
//     applies it to the clip-space positions);
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
#include "runtime/hw/gx/vertex.h"
#include "runtime/runtime.h"
#include "gx_pica.h"

extern "C" u32 __ctru_heap_size;
extern "C" const uint8_t gx_shbin[];
extern "C" const size_t gx_shbin_size;

namespace guest {

namespace {

const float FRAME_W = 640.0f, FRAME_H = 456.0f;  // the game's frame buffer
const u32 VBUF_BYTES = 6 << 20;

#define DISPLAY_TRANSFER_FLAGS                                                                  \
    (GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |           \
     GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | \
     GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

struct PVertex {
    float pos[4];
    u8 color[4];
    float tc[3][2];
};

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
    int u_xform = -1;
    PVertex* vbuf = nullptr;
    u32 vused = 0;  // vertices used this frame
    u32 vmax = 0;   // ... at most, since the last report
    bool in_frame = false;
    u32 frame = 0;
    std::unordered_map<u32, TexEntry> textures;  // by GxTexture id
    std::vector<C3D_Tex> dead;                   // freed after the GPU finished the frame
    u32 warned = 0;
    u32 evicted = 0, failed = 0;                 // textures dropped for memory; not made
};
State P;

// ---------------------------------------------------------------- textures
u32 next_pow2(u32 v) {
    u32 p = 8;
    while (p < v && p < 1024)
        p <<= 1;
    return p;
}

// PICA textures are 8x8 tiles in Z order; texel (x, y) of a w-wide texture
u32 tiled_offset(u32 x, u32 y, u32 w) {
    u32 tile = (y >> 3) * (w >> 3) + (x >> 3);
    u32 m = (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3);
    return tile * 64 + m;
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

// texture memory, from the linear heap: what the sound cache may still take and a little for
// screenshots and read-backs stay free; older textures make room
bool tex_init(C3D_Tex* t, u32 w, u32 h, GPU_TEXCOLOR f) {
    const u32 MARGIN = 512 << 10;
    u32 need = w * h * texel_bytes(f);
    for (;;) {
        if (linearSpaceFree() >= need + audio_dsp_linear_reserve() + MARGIN && C3D_TexInit(t, w, h, f))
            return true;
        if (!evict_texture())
            return C3D_TexInit(t, w, h, f);  // nothing left to drop: whatever fits
    }
}

// the PICA texture for texture map `map`; scale: what to multiply its coordinates with
C3D_Tex* texture_for(u32 map, float* sx, float* sy) {
    const GxTexture* gt = gx_texture(map);
    if (!gt || gt->rgba.empty())
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
        u32 tb = texel_bytes(fmt);
        u8* out = (u8*)e.tex.data;
        // repeating textures are resampled to the power-of-two size, the others padded
        for (u32 y = 0; y < H; y++) {
            for (u32 x = 0; x < W; x++) {
                u32 sx_ = x, sy_ = y;
                if (repeat) {
                    sx_ = x * gt->width / W;
                    sy_ = y * gt->height / H;
                } else {
                    sx_ = std::min(x, gt->width - 1);
                    sy_ = std::min(y, gt->height - 1);
                }
                u32 c = gt->rgba[sy_ * gt->width + sx_];  // R G B A (bytes 0-3)
                u32 r = c & 0xFF, g = (c >> 8) & 0xFF, b = (c >> 16) & 0xFF, a = c >> 24;
                // The PICA's t runs from the last row up (t = 0 is row H-1), GX's from the first
                // down: the image is stored upside down (padding then sits above it, at
                // t > height/H, where coordinates don't reach). Texels are little-endian:
                // RGBA8 0xRRGGBBAA, LA8 0xLLAA, LA4 0xLA, RGB565, RGBA5551 (R in the top bits).
                u8* t = out + tiled_offset(x, H - 1 - y, W) * tb;
                switch (fmt) {
                case GPU_LA4: t[0] = (u8)((r & 0xF0) | a >> 4); break;
                case GPU_LA8: t[0] = (u8)a, t[1] = (u8)r; break;
                case GPU_RGB565: {
                    u16 v = (u16)((r >> 3) << 11 | (g >> 2) << 5 | b >> 3);
                    t[0] = (u8)v, t[1] = (u8)(v >> 8);
                    break;
                }
                case GPU_RGBA5551: {
                    u16 v = (u16)((r >> 3) << 11 | (g >> 3) << 6 | (b >> 3) << 1 | (a >> 7));
                    t[0] = (u8)v, t[1] = (u8)(v >> 8);
                    break;
                }
                default: {
                    u32 v = r << 24 | g << 16 | b << 8 | a;
                    memcpy(t, &v, 4);
                    break;
                }
                }
            }
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

bool is_zero(const In& i) { return i.kind == In::ZERO || (i.kind == In::CONST && i.value == 0); }
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
    if (is_zero(c)) {
        shape = is_zero(a) ? NONE : SINGLE, x = a;
    } else if (is_one(c, alpha)) {
        shape = is_zero(b) ? NONE : SINGLE, x = b;
    } else if (is_zero(a)) {
        if (is_zero(b))
            shape = NONE;
        else if (is_one(b, alpha))
            shape = SINGLE, x = c;
        else
            shape = PRODUCT, p0 = b, p1 = c;
    } else if (is_zero(b)) {
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
    bool has_d = !is_zero(d);
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
        u32 v = in.kind == In::ONE ? 0xFFFFFFFF : in.kind == In::ZERO ? 0 : in.value;
        u32 mask = alpha ? 0xFF000000u : 0x00FFFFFFu;
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

// Set up the PICA texture combiners for the current TEV configuration; fills units[] with
// the (map, coord) pair each texture unit samples.
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
            C3D_TexEnv* env = C3D_GetTexEnv(b.stage + k);
            C3D_TexEnvInit(env);
            u32 konst = 0;
            bool have = false;
            // the half that is shorter passes its result on
            Half c = k < nc ? ch[k] : Half(), a = k < na ? ah[k] : Half();
            if (k >= nc) {
                c.func = GPU_REPLACE;
                c.s[0].kind = In::SRC, c.s[0].src = GPU_PREVIOUS, c.s[0].op = GPU_TEVOP_RGB_SRC_COLOR;
            }
            if (k >= na) {
                a.func = GPU_REPLACE;
                a.s[0].kind = In::SRC, a.s[0].src = GPU_PREVIOUS, a.s[0].op = GPU_TEVOP_A_SRC_ALPHA;
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
            if (!merge_consts(c, false, &konst, &have_bits) || !merge_consts(a, true, &konst, &have_bits))
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
        C3D_TexEnv* env = C3D_GetTexEnv(s);
        C3D_TexEnvInit(env);
        C3D_TexEnvSrc(env, C3D_Both, GPU_PREVIOUS, GPU_PREVIOUS, GPU_PREVIOUS);
        C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
    }
    C3D_TexEnvBufUpdate(C3D_RGB, b.buf_rgb_mask);
    C3D_TexEnvBufUpdate(C3D_Alpha, b.buf_a_mask);
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
// for the 240x400 render target (citro3d's Mtx_OrthoTilt convention), GX depth range
void set_xform() {
    float vsx = xf_f32(XF_VIEWPORT + 0), vsy = xf_f32(XF_VIEWPORT + 1), vsz = xf_f32(XF_VIEWPORT + 2);
    float vox = xf_f32(XF_VIEWPORT + 3), voy = xf_f32(XF_VIEWPORT + 4), voz = xf_f32(XF_VIEWPORT + 5);
    float kx = 400.0f / FRAME_W, ky = 240.0f / FRAME_H;
    C3D_Mtx m;
    Mtx_Zeros(&m);
    // x' = 1 - Y/120, Y = (y/w * vsy + voy - 342) * ky
    m.r[0].y = -vsy * ky / 120.0f;
    m.r[0].w = 1.0f - (voy - 342.0f) * ky / 120.0f;
    // y' = 1 - X/200, X = (x/w * vsx + vox - 342) * kx
    m.r[1].x = -vsx * kx / 200.0f;
    m.r[1].w = 1.0f - (vox - 342.0f) * kx / 200.0f;
    // the PICA keeps depth -z'/w: the GX window depth, 0-1
    m.r[2].z = -vsz / 16777215.0f;
    m.r[2].w = -voz / 16777215.0f;
    m.r[3].w = 1.0f;
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, P.u_xform, &m);

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

bool culled(const GxOutVertex& v0, const GxOutVertex& v1, const GxOutVertex& v2, u32 cull) {
    if (!cull)
        return false;
    const f32 *p0 = v0.clip, *p1 = v1.clip, *p2 = v2.clip;
    f32 nz = (p0[0] * p2[3] - p2[0] * p0[3]) * p1[1] + (p2[0] * p0[1] - p0[0] * p2[1]) * p1[3] +
             (p2[1] * p0[3] - p0[1] * p2[3]) * p1[0];
    bool back = nz <= 0;
    if (xf_f32(XF_VIEWPORT + 1) > 0)
        back = !back;
    if (xf_f32(XF_VIEWPORT + 0) < 0)
        back = !back;
    return back ? (cull == 2 || cull == 3) : (cull == 1 || cull == 3);
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
    collect_textures();
    const u32* bp = g_gx.bp;
    u32 ar = bp[BP_CLEAR_AR], gb = bp[BP_CLEAR_GB];
    u32 color = (ar & 255) << 24 | ((gb >> 8) & 255) << 16 | (gb & 255) << 8 | 0xFF;
    C3D_RenderTargetClear(P.top, C3D_CLEAR_ALL, color, bp[BP_CLEAR_Z] & 0xFFFFFF);
    C3D_FrameDrawOn(P.top);
    P.vmax = std::max(P.vmax, P.vused);
    P.vused = 0;
    P.in_frame = true;
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
    P.u_xform = shaderInstanceGetUniformLocation(P.prog.vertexShader, "xform");
    C3D_AttrInfo* attr = C3D_GetAttrInfo();
    AttrInfo_Init(attr);
    AttrInfo_AddLoader(attr, 0, GPU_FLOAT, 4);          // position (clip space)
    AttrInfo_AddLoader(attr, 1, GPU_UNSIGNED_BYTE, 4);  // color
    AttrInfo_AddLoader(attr, 2, GPU_FLOAT, 2);          // texture coordinates 0-2
    AttrInfo_AddLoader(attr, 3, GPU_FLOAT, 2);
    AttrInfo_AddLoader(attr, 4, GPU_FLOAT, 2);
    P.vbuf = (PVertex*)linearAlloc(VBUF_BYTES);
    if (!P.vbuf)
        return false;
    C3D_BufInfo* bi = C3D_GetBufInfo();
    BufInfo_Init(bi);
    BufInfo_Add(bi, P.vbuf, sizeof(PVertex), 5, 0x43210);
    C3D_CullFace(GPU_CULL_NONE);
    P.ready = true;
    log("pica: renderer ready");
    return true;
}

void PicaRenderer::draw(u32 prim, u32 vat, u32 count, const u8* data) {
    if (!P.ready || !count)
        return;
    static std::vector<GxVertex> in;
    static std::vector<GxOutVertex> out;
    in.resize(count);
    out.resize(count);
    {
        NOMA_PROF_SCOPE("(gx vertices)");
        gx_decode_vertices(vat, count, data, in.data());
        gx_transform(in.data(), count, out.data());
    }
    begin_frame();

    // triangles (quads, strips and fans expanded; lines and points not drawn yet)
    static std::vector<u32> idx;
    idx.clear();
    u32 cull = bits(g_gx.bp[BP_GENMODE], 14, 2);
    auto tri = [&](u32 a, u32 b, u32 c) {
        if (!culled(out[a], out[b], out[c], cull))
            idx.push_back(a), idx.push_back(b), idx.push_back(c);
    };
    switch (prim) {
    case 0x80:
    case 0x88:
        for (u32 i = 0; i + 3 < count; i += 4)
            tri(i, i + 1, i + 2), tri(i, i + 2, i + 3);
        break;
    case 0x90:
        for (u32 i = 0; i + 2 < count; i += 3)
            tri(i, i + 1, i + 2);
        break;
    case 0x98:
        for (u32 i = 0; i + 2 < count; i++) {
            if (i & 1)
                tri(i + 1, i, i + 2);
            else
                tri(i, i + 1, i + 2);
        }
        break;
    case 0xA0:
        for (u32 i = 1; i + 1 < count; i++)
            tri(0, i, i + 1);
        break;
    default:
        return;
    }
    if (idx.empty())
        return;
    if (P.vused + idx.size() > VBUF_BYTES / sizeof(PVertex)) {
        if (P.warned++ < 4)
            log("pica: vertex buffer full");
        return;
    }

    // state
    NOMA_PROF_SCOPE("(gx state)");
    Builder b;
    tev_to_texenv(b);
    float tsx[3] = {1, 1, 1}, tsy[3] = {1, 1, 1};
    int coord[3] = {0, 0, 0};
    int units = 0;
    for (int u = 0; u < 3; u++) {
        if (b.units[u].map < 0)
            continue;
        C3D_Tex* t = texture_for(b.units[u].map, &tsx[u], &tsy[u]);
        if (t) {
            C3D_TexBind(u, t);
            units |= 1 << u;
        } else {
            C3D_TexBind(u, nullptr);
        }
        coord[u] = b.units[u].coord;
    }
    pixel_state();
    set_xform();

    NOMA_PROF_SCOPE("(gx submit)");
    PVertex* v = P.vbuf + P.vused;
    for (u32 i = 0; i < idx.size(); i++) {
        const GxOutVertex& o = out[idx[i]];
        PVertex& pv = v[i];
        memcpy(pv.pos, o.clip, sizeof(pv.pos));
        for (int k = 0; k < 4; k++) {
            float c = o.color[0][k];
            pv.color[k] = (u8)(c < 0 ? 0 : c > 255 ? 255 : c);
        }
        for (int u = 0; u < 3; u++) {
            const f32* t = o.tex[coord[u]];
            float q = t[2] != 0 ? t[2] : 1.0f;
            pv.tc[u][0] = t[0] / q * tsx[u];
            pv.tc[u][1] = t[1] / q * tsy[u];
        }
    }
    C3D_DrawArrays(GPU_TRIANGLES, P.vused, idx.size());
    P.vused += idx.size();
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
    if (!s_readback && !(s_readback = (u32*)linearAlloc(240 * 400 * 4)))
        return false;
    bool in_frame = P.in_frame;
    if (in_frame) {
        C3D_RenderTargetDetachOutput(P.top);
        C3D_FrameEnd(0);
    }
    // outside a frame this waits for the GPU's queue, then for the transfer
    C3D_SyncDisplayTransfer((u32*)P.top->frameBuf.colorBuf, GX_BUFFER_DIM(240, 400), s_readback,
                            GX_BUFFER_DIM(240, 400),
                            GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |
                                GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) |
                                GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA8) |
                                GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));
    if (in_frame) {
        C3D_RenderTargetSetOutput(P.top, GFX_TOP, GFX_LEFT, DISPLAY_TRANSFER_FLAGS);
        C3D_FrameBegin(0);
        C3D_FrameDrawOn(P.top);
    }
    return true;
}

// The render target holds the frame where it goes on screen. A game that draws its frame in
// bands (this one: two passes through an anti-aliased EFB) moves each band to the top of the
// EFB with the scissor box offset; EFB coordinates are screen coordinates minus that offset.
s32 efb_off_x, efb_off_y;
void load_efb_offset() {
    u32 r = g_gx.bp[BP_SCISSOR_OFFSET];
    efb_off_x = (s32)bits(r, 0, 10) * 2 - 342, efb_off_y = (s32)bits(r, 10, 10) * 2 - 342;
}

// an EFB pixel from the read-back render target (the frame scaled to 400x240, rotated)
void readback_rgba(u32 x, u32 y, s32* out) {
    float fx = (float)((s32)x + efb_off_x) + 0.5f, fy = (float)((s32)y + efb_off_y) + 0.5f;
    u32 X = (u32)std::min(399.0f, std::max(0.0f, fx * 400.0f / FRAME_W));
    u32 Y = (u32)std::min(239.0f, std::max(0.0f, fy * 240.0f / FRAME_H));
    u32 p = s_readback[X * 240 + (239 - Y)];
    out[0] = (s32)(p >> 24), out[1] = (s32)((p >> 16) & 255), out[2] = (s32)((p >> 8) & 255), out[3] = (s32)(p & 255);
}

// the clear that goes with a copy: a rectangle in the clear color and depth (the copy's
// source; color, alpha and depth as their update flags allow)
void clear_rect(u32 sx, u32 sy, u32 w, u32 h) {
    if (P.vused + 6 > VBUF_BYTES / sizeof(PVertex))
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
    C3D_Mtx id;
    Mtx_Identity(&id);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, P.u_xform, &id);
    // corners straight in the PICA's clip space (as set_xform maps them)
    float z = -(float)(bp[BP_CLEAR_Z] & 0xFFFFFF) / 16777215.0f;
    auto clip = [&](u32 ex, u32 ey, PVertex& v) {
        float X = ((s32)ex + efb_off_x) * 400.0f / FRAME_W, Y = ((s32)ey + efb_off_y) * 240.0f / FRAME_H;
        v.pos[0] = 1.0f - Y / 120.0f, v.pos[1] = 1.0f - X / 200.0f, v.pos[2] = z, v.pos[3] = 1.0f;
        v.color[0] = (u8)ar, v.color[1] = (u8)(gb >> 8), v.color[2] = (u8)gb, v.color[3] = (u8)(ar >> 8);
        memset(v.tc, 0, sizeof(v.tc));
    };
    PVertex* v = P.vbuf + P.vused;
    clip(sx, sy, v[0]), clip(sx + w, sy, v[1]), clip(sx + w, sy + h, v[2]);
    v[3] = v[0], v[4] = v[2], clip(sx, sy + h, v[5]);
    C3D_DrawArrays(GPU_TRIANGLES, P.vused, 6);
    P.vused += 6;
}

}  // namespace

void PicaRenderer::bp_write(u32 reg, u32 value) {
    if (reg == 0x45 && (value & 0xFF) == 2 && P.in_frame) {  // draw done: the frame is complete
        if (gx_efb_copy_test_frame() == (int)P.frame && read_back()) {
            efb_off_x = efb_off_y = 0;  // the whole frame
            gx_efb_copy_test(readback_rgba, "sdmc:/3ds/noma/efbcopy_3ds");
        }
        {
            NOMA_PROF_SCOPE("(gx wait)");
            C3D_FrameEnd(0);
        }
        P.in_frame = false;
        P.frame++;
        static int s_shot_every = getenv("NOMA_SHOT_EVERY") ? atoi(getenv("NOMA_SHOT_EVERY")) : 0;
        if (s_shot_every > 0 && P.frame % (u32)s_shot_every == 0)
            save_shot(P.top, P.frame);
        if (s_unsupported && P.frame % 600 == 1)
            log("pica: %u TEV configurations approximated", s_unsupported);
        if (P.frame % 600 == 1) {
            u32 kb = 0;
            for (auto& kv : P.textures)
                kb += kv.second.tex.size >> 10;
            log("pica: %zu textures (%u KB), %u dropped for memory, %u not made; %u KB linear heap free, "
                "heap %u KB used of %u KB; at most %u vertices a frame", P.textures.size(), kb, P.evicted,
                P.failed, (u32)(linearSpaceFree() >> 10), (u32)(mallinfo().uordblks >> 10),
                __ctru_heap_size >> 10, P.vmax);
            P.vmax = 0;
        }
    }
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
        gx_copy_to_texture(c, readback_rgba);
    }
    if (c.clear && P.in_frame)
        clear_rect(c.sx, c.sy, c.w, c.h);
}

}  // namespace guest
