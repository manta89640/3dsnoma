// External frame buffers: YUYV 4:2:2 in main memory, what the video interface scans out.
#include "raster.h"

namespace guest {

static u8 clamp8(s32 v) { return (u8)(v < 0 ? 0 : v > 255 ? 255 : v); }

void gx_xfb_to_rgba(u32 addr, u32 width, u32 height, u32 stride, u32* out) {
    for (u32 y = 0; y < height; y++) {
        const u8* row = host_ptr(addr + y * stride);
        for (u32 x = 0; x + 1 < width + 1; x += 2) {
            s32 y0 = 0, u = 128, y1 = 0, v = 128;
            if (row && x * 2 + 3 < stride) {
                const u8* p = row + x * 2;
                y0 = p[0], u = p[1], y1 = p[2], v = p[3];
            }
            for (int k = 0; k < 2 && x + k < width; k++) {
                s32 c = ((k ? y1 : y0) - 16) * 298;
                s32 r = (c + 409 * (v - 128) + 128) >> 8;
                s32 g = (c - 100 * (u - 128) - 208 * (v - 128) + 128) >> 8;
                s32 b = (c + 516 * (u - 128) + 128) >> 8;
                out[y * width + x + k] = clamp8(r) | clamp8(g) << 8 | clamp8(b) << 16 | 0xFF000000u;
            }
        }
    }
}

}  // namespace guest
