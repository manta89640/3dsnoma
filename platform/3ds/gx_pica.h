// The GX renderer on the 3DS GPU (gx_pica.cpp).
#pragma once
#include "runtime/hw/gx/gx.h"

namespace guest {

class PicaRenderer : public GxRenderer {
public:
    bool init();
    void draw(u32 prim, u32 vat, u32 count, const u8* data) override;
    void draw_done() override;
    void efb_copy(u32 value) override;
    void suspend() override;
};

}  // namespace guest
