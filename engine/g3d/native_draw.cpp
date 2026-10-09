// Native draws inside the GX frame (native_draw.h): the packet ring, the FIFO command, executing it.
//
// A ring slot is FREE, FILLING (the CPU's thread has it between begin and submit) or QUEUED (its
// command is in the stream). The renderer's thread executes a QUEUED slot whose packet has the
// command's id and frees it; a command whose slot holds another packet or isn't QUEUED is stale (a
// packet given up on: see wait_free) and ignored. Packet ids count up from 1, so slot = id % size.
#include "engine/g3d/native_draw.h"

#include "noma_decomp.h"

#include <algorithm>
#include <atomic>

#include "hw/gx/regs.h"
#include "hw/hw.h"

namespace engine::g3d {

namespace {

enum : u32 { FREE = 0, FILLING = 1, QUEUED = 2 };

struct Slot {
    Packet packet;
    std::atomic<u32> state{FREE};
};
Slot s_slots[NATIVE_DRAW_PACKETS];
u32 s_next_id;          // (the CPU's thread)
Backend* s_backend;
// packages the backend has prepared and not released (the renderer's thread; the CPU's thread
// only once the renderer is idle: native_draw_release)
std::vector<const Package*> s_prepared;

struct Counters {
    std::atomic<u32> submitted{0}, executed{0}, drawn{0}, stale{0}, lost{0}, waits{0};
} s_count;

Slot& slot_of(u32 id) { return s_slots[id % NATIVE_DRAW_PACKETS]; }

// the command: opcode and the packet's id, big-endian, through the write-gather pipe
void write_command(u32 id) {
    guest::gx_fifo_write(0, NATIVE_DRAW_OPCODE, 1);
    guest::gx_fifo_write(0, id, 4);
}

// what the pipe holds goes to the FIFO now: NOPs to the end of its 32-byte burst (GXFlush's way)
void flush_pipe() {
    while (guest::g_gather_n)
        guest::gx_fifo_write(0, 0, 1);
}

// The slot must be free before its next packet goes in: the renderer runs behind, so wait for it
// to execute what it has (the pipe flushed, the GPU's thread drained); a packet still not executed
// then never will be (no renderer, the FIFO not being run): it is given up on.
void wait_free(Slot& s) {
    if (s.state.load(std::memory_order_acquire) == FREE)
        return;
    s_count.waits++;
    flush_pipe();
    guest::gx_sync();
    if (s.state.load(std::memory_order_acquire) != FREE) {
        if (s_count.lost++ == 0)
            guest::log("native draw: packet %u was never executed (no renderer?): given up on", s.packet.id);
        s.state.store(FREE, std::memory_order_release);
    }
}

bool prepared(const Package* p) { return std::find(s_prepared.begin(), s_prepared.end(), p) != s_prepared.end(); }

// (a backend gets the package as its own to keep resources in: Package::backend_data)
void prepare(const Package* p) {
    if (!s_backend || !p || prepared(p))
        return;
    s_backend->prepare(*const_cast<Package*>(p));
    s_prepared.push_back(p);
}

void release(const Package* p) {
    auto it = std::find(s_prepared.begin(), s_prepared.end(), p);
    if (it == s_prepared.end())
        return;
    s_prepared.erase(it);
    if (s_backend)
        s_backend->release(*const_cast<Package*>(p));
}

}  // namespace

void Packet::reset() {
    kind = PacketKind::DRAW;
    package = nullptr;
    materials = nullptr;
    pass = Pass::OPAQUE;
    memset(view, 0, sizeof(view));
    world_scale = 1.0f;
    eye[0] = eye[1] = eye[2] = 0;
    lights.clear();
    light_sets.clear();
    fogs.clear();
    time = 0;
    static const f32 k_identity[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    matrices.assign(k_identity, k_identity + 12);
    mat_states.clear();
    items.clear();
    shadow = ShadowMap();
    casters.clear();
    sea.amp.clear();
    dynamic.clear();
    dynamic_ranges.clear();
}

GxView native_draw_view() {
    using namespace guest;
    GxView v;
    f32 p[6];
    for (int i = 0; i < 6; i++)
        p[i] = xf_f32(XF_PROJECTION + i);
    v.orthographic = g_gx.xf[XF_PROJECTION + 6] != 0;
    memset(v.projection, 0, sizeof(v.projection));
    if (v.orthographic) {
        v.projection[0][0] = p[0], v.projection[0][3] = p[1];
        v.projection[1][1] = p[2], v.projection[1][3] = p[3];
        v.projection[2][2] = p[4], v.projection[2][3] = p[5];
        v.projection[3][3] = 1;
    } else {
        v.projection[0][0] = p[0], v.projection[0][2] = p[1];
        v.projection[1][1] = p[2], v.projection[1][2] = p[3];
        v.projection[2][2] = p[4], v.projection[2][3] = p[5];
        v.projection[3][2] = -1;
    }
    for (int i = 0; i < 6; i++)
        v.viewport[i] = xf_f32(XF_VIEWPORT + i);
    v.scissor_tl = g_gx.bp[BP_SCISSOR_TL], v.scissor_br = g_gx.bp[BP_SCISSOR_BR];
    v.scissor_offset = g_gx.bp[BP_SCISSOR_OFFSET];
    return v;
}

void native_draw_set_backend(Backend* backend) { s_backend = backend; }
Backend* native_draw_backend() { return s_backend; }

Packet* native_draw_begin() {
    if (++s_next_id == 0)
        s_next_id = 1;
    Slot& s = slot_of(s_next_id);
    wait_free(s);
    s.state.store(FILLING, std::memory_order_relaxed);
    s.packet.reset();
    s.packet.id = s_next_id;
    return &s.packet;
}

void native_draw_submit(Packet* p) {
    Slot& s = slot_of(p->id);
    s_count.submitted++;
    // The GX library keeps what GXSetProjection, GXSetViewport and the like change as dirty state
    // and writes it to the FIFO at the next GXBegin (__GXSetDirtyState); a native draw is no GX draw,
    // so the registers it reads at its command would still be the last draw's (the 2D layouts'
    // orthographic projection: the island's opaque pass, the frame's first 3D, was skipped whole).
    // So the state goes out first, as GXBegin would send it (once GX is set up: not in self-tests).
    if (p->kind == PacketKind::DRAW && guest::load32(GA___GXData))
        g::__GXSetDirtyState();
    // (QUEUED before the command is written: on a host without a GPU thread the decoder runs
    // inline and may execute it before gx_fifo_write returns)
    s.state.store(QUEUED, std::memory_order_release);
    write_command(p->id);
}

void native_draw_prepare(Package& package) {
    Packet* p = native_draw_begin();
    p->kind = PacketKind::PREPARE;
    p->package = &package;
    native_draw_submit(p);
}

void native_draw_release(Package& package) {
    Packet* p = native_draw_begin();
    p->kind = PacketKind::RELEASE;
    p->package = &package;
    native_draw_submit(p);
    flush_pipe();
    guest::gx_sync();
    // the renderer is idle now. Commands it hasn't executed never will be: their packets are given
    // up on (a late decode finds them stale), and the release happens here
    for (Slot& s : s_slots)
        if (s.state.load(std::memory_order_acquire) == QUEUED) {
            s_count.lost++;
            s.state.store(FREE, std::memory_order_release);
        }
    release(&package);
}

void native_draw_execute(u32 id, bool draw) {
    Slot& s = slot_of(id);
    if (s.state.load(std::memory_order_acquire) != QUEUED || s.packet.id != id) {
        s_count.stale++;
        return;
    }
    const Packet& p = s.packet;
    switch (p.kind) {
    case PacketKind::PREPARE: prepare(p.package); break;
    case PacketKind::RELEASE: release(p.package); break;
    case PacketKind::DRAW:
        if (draw && s_backend && p.package) {
            prepare(p.package);
            s_backend->draw(p, native_draw_view());
            s_count.drawn++;
        }
        break;
    }
    s_count.executed++;
    s.state.store(FREE, std::memory_order_release);
}

NativeDrawStats native_draw_stats() {
    return {s_count.submitted.load(), s_count.executed.load(), s_count.drawn.load(),
            s_count.stale.load(),     s_count.lost.load(),     s_count.waits.load()};
}

}  // namespace engine::g3d
