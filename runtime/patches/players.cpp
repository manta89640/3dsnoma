// The content players, left out of the 3DS port: Adobe Flash (the shop's catalogues and menus, the
// channel's interactive content) and Actimagine's MobiClip (video). Their libraries are stubbed
// (tools/decomp/regions.py) and nothing starts them: the Flash manager stays "not ready" (its
// +0x94, which its methods check) and the movie manager has no movie (its methods check +0x0).
// Nothing builds Flash's heaps (5.6 MB of MEM1, 24 MB of MEM2), its sound thread or MobiClip's
// 4.2 MB work area any more.
#include "noma_decomp.h"
#include "runtime.h"

namespace g {

u32 movie_Player_Open_orig(u32 r3, u32 r4, u32 r5);

// The Flash player's start (flash_Manager_ctor: r3 the manager, then the sizes of its MEM1 and
// MEM2 heaps and its sound buffer): its heaps "FMEM1" and "FMEM2" (+0x10, +0x3C) on blocks of
// MEM1 and MEM2 (+0x0, +0x4; their expanded heaps +0x8, +0xC), then the player and its sound
// thread, which lends the heap "FDYMC" (+0x68). None is made. The scenes built for Flash also keep
// their own data in its heaps (the shop its catalogue in "FMEM2", the content player its
// downloads in "FDYMC"), so they mustn't open: the shop's button is hidden (menus.cpp), and
// content playback is only reached from the server's content, which offline there is none of.
u32 flash_Manager_Init(u32 r3, u32, u32, u32) {
    M2C_FIELD(r3, gp<u32>, 0x8) = 0;  // (the destructor frees what isn't 0)
    M2C_FIELD(r3, gp<u32>, 0xC) = 0;
    return 0;  // not ready: its methods leave the player alone
}

// The movie manager (r3; r4 the heap for its work area): no work area, MobiClip not started. A
// movie opened on it (movie_Manager_Open: content playback, the room screen's server videos) would
// fail at MobiClip's first call; offline none is.
u32 movie_Manager_ctor(u32 r3, u32) {
    GV(u32, GA_movie_Manager_sInstance) = r3;
    M2C_FIELD(r3, gp<u32>, 0x0) = 0;  // the movie
    M2C_FIELD(r3, gp<u32>, 0x4) = 1;
    M2C_FIELD(r3, gp<u32>, 0x8) = 0;  // the work area
    M2C_FIELD(r3, gp<u8>, 0xC) = 0;
    M2C_FIELD(r3, gp<u8>, 0xD) = 0;
    return r3;
}

// A movie opened on it (r4 the movie's player, r5 a heap for it): none is made, as MobiClip isn't
// there to play it, nor the movie: the help's videos (使い方) were the server's, streamed through
// the content requests (fn_80276A40 makes them), as its other movies; the movie's methods leave the
// manager alone (no movie at +0x0).
u32 movie_Manager_Open(u32 r3, u32 r4, u32 r5) {
    (void)r4, (void)r5;
    M2C_FIELD(r3, gp<u32>, 0x0) = 0;
    guest::log("movie: not played (no MobiClip)");
    return 0;
}

// A movie player (the help's, 使い方; movie_Player_Update plays it: +0x3854 its state, 1 playing, 4
// ending, 5 ended; +0x4 its own movie manager) opening a movie: with none made, it goes straight to
// its end (playing, it would read the movie's stream, which isn't there), and the help's list
// comes back.
u32 movie_Player_Open(u32 r3, u32 r4, u32 r5) {
    u32 r = movie_Player_Open_orig(r3, r4, r5);
    if (!M2C_FIELD(r3 + 4, gp<u32>, 0x0))
        M2C_FIELD(r3, gp<s32>, 0x3854) = 4;
    return r;
}

// ... and its destructor, without stopping MobiClip (r4 > 0: delete it too)
void movie_Manager_dtor(u32 r3, u32 r4, u32 r5, u32 r6, u32 r7) {
    if (!r3)
        return;
    movie_Manager_Close(r3, r4, r5, r6, r7);
    GV(u32, GA_movie_Manager_sInstance) = 0;
    if ((s16)r4 > 0)
        operator_delete(r3);
}

}  // namespace g
