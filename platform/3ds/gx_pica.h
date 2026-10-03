// The GX renderer on the 3DS GPU (gx_pica.cpp).
#pragma once
#include "runtime/hw/gx/gx.h"

namespace guest {

class PicaRenderer : public GxRenderer {
public:
    bool init();
    void draw(u32 prim, u32 vat, u32 count, const u8* data) override;
    void bp_write(u32 reg, u32 value) override;
    void xf_write(u32 addr, u32 value) override;
    void efb_copy(u32 value) override;
};

}  // namespace guest
