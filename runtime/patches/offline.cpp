// The channel without its servers: the port is offline (the user's call for the 3DS redesign: no
// online mode at all, net.cpp has no network). The steps that go online are skipped where the
// channel starts them, and what only a server can provide is left out. The functions here that
// replace a step whole are natives (tools/decomp/decompile.py NATIVE_NAMES); those that adjust
// one keep the original as <name>_orig (PATCHED_NAMES).
#include "noma_decomp.h"
#include "runtime.h"

namespace g {

void wii_ServerClock_ctor_orig(u32 r3);
void comm_httpcache_Request_MakeUrl_orig(u32 r3, u32 r4);
void room_ContentImage_Prefetch_orig(u32 r3, u32 r4);
u32 scene_TitleScene_Opening_OnLogoEnd_orig(u32 r3);
u32 scene_TitleScene_Opening_OnOpeningEnd_orig(u32 r3);
void scene_TitleScene_Opening_Update_orig(u32 r3);
u32 comm_ec_EcManager_UpdateOff_orig(u32 r3);
void comm_ec_EcManager_AddRequest_orig(u32 r3, u32 r4);
u32 comm_nwc24_Nwc24Manager_UpdateClosed_orig(u32 r3);
void comm_nwc24_Nwc24Manager_AddRequest_orig(u32 r3, u32 r4);

// The title scene's startup task (TitleScene + 0x150): the WiiConnect24 friend data
// (comm_friends_FriendInitializer_ReadNwc24Friends, local), then the online checks (comm_friends_FriendInitializer_CheckOnline: network up to the DWC_Tac
// notification, the Tac service's answers). The task's state machine (comm_friends_FriendInitializer_Update) applies the
// friend data when 0xDB80 is set and the Tac answers when 0xDB81 is: offline there are none.
u32 comm_friends_FriendInitializer_Run(u32 r3) {
    u32 ok = comm_friends_FriendInitializer_ReadNwc24Friends(r3);
    if (ok)
        M2C_FIELD(r3, gp<s8>, 0xDB80) = 1;
    return ok;
}

// A job (comm_httpcache_Job_ctor) goes to the download manager (comm_httpcache_Manager_sInstance): through the NAND cache, HTTP
// and the XML parser it ends in state 6 (done), 7 (failed) or 8 (cancelled). Offline every job is
// done at once, an empty document: its target (+0x40, +0x44 bytes) zero, nothing downloaded. So
// the data manager's startup documents (comm_DataManager_sInstance, what comm_DataManager_IsStartupDataReady waits for: first.bin,
// reginfo.cgi, event/today.xml, pay/event/today.xml, conf2/paylink.xml, special/all.xml) parse
// to nothing (no posters, Miis, news, ...), and so does the first start's license (conf/eula.xml).
// Left to the download manager, a job would wait for a network that never comes: the room's
// texture cache keeps a slot per job until its job ends and then has none left (a null slot it
// doesn't check), and a job that does fail makes the room report "can't connect" (50220). Some
// callers also read a job's target at once (a wall poster reads its .met, which on the console
// the NAND cache has filled): zero, not the heap's fill pattern.
void comm_httpcache_Manager_AddJob(u32 r3, u32 r4) {
    memset(M2C_FIELD(r4, gp<u32>, 0x40), 0, M2C_FIELD(r4, gp<u32>, 0x44));
    comm_httpcache_Job_SetState(r4, 6);
}

// A job's URL (r3: the job's request at +0x10, r4: the buffer): offline nothing should be
// downloaded any more, so each URL the channel still asks for is logged.
void comm_httpcache_Request_MakeUrl(u32 r3, u32 r4) {
    comm_httpcache_Request_MakeUrl_orig(r3, r4);
    guest::log("offline: download %s", guest::read_string(r4, 0x200).c_str());
}

// The server clock (wii_ServerClock_sInstance): the offset from the console's clock, known (+0x30) once a
// server has answered; the title's later downloads wait for it. Offline the console's clock is
// the only one.
void wii_ServerClock_ctor(u32 r3) {
    wii_ServerClock_ctor_orig(r3);
    M2C_FIELD(r3, gp<s8>, 0x30) = 1;
}

// The server's time (r5:r6, ticks), from the data manager's configuration: the clock keeps the
// difference to the console's (+0x28). Offline that document is empty, its time zero -- year 0,
// and every date the channel shows came out two thousand years early (a weekday of -5 named
// through a pointer). The console's clock stays the only one.
void wii_ServerClock_SetServerTime(u32 r3, u32, u32, u32) {
    M2C_FIELD(r3, gp<s8>, 0x30) = 1;
}

// The title scene preloads the room's posters (event/today.xml's posterinfo): their images
// (TitleScene + 0xDCDC, done when +0x34 is set) and details (+0xDD14, done in state 3), and
// waits for both. Offline there are no posters.
void scene_TitleScene_PosterImageLoader_Update(u32 r3) {
    M2C_FIELD(r3, gp<u8>, 0x34) = 1;
}

void scene_TitleScene_PosterInfoLoader_Update(u32 r3) {
    M2C_FIELD(r3, gp<s32>, 0x0) = 3;
}

// The room's content images (r3: the loader, +0x98 its image cache; r4: a record of the
// content data): the record's image (+0x3C) goes into the cache unless it is there (+0x54).
// Offline the content data is empty and its records have no image: the cache's slots take only
// real images (comm_ImageCache_Entry_AcceptsType), and the caller doesn't check for none.
void room_ContentImage_Prefetch(u32 r3, u32 r4) {
    if (M2C_FIELD(r4, gp<u32>, 0x3C) == 0)
        return;
    room_ContentImage_Prefetch_orig(r3, r4);
}

// The first start's setup (FirstSetupScene): a welcome, the license agreement, the region,
// the video options and the family's Miis. Offline the license agreement is left out: there's no
// service to agree to.
// The welcome's end (r3: StateWelcome, +4 the scene): the license agreement, or waiting for its
// text. Offline: what agreeing leads to (scene_FirstSetupScene_StateLicenseAgree_OnAnswer), the region setting, or what comes after
// it when the region is set already.
u32 scene_FirstSetupScene_StateWelcome_OnEnd(u32 r3, u32) {
    GFRAME(0x20);
    fn_801A5DAC((u32)&GS(u8, 0x8));
    u32 scene = M2C_FIELD(r3, gp<u32>, 0x4) + 0x100000;
    M2C_FIELD(scene, gp<gp<void>>, 0x10) = GS(u8, 0x8) ? GFN(scene_FirstSetupScene_StateAddMii_Create) : GFN(scene_FirstSetupScene_StateRegionSetting_Create);
    return scene;
}

// The title scene's opening (TitleScene + 0xC: state +0x0, counter +0x4): when the logo ends the
// opening starts (628 frames), and 20 frames into it the state goes from 2 to 3
// (scene_TitleScene_Opening_Update); from then on the scene moves on as soon as its startup data
// is ready. Online that data comes from the servers while the opening plays; offline it is ready
// at once, and the opening would stop after 20 frames. Offline, the state changes when the
// opening ends.
u32 scene_TitleScene_Opening_OnLogoEnd(u32 r3) {
    u32 r = scene_TitleScene_Opening_OnLogoEnd_orig(r3);
    M2C_FIELD(r3, gp<s32>, 0x4) = 0x7FFFFFFF;
    return r;
}

u32 scene_TitleScene_Opening_OnOpeningEnd(u32 r3) {
    u32 r = scene_TitleScene_Opening_OnOpeningEnd_orig(r3);
    if (M2C_FIELD(r3, gp<s32>, 0x0) == 2)
        M2C_FIELD(r3, gp<s32>, 0x4) = 1;
    return r;
}

// ... and A (or a tap, which the game's remote has as A) skips the rest of it: the state moves on
// at the next update, as at the opening's end
void scene_TitleScene_Opening_Update(u32 r3) {
    const u32 controller = GV(u32, GA_input_Controller_sInstance);
    if (M2C_FIELD(r3, gp<s32>, 0x0) == 2 && controller && (M2C_FIELD(controller, gp<u32>, 0xB44) & 0x800)) {
        guest::log("title: the opening skipped");
        M2C_FIELD(r3, gp<s32>, 0x4) = 1;
    }
    scene_TitleScene_Opening_Update_orig(r3);
}

// ---- background services. The channel keeps managers on threads of their own that work
// through requests from the rest of the program; offline some have nothing to do at all.

// model::Weather reads the Forecast Channel's forecast (its WiiConnect24 download) at the start
// and when the room comes up after 30 minutes, on a thread of its own: through the WC24
// library, opened for it by the Nwc24Manager (11 files and devices). Offline, and on a 3DS,
// there is no forecast to read: the thread ends as it does when the library can't be opened
// (+0 done, no data at +2), and the room shows its weather without one.
u32 model_Weather_Read(u32 r3) {
    M2C_FIELD(r3, gp<s8>, 0x0) = 1;
    return 0;
}

// comm::message::ReceiveMessageManager reads the WiiConnect24 inbox at the start, then again
// every 5 minutes (+0x3BBC: 5 waits until the time at +0x3BB0, then 0 reads again). Offline
// nothing delivers mail, so the inbox read at the start is the last: it waits for good.
void comm_message_ReceiveMessageManager_WaitNextRead(u32) {}

// The EC manager (comm::ec::EcManager, the Wii Shop's library) and the Nwc24Manager (the
// WiiConnect24 library, opened for its users) are request queues with a thread each that looks
// at its queue every 16 ms, idle or not. Offline they wait instead: when the thread finds itself
// idle (the EC library off and nothing asked: state 0 at its target 2; the WC24 library closed
// and no request), it suspends itself until a request is added, which resumes it.
namespace {

struct IdleWait {
    u32 thread = 0;  // the manager's thread (an OSThread) while it waits

    // the manager's thread: suspended if `idle` still holds with interrupts off, so that a
    // request added from another thread can't come between the check and the suspend
    template <class F>
    void wait_while(F idle) {
        u32 level = NOMA_HI32(OSDisableInterrupts());
        if (idle()) {
            thread = OSGetCurrentThread();
            OSSuspendThread(thread);
            thread = 0;
        }
        OSRestoreInterrupts(level);
    }
    // a request was added: the thread looks at it (a resume of a thread that isn't suspended
    // does nothing)
    void wake() {
        if (thread)
            OSResumeThread(thread);
    }
};

IdleWait s_ec_idle, s_nwc24_idle;

// an empty list head (next and prev pointing back at it)
bool list_empty(u32 head) { return guest::load32(head) == head && guest::load32(head + 4) == head; }

}  // namespace

// EcManager state 0 (the library off): returns 1 when the target is off too (+0x53C8 == 2),
// after marking the current request (+0x53C4) done if it asked for that.
u32 comm_ec_EcManager_UpdateOff(u32 r3) {
    u32 r = comm_ec_EcManager_UpdateOff_orig(r3);
    if (r == 1) {
        s_ec_idle.wait_while([r3] {
            return list_empty(r3 + 0x53BC) && M2C_FIELD(r3, gp<s32>, 0x53C8) == 2;
        });
        return 0;  // (look at the queue again at once)
    }
    return r;
}

void comm_ec_EcManager_AddRequest(u32 r3, u32 r4) {
    comm_ec_EcManager_AddRequest_orig(r3, r4);
    s_ec_idle.wake();
}

// Nwc24Manager state 0 (the library closed): returns 1 when no request is queued (+0x3A8).
u32 comm_nwc24_Nwc24Manager_UpdateClosed(u32 r3) {
    u32 r = comm_nwc24_Nwc24Manager_UpdateClosed_orig(r3);
    if (r == 1) {
        s_nwc24_idle.wait_while([r3] { return list_empty(r3 + 0x3A8); });
        return 0;
    }
    return r;
}

void comm_nwc24_Nwc24Manager_AddRequest(u32 r3, u32 r4) {
    comm_nwc24_Nwc24Manager_AddRequest_orig(r3, r4);
    s_nwc24_idle.wake();
}

}  // namespace g
