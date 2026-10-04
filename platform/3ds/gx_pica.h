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

// a wait for the GPU that hasn't ended in 3 s, logged (the I/O thread, every 100 ms)
void pica_watchdog();

}  // namespace guest
