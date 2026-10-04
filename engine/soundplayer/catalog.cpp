// The Sound Player's tracks (see catalog.h).
//
// The sound archive as nw4r::snd keeps it (sound::SoundManager at lbl_80882E48: +0 its
// MemoryArchive, whose +4 is the nw4r::snd::MemorySoundArchive; that one's +4 the
// SoundArchiveFileReader): the reader's +0x28 is the INFO block (past its 8-byte header), +0x2C the
// SYMB block, +0x30 SYMB's string table {u32 count, u32 offsets from SYMB}. INFO starts with
// DataRefs {u8 type, u8, u16, u32 offset from INFO} to its tables, each {u32 count, DataRef[count]}:
// +0x00 sounds, +0x10 players. A sound (SoundCommonInfo): +0x00 label (string index), +0x04 file,
// +0x08 player, +0x14 u8 volume, +0x16 u8 type (1 sequence, 2 stream, 3 wave), +0x18 DataRef to its
// type's info (a sequence's: +0 offset of its data in the file). A player: +0x00 name (string index).
#include "engine/soundplayer/catalog.h"

#include <algorithm>
#include <map>
#include <set>

#include "noma_decomp.h"

namespace engine::soundplayer {

namespace {

enum : u8 { SOUND_SEQ = 1 };

// The channel's labels in words, in the list's order: the room's own music first, then the other
// rooms and screens, the seasonal slideshows, the shop; the jingles last.
struct Named {
    const char* label;
    const char16_t* name;
    bool jingle;
    bool loops;
};
const Named NAMES[] = {
    // the room: its theme (one on odd days of the month, one on even days), meals, visitors,
    // birthdays (sung once)
    {"SEQ_BGM_MAIN_01", u"Wiiの間のテーマ 1", false, true},
    {"SEQ_BGM_MAIN_02", u"Wiiの間のテーマ 2", false, true},
    {"SEQ_BGM_MEALS", u"ごはんの時間", false, true},
    {"SEQ_BGM_CONCIERGE", u"コンシェルジュ", false, true},
    {"SEQ_BGM_CONCIERGE_WAITING", u"コンシェルジュ 2", false, true},
    {"SEQ_BGM_BIRTHDAY", u"おたんじょうび", false, false},
    // the start and the settings
    {"SEQ_BGM_BOOT", u"タイトル", false, true},
    {"SEQ_BGM_INI_SET", u"はじめの設定", false, true},
    // the bar's other places
    {"SEQ_BGM_THEATERROOM", u"ホームシアター", false, true},
    {"SEQ_BGM_TELEVISION_NEW", u"番組を見る", false, true},
    {"SEQ_BGM_MARCH", u"いろんな間", false, true},
    {"SEQ_BGM_COMPANY", u"企業の間 A", false, true},
    {"SEQ_BGM_COMPANY_B", u"企業の間 B", false, true},
    {"SEQ_BGM_COMPANY_C", u"企業の間 C", false, true},
    {"SEQ_BGM_COMPANY_D", u"企業の間 D", false, true},
    {"SEQ_BGM_COMPANY_E", u"企業の間 E", false, true},
    {"SEQ_BGM_COMPANY_F", u"企業の間 F", false, true},
    {"SEQ_BGM_COMPANY_G", u"企業の間 G", false, true},
    {"SEQ_BGM_COMPANY_H", u"企業の間 H", false, true},
    {"SEQ_BGM_VOTE", u"みんなで投票", false, true},
    {"SEQ_BGM_DSI_DELIVERY_NEW", u"DSi配信", false, true},
    // the calendar's seasonal slideshows (歳時記), in the order the slideshow numbers them
    {"SEQ_BGM_SAIZIKI_A", u"歳時記 1", false, true},
    {"SEQ_BGM_SAIZIKI_B", u"歳時記 2", false, true},
    {"SEQ_BGM_SAIZIKI_C", u"歳時記 3", false, true},
    {"SEQ_BGM_SAIZIKI_D", u"歳時記 4", false, true},
    {"SEQ_BGM_SAIZIKI_E", u"歳時記 5", false, true},
    {"SEQ_BGM_SAIZIKI_F", u"歳時記 6", false, true},
    {"SEQ_BGM_SAIZIKI_G", u"歳時記 7", false, true},
    {"SEQ_BGM_SAIZIKI_H", u"歳時記 8", false, true},
    {"SEQ_BGM_SAIZIKI_09", u"歳時記 9", false, true},
    {"SEQ_BGM_SAIZIKI_10", u"歳時記 10", false, true},
    {"SEQ_BGM_SAIZIKI_11", u"歳時記 11", false, true},
    {"SEQ_BGM_SAIZIKI_12", u"歳時記 12", false, true},
    {"SEQ_BGM_SAIZIKI_I", u"歳時記 13", false, true},
    // the shop (its pages were Flash, from the server)
    {"SEQ_BGM_SHOP_BOOT", u"ショッピング（入店）", false, true},
    {"SEQ_BGM_SHOP_MAIN", u"ショッピング", false, true},
    {"SEQ_BGM_SHOP_EXCHANGE", u"ショッピング（交換）", false, true},
    {"SEQ_BGM_SHOP_RETURN", u"ショッピング（おかえり）", false, true},
    {"SEQ_BGM_SHOP_MISC1", u"ショッピング 1", false, true},
    {"SEQ_BGM_SHOP_MISC2", u"ショッピング 2", false, true},
    {"SEQ_BGM_SHOP_MISC3", u"ショッピング 3", false, true},
    {"SEQ_BGM_SHOP_MISC4", u"ショッピング 4", false, true},
    {"SEQ_BGM_SHOP_MISC5", u"ショッピング 5", false, true},
    // jingles: the Miis' feelings (messages), a Mii playing a DSi, the 5 pm chime ("Going Home"),
    // the company logos of いろんな間
    {"SEQ_SE_FEELING_GREETING", u"あいさつ", true, false},
    {"SEQ_SE_FEELING_CONGRATULATION", u"おめでとう", true, false},
    {"SEQ_SE_FEELING_EXCITEMENT", u"わくわく", true, false},
    {"SEQ_SE_FEELING_CALM", u"おだやか", true, false},
    {"SEQ_SE_FEELING_APOLOGIZE", u"ごめんなさい", true, false},
    {"SEQ_SE_FEELING_SADNESS", u"かなしい", true, false},
    {"SEQ_SE_PLAY_GAME_DSI", u"DSiであそぶ", true, false},
    {"SEQ_SE_PLAY_GAME_DSI_02_LOOP", u"DSiであそぶ 2", true, true},
    {"SEQ_SE_012_JIHOU_1700", u"夕方5時のチャイム", true, false},
    {"WAV_SE_123_COMPANY_LOGO", u"企業ロゴ A", true, false},
    {"WAV_SE_124_COMPANY_LOGO", u"企業ロゴ B", true, false},
    {"WAV_SE_125_COMPANY_LOGO", u"企業ロゴ C", true, false},
    {"SEQ_SE_126_COMPANY_LOGO_D", u"企業ロゴ D", true, false},
    {"SEQ_SE_127_COMPANY_LOGO_E", u"企業ロゴ E", true, false},
    {"SEQ_SE_128_COMPANY_LOGO_F", u"企業ロゴ F", true, false},
    {"SEQ_SE_129_COMPANY_LOGO_G", u"企業ロゴ G", true, false},
    {"SEQ_SE_130_COMPANY_LOGO_H", u"企業ロゴ H", true, false},
};

// a label the table doesn't know, in words: "SEQ_BGM_SOMETHING_NEW" -> "SOMETHING NEW"
std::u16string name_of_label(const std::string& label) {
    std::string s = label.compare(0, 8, "SEQ_BGM_") == 0 ? label.substr(8) : label;
    std::u16string n;
    for (char c : s)
        n += (char16_t)(c == '_' ? ' ' : c);
    return n;
}

std::vector<Track> read_catalog() {
    std::vector<Track> out;
    u32 manager = guest::load32(GA_lbl_80882E48);  // sound::SoundManager
    u32 memory_archive = manager ? guest::load32(manager) : 0;
    u32 reader = memory_archive ? guest::load32(memory_archive + 4 + 4) : 0;
    if (!reader) {
        guest::log("soundplayer: no sound archive in memory");
        return out;
    }
    u32 info = guest::load32(reader + 0x28), symb = guest::load32(reader + 0x2C);
    u32 strings = guest::load32(reader + 0x30);
    u32 n_strings = guest::load32(strings);
    auto label_at = [&](u32 index) { return symb + guest::load32(strings + 4 + 4 * index); };
    auto table_entry = [&](u32 table, u32 i) { return info + guest::load32(table + 4 + 8 * i + 4); };

    u32 players = info + guest::load32(info + 0x14);
    u32 bgm_player = ~0u;
    for (u32 i = 0; i < guest::load32(players); i++) {
        u32 name = guest::load32(table_entry(players, i));
        if (name < n_strings && guest::read_string(label_at(name), 32) == "PLAYER_BGM")
            bgm_player = i;
    }

    struct Found {
        u32 id, label;
        bool music;
    };
    std::map<std::string, Found> by_label;
    std::set<std::pair<u32, u32>> sequences;  // (file, data offset): a sequence filed twice is listed once
    u32 sounds = info + guest::load32(info + 4);
    u32 n_sounds = guest::load32(sounds);
    for (u32 id = 0; id < n_sounds; id++) {
        u32 e = table_entry(sounds, id);
        u32 label_index = guest::load32(e);
        if (label_index >= n_strings)
            continue;
        u32 label = label_at(label_index);
        std::string text = guest::read_string(label, 64);
        bool music = guest::load8(e + 0x16) == SOUND_SEQ && guest::load32(e + 8) == bgm_player &&
                     guest::load8(e + 0x14) > 0 && text.compare(0, 8, "SEQ_BGM_") == 0;
        if (music && !sequences.insert({guest::load32(e + 4), guest::load32(info + guest::load32(e + 0x1C))}).second)
            continue;
        by_label[text] = {id, label, music};
    }

    for (const Named& n : NAMES) {
        auto it = by_label.find(n.label);
        if (it == by_label.end() || (!n.jingle && !it->second.music))
            continue;
        out.push_back({it->second.id, it->second.label, n.name, n.jingle, n.loops});
        by_label.erase(it);
    }
    // music the table doesn't name (none in this archive) goes before the jingles
    std::vector<Track> more;
    for (const auto& kv : by_label)
        if (kv.second.music)
            more.push_back({kv.second.id, kv.second.label, name_of_label(kv.first), false, true});
    std::sort(more.begin(), more.end(), [](const Track& a, const Track& b) { return a.sound_id < b.sound_id; });
    auto first_jingle = std::find_if(out.begin(), out.end(), [](const Track& t) { return t.jingle; });
    out.insert(first_jingle, more.begin(), more.end());
    u32 n_music = (u32)std::count_if(out.begin(), out.end(), [](const Track& t) { return !t.jingle; });
    guest::log("soundplayer: %u sounds in the archive: %u pieces of music, %u jingles", n_sounds, n_music,
               (u32)out.size() - n_music);
    return out;
}

}  // namespace

const std::vector<Track>& catalog() {
    static std::vector<Track> tracks = read_catalog();
    return tracks;
}

}  // namespace engine::soundplayer
