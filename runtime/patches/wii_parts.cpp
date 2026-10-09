// Parts of the channel that are about the Wii itself, left out on the 3DS (the rest of the program
// is unchanged; each original stays as <name>_orig).
#include <algorithm>
#include <utility>
#include <vector>

#include "engine/system/save.h"
#include "noma_decomp.h"
#include "runtime.h"

namespace g {

u32 scene_TitleScene_ctor_orig(u32 r3, u32 r4, u32 r5, u32 r6);
u32 scene_PreTitleScene_ctor_orig(u32 r3, u32 r4);
void wii_MessageData_Load_orig(u32 r3);
void VIWaitForRetrace_orig();
void app_Exit_Dispatch_orig(u32 r3);
u32 app_Exit_Update_orig(u32 r3, u32 r4, u32 r5, u32 r6, u32 r7, u32 r8, u32 r9, u32 r10, f64 f1, f64 f2, f64 f3,
                         f64 f4, f64 f5, f64 f6, f64 f7, f64 f8);

// The Wii Remote strap screen ("use the strap"), shown at the start until a button is pressed or
// 20 seconds have passed (wii_Strap_*): there's no strap on a 3DS. The scenes that show it are
// made without it (r5 for the title scene, r4 for the pre-title one), as they are when the
// channel comes back to them: the title scene plays the start's jingle and the opening at once,
// as it would have when the strap screen went, and the pre-title one moves on when it has loaded.
u32 scene_TitleScene_ctor(u32 r3, u32 r4, u32, u32 r6) { return scene_TitleScene_ctor_orig(r3, r4, 0, r6); }
u32 scene_PreTitleScene_ctor(u32 r3, u32) { return scene_PreTitleScene_ctor_orig(r3, 0); }

// The first start's setup: after the family's Miis, the video quality (StateVideoOption: high or
// normal quality for streaming over the internet, with samples to watch). Left out: the setting
// keeps its default (the scene's +0x8), and the setup goes on to its end (StateFinish).
u32 scene_FirstSetupScene_StateConfirmAddMii_OnEnd(u32 r3) {
    u32 scene = M2C_FIELD(r3, gp<u32>, 0x4) + 0x100000;
    M2C_FIELD(scene, gp<gp<void>>, 0x10) = GFN(scene_FirstSetupScene_StateFinish_Create);
    return scene;
}

// The channel's texts, where they speak of the Wii's own parts, as a 3DS has them: the button
// that went to the Wii Menu ends the app, the Mii Selector takes the Mii Channel's place, the
// touch screen the Wii Remote's pointing, and what was left out isn't announced or warned about.
// An edit replaces part of a message (by its index, as the code asks for it: the first INF1
// group); escape sequences elsewhere in it (button icons) stay. The edited messages go to a block
// of their own, and their INF1 entries point there (DAT1 + offset, wrapping as a u32).
namespace {

struct MessageEdit {
    u16 index;
    const char16_t* from;
    const char16_t* to;
};
const MessageEdit MESSAGE_EDITS[] = {
    {5, u"Wiiメニューへ", u"おわる"},    // the buttons: "to the Wii Menu" -> "quit"
    {142, u"Wiiメニューへ", u"おわる"},  // (the room's menu)
    {124, u"Wiiメニューへ戻ります。", u"Wiiの間をおわります。"},
    {145, u"Wiiメニューに戻ります。", u"Wiiの間をおわります。"},
    {6, u"似顔絵チャンネルへ", u"Miiをえらぶ"},  // no Mii yet: "to the Mii Channel" -> "choose a Mii"
    {125, u"似顔絵チャンネルでMiiを作成しておく\n必要があります。", u"Miiをえらぶ必要があります。"},
    {94, u"似顔絵チャンネルのセーブデータが", u"Miiのデータが"},
    {94, u"\n\n似顔絵チャンネルで確認してください。", u""},
    {140, u"似顔絵チャンネルで", u""},
    {581, u"\n\nこのMiiを『似顔絵チャンネル』で消去、\nまたは似顔絵パレードに移動させると、\n登録したMiiの情報も消去されます。\nご注意ください。", u""},
    {121, u"\n\n続いて映像の設定を行います。", u""},  // (the setup's video step: left out above)
    // the room's first tips, for its buttons on the bottom screen (engine/room/panel.h)
    {126, u"部屋にWiiリモコンを向けてみましょう！\n中央のスクリーンやまわりのメニューを選んで\n\ue042を押すと、いろいろな機能を使えます。",
     u"下の画面にタッチしてみましょう！\n下の画面のボタンにタッチすると、\nいろいろな機能を使えます。"},
    {128, u"下のメニューから、映画などをレンタルできる\n「ホームシアター」や、さまざまなテーマの\n「いろんな間」などへ移動できます。",
     u"下の画面の下のボタンから、「ホームシアター」や、\n音楽を聞ける「サウンド」、ウーフー島へ\n行ける「出発」などへ移動できます。"},
    {130, u"右上の「その他」にリモコンを向けると\n隠れたメニューが出てきて、メッセージを\n読んだりすることができます。",
     u"下の画面の上のボタンから、メッセージを\n読んだり、設定を変えたりすることが\nできます。"},
    {134, u"\n足りない場合は「Wiiポイント」でWiiショッ\nピングチャンネルに移動して追加できます。", u""},
    // "can't connect" (what the shop, content and online parts show offline): no Wii settings to check
    {277, u"\nWii本体設定のインターネット設定を\nご確認ください。", u""},
    {278, u"\nWii本体設定のインターネット設定を\nご確認ください。", u""},
};

std::vector<u8> be16(const char16_t* s) {
    std::vector<u8> out;
    for (; *s; s++)
        out.push_back((u8)(*s >> 8)), out.push_back((u8)*s);
    return out;
}

// a message's bytes, to its terminator (an escape sequence, 0x1A then its length in bytes, whole)
std::vector<u8> message_bytes(const u8* p) {
    std::vector<u8> out;
    for (size_t i = 0;;) {
        u16 c = (u16)(p[i] << 8 | p[i + 1]);
        if (c == 0)
            return out;
        size_t n = c == 0x1A && p[i + 2] >= 3 ? p[i + 2] : 2;
        out.insert(out.end(), p + i, p + i + n);
        i += n;
    }
}

}  // namespace

void wii_MessageData_Load(u32 r3) {
    wii_MessageData_Load_orig(r3);
    u32 groups = M2C_FIELD(r3, gp<u32>, 0x4), count = M2C_FIELD(r3, gp<u32>, 0x8);
    u32 dat = M2C_FIELD(r3, gp<u32>, 0x10);
    u32 inf = 0;
    for (u32 i = 0; i < count && !inf; i++)
        if (guest::load16(guest::load32(groups + 4 * i) + 0xC) == 0)
            inf = guest::load32(groups + 4 * i);
    if (!inf || !guest::host_ptr(dat))
        return;
    u32 stride = guest::load16(inf + 0xA), entries = guest::load16(inf + 0x8);
    std::vector<std::pair<u16, std::vector<u8>>> edited;  // index, new text (with its terminator)
    for (const MessageEdit& e : MESSAGE_EDITS) {
        if (e.index >= entries)
            continue;
        auto it = std::find_if(edited.begin(), edited.end(), [&](const auto& m) { return m.first == e.index; });
        if (it == edited.end()) {
            u32 at = dat + guest::load32(inf + 0x10 + e.index * stride);
            edited.push_back({e.index, message_bytes(guest::host_ptr(at))});
            it = edited.end() - 1;
        }
        std::vector<u8> from = be16(e.from), to = be16(e.to), &text = it->second;
        bool found = false;
        for (size_t i = 0; i + from.size() <= text.size(); i += 2)
            if (std::equal(from.begin(), from.end(), text.begin() + i)) {
                text.erase(text.begin() + i, text.begin() + i + from.size());
                text.insert(text.begin() + i, to.begin(), to.end());
                found = true;
                break;
            }
        if (!found)
            guest::log("message %u: an edit's text isn't in it (left as it was)", e.index);
    }
    u32 size = 0;
    for (auto& m : edited)
        m.second.push_back(0), m.second.push_back(0), size += (u32)m.second.size();
    u32 block = operator_new_array(size);
    if (!block)
        return;
    for (auto& m : edited) {
        guest::write_g(block, m.second.data(), (u32)m.second.size());
        guest::store32(inf + 0x10 + m.first * stride, block - dat);
        block += (u32)m.second.size();
    }
}

// VIWaitForRetrace, which the render loop (fn_801C00D8) calls once a frame: the next retrace,
// unless one has come since the last call. On the Wii a frame that took longer than a retrace
// still waits for the next one, so every frame takes whole retraces (60, 30, 20, 15 fps); on the
// 3DS, where the room's frames take several, that loses half a retrace a frame on average. A
// frame quicker than a retrace still waits for it: 60 fps at most.
// That wait was also when the threads below the main thread's priority ran (loading, the EC
// manager that the way out waits for): if any is ready, they get the time up to the retrace, in
// slices they give back as soon as none is ready.
void VIWaitForRetrace() {
    static u32 s_last;
    u32 now = GV(u32, GA___VIRetraceCount);
    if (now == s_last) {
        VIWaitForRetrace_orig();
    } else {
        // the running thread (OS globals 0x800000E4), its priority (+0x2D0); the ready queues by
        // priority (RunQueueBits: bit 31 - priority)
        u32 thread = guest::load32(0x800000E4);
        u32 prio = thread ? guest::load32(thread + 0x2D0) : 31;
        u32 below = prio < 31 ? (1u << (31 - prio)) - 1 : 0;
        while ((GV(u32, GA_RunQueueBits_80883228) & below) && GV(u32, GA___VIRetraceCount) == now)
            OSSleepTicks(0, (u32)(guest::TB_HZ / 4000));  // 0.25 ms
    }
    s_last = GV(u32, GA___VIRetraceCount);
}

// The way out (app::Exit: app_Exit_Request, then app_Exit_Update every frame until it goes on to
// app_Exit_Dispatch) counts frames: the fade to black takes 60 (+0xC up to +0xA, while +0x8 is
// set), and the sound stops and the program may go on after 120 (+0x4): 1 and 2 s at the Wii's
// 60 fps, but 4 and 8 s at the room's 15 fps on the 3DS. Here they count 60ths of a second: before
// each update both are brought up to the time since the first update, one short of the count the
// update itself then reaches (so the sound still stops at exactly the 120th).
u32 app_Exit_Update(u32 r3, u32 r4, u32 r5, u32 r6, u32 r7, u32 r8, u32 r9, u32 r10, f64 f1, f64 f2, f64 f3, f64 f4,
                    f64 f5, f64 f6, f64 f7, f64 f8) {
    static u64 s_start;  // the time of the first update
    if (M2C_FIELD(r3, gp<s32>, 0x0) != 0) {
        u64 now = guest::timebase();
        u32 frames = M2C_FIELD(r3, gp<u32>, 0x4);
        if (frames == 0)
            s_start = now;
        u32 n = (u32)((now - s_start) * 60 / guest::TB_HZ);
        if (n > 0) {
            if (frames < 0x78)
                M2C_FIELD(r3, gp<u32>, 0x4) = std::max<u32>(frames, std::min<u32>(n, 0x78) - 1);
            u32 fade = M2C_FIELD(r3, gp<u16>, 0xC), len = M2C_FIELD(r3, gp<u16>, 0xA);
            if (M2C_FIELD(r3, gp<u8>, 0x8) && fade < len)
                M2C_FIELD(r3, gp<u16>, 0xC) = (u16)std::max<u32>(fade, std::min<u32>(n, len) - 1);
        }
    }
    return app_Exit_Update_orig(r3, r4, r5, r6, r7, r8, r9, r10, f1, f2, f3, f4, f5, f6, f7, f8);
}

// Leaving for another Wii program (the Wii Menu, the Mii Channel, the Wii Shop, ...: the exit
// codes of app_Exit_Dispatch) ends the app here, as there are none. The Mii Channel (offered when
// there's no Mii yet) is the 3DS's Mii Selector instead: the app starts again with the Mii. The
// options' save-data button ends the app this way too: its save is then erased and the app starts
// again, at its first start (engine/system/save.h).
void app_Exit_Dispatch(u32 r3) {
    if (engine::save::erase_if_marked())
        guest::platform_restart("the save data was erased: starting afresh");
    if (M2C_FIELD(r3, gp<s32>, 0x0) == 18 && guest::platform_choose_mii())
        guest::platform_quit("starting again with the chosen Mii");
    app_Exit_Dispatch_orig(r3);
}

}  // namespace g
