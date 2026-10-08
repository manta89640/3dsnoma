// The Sound Player's tracks (see catalog.h).
//
// The sound archive as nw4r::snd keeps it (sound::SoundManager at sound_SoundManager_sInstance: +0 its
// MemoryArchive, whose +4 is the nw4r::snd::MemorySoundArchive; that one's +4 the
// SoundArchiveFileReader): the reader's +0x28 is the INFO block (past its 8-byte header), +0x2C the
// SYMB block, +0x30 SYMB's string table {u32 count, u32 offsets from SYMB}. INFO starts with
// DataRefs {u8 type, u8, u16, u32 offset from INFO} to its tables, each {u32 count, DataRef[count]}:
// +0x00 sounds, +0x10 players. A sound (SoundCommonInfo): +0x00 label (string index), +0x04 file,
// +0x08 player, +0x14 u8 volume, +0x16 u8 type (1 sequence, 2 stream, 3 wave), +0x18 DataRef to its
// type's info (a sequence's: +0 offset of its data in the file). A player: +0x00 name (string index).
#include "engine/soundplayer/catalog.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <set>

#include "engine/wuhu/music.h"
#include "noma_decomp.h"

namespace engine::soundplayer {

namespace {

enum : u8 { SOUND_SEQ = 1 };

// 「広い世界へ」, the island's music, is the port's own (engine/wuhu/music.cpp): no sound of the
// archive, it plays for its own sound ID. It is listed under its sequence's own label.
const char ISLAND_LABEL[] = "SEQ_BGM_HIROI_SEKAI";

// The channel's labels in words, in the list's order: the room's own music first, then the other
// rooms and screens, the seasonal slideshows, the shop; the jingles last.
struct Named {
    const char* label;
    const char16_t* name;
    bool jingle;
};
const Named NAMES[] = {
    // the room: its theme (one on odd days of the month, one on even days), meals, visitors,
    // birthdays (sung once)
    {"SEQ_BGM_MAIN_01", u"Wiiの間のテーマ 1", false},
    {"SEQ_BGM_MAIN_02", u"Wiiの間のテーマ 2", false},
    {"SEQ_BGM_MEALS", u"ごはんの時間", false},
    {"SEQ_BGM_CONCIERGE", u"コンシェルジュ", false},
    {"SEQ_BGM_CONCIERGE_WAITING", u"コンシェルジュ 2", false},
    {"SEQ_BGM_BIRTHDAY", u"おたんじょうび", false},
    // the start and the settings
    {"SEQ_BGM_BOOT", u"タイトル", false},
    {"SEQ_BGM_INI_SET", u"はじめの設定", false},
    // the bar's other places
    {"SEQ_BGM_THEATERROOM", u"ホームシアター", false},
    {"SEQ_BGM_TELEVISION_NEW", u"番組を見る", false},
    {"SEQ_BGM_MARCH", u"いろんな間", false},
    {"SEQ_BGM_COMPANY", u"企業の間 A", false},
    {"SEQ_BGM_COMPANY_B", u"企業の間 B", false},
    {"SEQ_BGM_COMPANY_C", u"企業の間 C", false},
    {"SEQ_BGM_COMPANY_D", u"企業の間 D", false},
    {"SEQ_BGM_COMPANY_E", u"企業の間 E", false},
    {"SEQ_BGM_COMPANY_F", u"企業の間 F", false},
    {"SEQ_BGM_COMPANY_G", u"企業の間 G", false},
    {"SEQ_BGM_COMPANY_H", u"企業の間 H", false},
    {"SEQ_BGM_VOTE", u"みんなで投票", false},
    {"SEQ_BGM_DSI_DELIVERY_NEW", u"DSi配信", false},
    // the island (出発): the port's own piece, not in the archive (see read_catalog)
    {ISLAND_LABEL, u"広い世界へ", false},
    // the calendar's seasonal slideshows (歳時記), in the order the slideshow numbers them
    {"SEQ_BGM_SAIZIKI_A", u"歳時記 1", false},
    {"SEQ_BGM_SAIZIKI_B", u"歳時記 2", false},
    {"SEQ_BGM_SAIZIKI_C", u"歳時記 3", false},
    {"SEQ_BGM_SAIZIKI_D", u"歳時記 4", false},
    {"SEQ_BGM_SAIZIKI_E", u"歳時記 5", false},
    {"SEQ_BGM_SAIZIKI_F", u"歳時記 6", false},
    {"SEQ_BGM_SAIZIKI_G", u"歳時記 7", false},
    {"SEQ_BGM_SAIZIKI_H", u"歳時記 8", false},
    {"SEQ_BGM_SAIZIKI_09", u"歳時記 9", false},
    {"SEQ_BGM_SAIZIKI_10", u"歳時記 10", false},
    {"SEQ_BGM_SAIZIKI_11", u"歳時記 11", false},
    {"SEQ_BGM_SAIZIKI_12", u"歳時記 12", false},
    {"SEQ_BGM_SAIZIKI_I", u"歳時記 13", false},
    // the shop (its pages were Flash, from the server)
    {"SEQ_BGM_SHOP_BOOT", u"ショッピング（入店）", false},
    {"SEQ_BGM_SHOP_MAIN", u"ショッピング", false},
    {"SEQ_BGM_SHOP_EXCHANGE", u"ショッピング（交換）", false},
    {"SEQ_BGM_SHOP_RETURN", u"ショッピング（おかえり）", false},
    {"SEQ_BGM_SHOP_MISC1", u"ショッピング 1", false},
    {"SEQ_BGM_SHOP_MISC2", u"ショッピング 2", false},
    {"SEQ_BGM_SHOP_MISC3", u"ショッピング 3", false},
    {"SEQ_BGM_SHOP_MISC4", u"ショッピング 4", false},
    {"SEQ_BGM_SHOP_MISC5", u"ショッピング 5", false},
    // jingles: the Miis' feelings (messages), a Mii playing a DSi, the 5 pm chime ("Going Home"),
    // the company logos of いろんな間
    {"SEQ_SE_FEELING_GREETING", u"あいさつ", true},
    {"SEQ_SE_FEELING_CONGRATULATION", u"おめでとう", true},
    {"SEQ_SE_FEELING_EXCITEMENT", u"わくわく", true},
    {"SEQ_SE_FEELING_CALM", u"おだやか", true},
    {"SEQ_SE_FEELING_APOLOGIZE", u"ごめんなさい", true},
    {"SEQ_SE_FEELING_SADNESS", u"かなしい", true},
    {"SEQ_SE_PLAY_GAME_DSI", u"DSiであそぶ", true},
    {"SEQ_SE_PLAY_GAME_DSI_02_LOOP", u"DSiであそぶ 2", true},
    {"SEQ_SE_012_JIHOU_1700", u"夕方5時のチャイム", true},
    {"WAV_SE_123_COMPANY_LOGO", u"企業ロゴ A", true},
    {"WAV_SE_124_COMPANY_LOGO", u"企業ロゴ B", true},
    {"WAV_SE_125_COMPANY_LOGO", u"企業ロゴ C", true},
    {"SEQ_SE_126_COMPANY_LOGO_D", u"企業ロゴ D", true},
    {"SEQ_SE_127_COMPANY_LOGO_E", u"企業ロゴ E", true},
    {"SEQ_SE_128_COMPANY_LOGO_F", u"企業ロゴ F", true},
    {"SEQ_SE_129_COMPANY_LOGO_G", u"企業ロゴ G", true},
    {"SEQ_SE_130_COMPANY_LOGO_H", u"企業ロゴ H", true},
};

// the channel's jingles' heading in the list
const char16_t* const JINGLES = u"ジングル";

// the staged games' list (tools/music/stage_music.py), under the data directory
const char* const SPORTS_LIST = "/music/sports.tsv";
const char* const SPORTS_DIR = "/music/sports/";

// a label the table doesn't know, in words: "SEQ_BGM_SOMETHING_NEW" -> "SOMETHING NEW"
std::u16string name_of_label(const std::string& label) {
    std::string s = label.compare(0, 8, "SEQ_BGM_") == 0 ? label.substr(8) : label;
    std::u16string n;
    for (char c : s)
        n += (char16_t)(c == '_' ? ' ' : c);
    return n;
}

std::u16string from_utf8(const std::string& s) {
    std::u16string out;
    for (size_t i = 0; i < s.size();) {
        u8 c = (u8)s[i];
        u32 cp, n;
        if (c < 0x80)
            cp = c, n = 1;
        else if ((c & 0xE0) == 0xC0)
            cp = c & 0x1F, n = 2;
        else if ((c & 0xF0) == 0xE0)
            cp = c & 0x0F, n = 3;
        else
            cp = c & 0x07, n = 4;
        for (u32 k = 1; k < n && i + k < s.size(); k++)
            cp = cp << 6 | ((u8)s[i + k] & 0x3F);
        i += n;
        if (cp < 0x10000)
            out += (char16_t)cp;
    }
    return out;
}

// The games' music, from DATA/music/sports.tsv: a line a heading ("#\tHEADING") or a piece
// ("FILE\tFLAGS\tNAME", FLAGS "j" for a jingle), in the list's order. Nothing if not staged.
void read_streams(std::vector<Track>& out) {
    std::string dir = guest::data_dir();
    FILE* f = std::fopen((dir + SPORTS_LIST).c_str(), "rb");
    if (!f)
        return;
    std::string text;
    char buf[4096];
    size_t got;
    while ((got = std::fread(buf, 1, sizeof(buf), f)) > 0)
        text.append(buf, got);
    std::fclose(f);
    std::u16string heading;
    u32 n = 0;
    for (size_t at = 0; at < text.size();) {
        size_t end = text.find('\n', at);
        std::string line = text.substr(at, end == std::string::npos ? std::string::npos : end - at);
        at = end == std::string::npos ? text.size() : end + 1;
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        size_t t1 = line.find('\t');
        if (t1 == std::string::npos)
            continue;
        if (line.compare(0, t1, "#") == 0) {
            heading = from_utf8(line.substr(t1 + 1));
            continue;
        }
        size_t t2 = line.find('\t', t1 + 1);
        if (t2 == std::string::npos)
            continue;
        bool jingle = line.compare(t1 + 1, t2 - t1 - 1, "j") == 0;
        out.push_back({STREAM_ID + n++, from_utf8(line.substr(t2 + 1)), jingle, heading,
                       dir + SPORTS_DIR + line.substr(0, t1)});
    }
    guest::log("soundplayer: %u pieces of Wii Sports and Wii Sports Resort (%s)", n, SPORTS_LIST);
}

std::vector<Track> read_catalog() {
    std::vector<Track> out;
    u32 manager = guest::load32(GA_sound_SoundManager_sInstance);  // sound::SoundManager
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
        u32 id;
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
        by_label[text] = {id, music};
    }
    by_label[ISLAND_LABEL] = {engine::wuhu::WIDE_WORLD_ID, true};

    for (const Named& n : NAMES) {
        auto it = by_label.find(n.label);
        if (it == by_label.end() || (!n.jingle && !it->second.music))
            continue;
        out.push_back({it->second.id, n.name, n.jingle, n.jingle ? JINGLES : u"", {}});
        by_label.erase(it);
    }
    // music the table doesn't name (none in this archive) goes before the jingles
    std::vector<Track> more;
    for (const auto& kv : by_label)
        if (kv.second.music)
            more.push_back({kv.second.id, name_of_label(kv.first), false, u"", {}});
    std::sort(more.begin(), more.end(), [](const Track& a, const Track& b) { return a.sound_id < b.sound_id; });
    auto first_jingle = std::find_if(out.begin(), out.end(), [](const Track& t) { return t.jingle; });
    out.insert(first_jingle, more.begin(), more.end());
    u32 n_music = (u32)std::count_if(out.begin(), out.end(), [](const Track& t) { return !t.jingle; });
    guest::log("soundplayer: %u sounds in the archive: %u pieces of music, %u jingles", n_sounds, n_music,
               (u32)out.size() - n_music);
    read_streams(out);
    return out;
}

}  // namespace

const std::vector<Track>& catalog() {
    static std::vector<Track> tracks = read_catalog();
    return tracks;
}

const Track* track_of(u32 sound_id) {
    for (const Track& t : catalog())
        if (t.sound_id == sound_id)
            return &t;
    return nullptr;
}

}  // namespace engine::soundplayer
