// Replay a frame's draw calls as the 3DS renderer gave them to the GPU (NOMA_DUMP_BATCHES=N writes
// sdmc:/3ds/noma/batches.bin) through the PICA vertex shader interpreter: each batch's vertices
// go through the shader with the batch's uniforms, the triangles are drawn flat into a 400x240
// picture, and vertices that land far off the screen are listed with their palette slots and the
// rows those slots held.
//
//   pica_replay batches.bin gx.shbin out.png
//
// Build (host): clang++ -O2 -std=c++17 -I. -Iruntime tools/pica_replay.cpp runtime/hw/gx/pica_vsh.cpp
//               platform/host/png.cpp -o pica_replay
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "runtime/hw/gx/pica_vsh.h"

bool write_png(const std::string& path, const u32* rgba, u32 w, u32 h);

namespace guest {
void log(const char* fmt, ...) {}  // (pica_vsh.cpp doesn't log; hw.h may declare it)
}

using namespace guest;

struct Vertex {  // GxTnlVertex followed by its GxTnlSlots, as the dump writes them
    f32 pos[3], nrm[3];
    u8 color[4];
    f32 tc[3][3];
    u8 slot[4];
};
static_assert(sizeof(Vertex) == 68, "layout");

int main(int argc, char** argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: pica_replay batches.bin gx.shbin out.png\n");
        return 1;
    }
    FILE* f = fopen(argv[2], "rb");
    if (!f)
        return 1;
    std::vector<u8> sh;
    fseek(f, 0, SEEK_END);
    sh.resize(ftell(f));
    fseek(f, 0, SEEK_SET);
    fread(sh.data(), 1, sh.size(), f);
    fclose(f);
    PicaShader* shader = pica_shader_load(sh.data(), sh.size());
    if (!shader) {
        fprintf(stderr, "bad shader\n");
        return 1;
    }
    int o_pos = pica_shader_output(shader, 0), o_clr = pica_shader_output(shader, 2);
    f = fopen(argv[1], "rb");
    if (!f)
        return 1;
    const u32 W = 400, H = 240;
    std::vector<u32> img(W * H, 0xFF202020);
    std::vector<f32> depth(W * H, 1e9f);
    u32 nb = 0, nsuspect = 0, ntri = 0;
    PicaUniforms u;
    memset(&u, 0, sizeof(u));
    pica_shader_constants(shader, u);
    for (;;) {
        u32 hdr[8];
        if (fread(hdr, 4, 8, f) != 8)
            break;
        if (hdr[0] != 0x42415443) {
            fprintf(stderr, "bad batch header at batch %u\n", nb);
            break;
        }
        u32 nverts = hdr[1], nidx = hdr[2], bools = hdr[3], nlights = hdr[4], cull = hdr[5], base = hdr[6];
        f32 uni[96][4];
        fread(uni, 16, 96, f);
        std::vector<Vertex> verts(nverts);
        std::vector<u16> idx(nidx);
        fread(verts.data(), sizeof(Vertex), nverts, f);
        fread(idx.data(), 2, nidx, f);
        memcpy(u.f, uni, sizeof(uni));
        pica_shader_constants(shader, u);  // (the DVLE's constants: citro3d sends them apart from C3D_FVUnif)
        u.b = bools;
        u.i[0][0] = nlights ? (s32)nlights - 1 : 0, u.i[0][1] = 0, u.i[0][2] = 4, u.i[0][3] = 0;
        // the vertices through the shader
        struct Out {
            f32 c[4];  // clip
            f32 sx, sy, w, z;
            u32 color;
            bool bad;
        };
        std::vector<Out> out(nverts);
        u32 bad_here = 0;
        for (u32 i = 0; i < nverts; i++) {
            const Vertex& v = verts[i];
            f32 in[16][4], o[16][4];
            memset(in, 0, sizeof(in));
            memset(o, 0, sizeof(o));
            for (int k = 0; k < 3; k++)
                in[0][k] = v.pos[k], in[1][k] = v.nrm[k], in[4][k] = v.tc[0][k], in[5][k] = v.tc[1][k], in[6][k] = v.tc[2][k];
            in[0][3] = in[1][3] = in[4][3] = in[5][3] = in[6][3] = 1.0f;
            for (int k = 0; k < 4; k++)
                in[2][k] = v.color[k], in[3][k] = v.slot[k];
            pica_vsh_run(shader, u, in, o);
            f32 x = o[o_pos][0], y = o[o_pos][1], z = o[o_pos][2], w = o[o_pos][3];
            Out& r = out[i];
            r.c[0] = x, r.c[1] = y, r.c[2] = z, r.c[3] = w;
            r.w = w;
            r.sx = (1.0f - y / w) * 200.0f;  // (set_xform: y' = 1 - X/200)
            r.sy = (1.0f - x / w) * 120.0f;  // x' = 1 - Y/120
            r.z = z / w;
            auto c8 = [](f32 c) { return (u32)(c < 0 ? 0 : c > 1 ? 255 : c * 255); };
            r.color = 0xFF000000 | c8(o[o_clr][2]) << 16 | c8(o[o_clr][1]) << 8 | c8(o[o_clr][0]);
            r.bad = !(w > 0) || std::fabs(x / w) > 2.5f || std::fabs(y / w) > 2.5f || std::isnan(x) || std::isnan(y);
            if (r.bad && nsuspect++ < 40) {
                printf("batch %u vertex %u (index base %u): clip (%g %g %g %g) slots %u %u %u %u pos (%g %g %g)\n", nb, i, base,
                       x, y, z, w, v.slot[0], v.slot[1], v.slot[2], v.slot[3], v.pos[0], v.pos[1], v.pos[2]);
                for (int r3 = 0; r3 < 3; r3++) {
                    const f32* row = uni[53 + v.slot[0] + r3];
                    printf("   pal[%u] = %g %g %g %g\n", v.slot[0] + r3, row[0], row[1], row[2], row[3]);
                }
            }
            bad_here += r.bad;
        }
        // flat triangles, clipped against the near plane (w > 0) first
        f32 bbx0 = 1e9f, bby0 = 1e9f, bbx1 = -1e9f, bby1 = -1e9f;
        for (u32 t = 0; t + 2 < nidx; t += 3) {
            const Out* p[3];
            bool ok = true;
            for (int k = 0; k < 3; k++) {
                u32 ix = idx[t + k];
                if (ix < base || ix - base >= nverts) {
                    ok = false;
                    break;
                }
                p[k] = &out[ix - base];
            }
            if (!ok)
                continue;
            // Sutherland-Hodgman against w = 1e-3
            struct V {
                f32 c[4];
            };
            V poly[8], tmp[8];
            int n = 3;
            for (int k = 0; k < 3; k++)
                memcpy(poly[k].c, p[k]->c, 16);
            int m = 0;
            const f32 eps = 1e-3f;
            for (int k = 0; k < n; k++) {
                const V& a = poly[k];
                const V& b = poly[(k + 1) % n];
                bool ina = a.c[3] > eps, inb = b.c[3] > eps;
                if (ina)
                    tmp[m++] = a;
                if (ina != inb) {
                    f32 tt = (eps - a.c[3]) / (b.c[3] - a.c[3]);
                    V v;
                    for (int q = 0; q < 4; q++)
                        v.c[q] = a.c[q] + (b.c[q] - a.c[q]) * tt;
                    tmp[m++] = v;
                }
            }
            if (m < 3)
                continue;
            ntri++;
            // project
            f32 sx[8], sy[8], sz[8];
            for (int k = 0; k < m; k++) {
                f32 w = tmp[k].c[3];
                sx[k] = (1.0f - tmp[k].c[1] / w) * 200.0f;
                sy[k] = (1.0f - tmp[k].c[0] / w) * 120.0f;
                sz[k] = tmp[k].c[2] / w;
                bbx0 = std::fmin(bbx0, sx[k]), bbx1 = std::fmax(bbx1, sx[k]);
                bby0 = std::fmin(bby0, sy[k]), bby1 = std::fmax(bby1, sy[k]);
            }
            u32 col = p[0]->color;
            for (int k = 1; k + 1 < m; k++) {
                f32 ax = sx[0], ay = sy[0], bx = sx[k], by = sy[k], cx = sx[k + 1], cy = sy[k + 1];
                f32 minx = std::fmax(0.0f, std::fmin(ax, std::fmin(bx, cx)));
                f32 maxx = std::fmin((f32)W - 1, std::fmax(ax, std::fmax(bx, cx)));
                f32 miny = std::fmax(0.0f, std::fmin(ay, std::fmin(by, cy)));
                f32 maxy = std::fmin((f32)H - 1, std::fmax(ay, std::fmax(by, cy)));
                f32 zz = (sz[0] + sz[k] + sz[k + 1]) / 3;
                for (int y = (int)miny; y <= (int)maxy; y++)
                    for (int x = (int)minx; x <= (int)maxx; x++) {
                        f32 px = x + 0.5f, py = y + 0.5f;
                        f32 e0 = (bx - ax) * (py - ay) - (by - ay) * (px - ax);
                        f32 e1 = (cx - bx) * (py - by) - (cy - by) * (px - bx);
                        f32 e2 = (ax - cx) * (py - cy) - (ay - cy) * (px - cx);
                        if ((e0 >= 0 && e1 >= 0 && e2 >= 0) || (e0 <= 0 && e1 <= 0 && e2 <= 0)) {
                            if (zz <= depth[y * W + x]) {
                                depth[y * W + x] = zz;
                                img[y * W + x] = col;
                            }
                        }
                    }
            }
        }
        printf("batch %u: %u vertices, %u indices, bools %03X, lights %u, cull %u, %u vertices off screen, screen box (%.0f %.0f)-(%.0f %.0f)\n",
               nb, nverts, nidx, bools, nlights, cull, bad_here, bbx0, bby0, bbx1, bby1);
        nb++;
    }
    fclose(f);
    write_png(argv[3], img.data(), W, H);
    printf("%u batches, %u triangles drawn, %u suspect vertices -> %s\n", nb, ntri, nsuspect, argv[3]);
    return 0;
}
