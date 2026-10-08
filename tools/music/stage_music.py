#!/usr/bin/env python3
"""Stage Wii Sports' and Wii Sports Resort's music for the Sound Player: DATA/music/.

    tools/music/stage_music.py DATA [--extract ROOT] [--enc TOOL] [--jobs N] [--only LABEL...] [--list]

ROOT is your extraction of the Wii Sports + Wii Sports Resort disc (default ../wii-sports-extract
next to this repository: launcher/, wii-sports/, wii-sports-resort/ as tools/... split it; files/
under each is that game's root). DATA is the port's data directory (on the 3DS sdmc:/3ds/noma).
Nothing of the disc goes anywhere else, and nothing this makes belongs in the repository.

What it makes:
  DATA/music/sports/NAME.nstm   a piece each (engine/audio/nstm.h: 32 kHz DSP-ADPCM, its loop, its
                                tempo for the Sound Player's dances), encoded by TOOL (default
                                build-host/nstm-enc, tools/nstm_enc.cpp)
  DATA/music/sports.tsv         the list (engine/soundplayer/catalog.cpp reads it): a line a
                                heading ("#\tHEADING") or a piece ("FILE\tFLAGS\tNAME"; FLAGS j: a
                                jingle), in the Sound Player's order

What goes in: every piece of music of the two games and of the disc's menu --
  - streams (BRSTM): every file in the games' stream folders (the sports' own, Resort's one), the
    tracks a sound plays mixed down to stereo at the stream's track volumes (Resort's title and
    menu themes and the cycling race have 2 and 3 tracks: all of them play);
  - waves (RWSD in an archive): the fanfares and jingles (on the BGM and fanfare players, the
    "FANFARE_" sounds, Resort's SE_CMN_JINGLE_ ones, the MotionPlus video's menu music);
  - sequences (RSEQ): Resort's two (Swordplay's showdown, Cycling's race), played through the
    model of nw4r's sequencer and sampler in brsar.py / sndrender.py (close, not exact: no AX
    reverb), and its two mini fanfares.
Pieces whose samples are the same (the training results every Wii Sports sport has a copy of) are
listed once. Every piece is brought to the same loudness (the RMS of its loud parts at LOUDNESS_DB
dBFS, its peak kept below full scale), resampled to 32 kHz where it isn't, and gets a tempo and
beat phase from its onsets (an autocorrelation; the sequences' own tempo).
"""
import argparse
import hashlib
import os
import struct
import subprocess
import sys
from concurrent.futures import ProcessPoolExecutor

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import brsar  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DEFAULT_EXTRACT = os.path.join(os.path.dirname(ROOT), 'wii-sports-extract')
RATE = 32000
LOUDNESS_DB = -18.0   # the loud parts' RMS (dBFS): about the channel's own music, with room for its sounds
PEAK = 0.9

be_u8 = lambda d, o: d[o]
be_u16 = brsar.be_u16
be_u32 = brsar.be_u32
be_s16 = brsar.be_s16


# ---------------------------------------------------------------------------- the list

# (game, label) -> name; a piece's label is its sound's (or, for a stream no sound plays, its file's
# name). The list's order is this table's: by game, then where the game plays them; jingles after
# each game's music. Names in Japanese, as the channel's own (catalog.cpp).
WS = 'Wii Sports'
WSR = 'Wii Sports Resort'
LIST = [
    # ---- Wii Sports
    ('#', WS),
    (WS, 'BGM_LAUNCHER', 'ソフトの選択'),
    (WS, 'STRM_SportsPackTittle', 'タイトル'),
    (WS, 'STRM_SportsPackSelect', 'スポーツの選択'),
    (WS, 'BGM_SelectPosition@Tns', 'テニス：ポジション選択'),
    (WS, 'BGM_GameSetReplay@Tns', 'テニス：リプレイ'),
    (WS, 'BGM_GameResultDisp@Tns', 'テニス：結果発表'),
    (WS, 'BGM_GameResultDisp@Bsb', '野球：結果発表'),
    (WS, 'BGM_Env@Bow', 'ボウリング：ボウリング場'),
    (WS, 'BGM_GameScoreDisp@Bow', 'ボウリング：スコア'),
    (WS, 'BGM_CourseSelect@Gol', 'ゴルフ：コース選択'),
    (WS, 'BGM_CoursePresentationDemo@Gol', 'ゴルフ：コース紹介'),
    (WS, 'BGM_GameResultDisp@Gol', 'ゴルフ：結果発表'),
    (WS, 'BGM_GameSetReplay@Box', 'ボクシング：リプレイ'),
    (WS, 'BGM_GameResultDisp@Box', 'ボクシング：結果発表'),
    (WS, 'Bgm_Tutorial', '操作説明'),
    (WS, 'STRM_Training_Select', 'トレーニング：種目選択'),
    (WS, 'BGM_Training_Kabeuchi@Tns', 'トレーニング：テニス'),
    (WS, 'BGM_Training_Nerauchii@Tns', 'トレーニング：テニス 2'),
    (WS, 'BGM_Training_Renzoku@Tns', 'トレーニング：テニス 3'),
    (WS, 'BGM_Training_Homerun@Bsb', 'トレーニング：野球'),
    (WS, 'BGM_Training_Renzoku@Bsb', 'トレーニング：野球 2'),
    (WS, 'BGM_Training_Uchiwake@Bsb', 'トレーニング：野球 3'),
    (WS, 'BGM_Training_Spareget@Bow', 'トレーニング：ボウリング'),
    (WS, 'BGM_Training_Nagitaoshi@Bow', 'トレーニング：ボウリング 2'),
    (WS, 'BGM_Training_Gatetoshi@Bow', 'トレーニング：ボウリング 3'),
    (WS, 'BGM_Training_Nearpin@Gol', 'トレーニング：ゴルフ'),
    (WS, 'BGM_Training_Onepat@Gol', 'トレーニング：ゴルフ 2'),
    (WS, 'BGM_Training_Target@Gol', 'トレーニング：ゴルフ 3'),
    (WS, 'BGM_Training_Sandbag@Box', 'トレーニング：ボクシング'),
    (WS, 'BGM_Training_Mituchi@Box', 'トレーニング：ボクシング 2'),
    (WS, 'BGM_Training_Tamayoke@Box', 'トレーニング：ボクシング 3'),
    (WS, 'BGM_TrainingResultDisp@Tns', 'トレーニング：結果発表'),
    (WS, 'BGM_TrainingResultDisp@Bow', 'トレーニング：結果発表 2'),
    (WS, 'BGM_TrainingResultDisp@Gol', 'トレーニング：結果発表 3'),
    (WS, 'BGM_TrainingResultDisp@Bsb', 'トレーニング：結果発表（野球）'),
    (WS, 'STRM_Training_Result01', 'トレーニング：メダル 1'),
    (WS, 'STRM_Training_Result02', 'トレーニング：メダル 2'),
    (WS, 'STRM_Training_Result03', 'トレーニング：メダル 3'),
    (WS, 'STRM_Measurement_Start', '体力測定'),
    (WS, 'STRM_Measurement_Tns', '体力測定：テニス'),
    (WS, 'STRM_Measurement_Bsb', '体力測定：野球'),
    (WS, 'STRM_Measurement_Gol', '体力測定：ゴルフ'),
    (WS, 'STRM_Measurement_Bow', '体力測定：ボウリング'),
    (WS, 'STRM_Measurement_Box', '体力測定：ボクシング'),
    (WS, 'STRM_SportsPack_PhysicalMeasure_bridge', '体力測定：つなぎ'),
    (WS, 'STRM_Measurement_Result', '体力測定：結果発表'),
    ('#', WS + ' ジングル'),
    (WS, 'Fanfare_Set@Tns', 'テニス：セット'),
    (WS, 'FANFARE_GameSet', 'ゲームセット（勝ち）'),
    (WS, 'FANFARE_GameSet_Win', 'ゲームセット（勝ち）'),
    (WS, 'FANFARE_GameSet_Lose', 'ゲームセット（負け）'),
    (WS, 'FANFARE_GameSet_Tie', 'ゲームセット（引き分け）'),
    (WS, 'FANFARE_Form_A_Line@Bsb', '野球：整列'),
    (WS, 'FANFARE_VS@Bsb', '野球：対戦'),
    (WS, 'FANFARE_SingleHit@Bsb', '野球：ヒット'),
    (WS, 'FANFARE_TwoBaseHit@Bsb', '野球：ツーベース'),
    (WS, 'FANFARE_ThreeBaseHit@Bsb', '野球：スリーベース'),
    (WS, 'FANFARE_HomeRun@Bsb', '野球：ホームラン'),
    (WS, 'FANFARE_OutOfTheParkHomer@Bsb', '野球：場外ホームラン'),
    (WS, 'FANFARE_Strike@Bow', 'ボウリング：ストライク'),
    (WS, 'FANFARE_SpareGet@Bow', 'ボウリング：スペア'),
    (WS, 'BGM_CoursePresentationDemo_WS@Bow', 'ボウリング：はじまり'),
    (WS, 'BGM_Close@Bow', 'ボウリング：おわり'),
    (WS, 'FANFARE_Training_SpareGet@Bow', 'トレーニング：ボウリング'),
    (WS, 'FANFARE_Training_Nagitaoshi@Bow', 'トレーニング：ピンたおし'),
    (WS, 'FANFARE_Training_Kabeyoke@Bow', 'トレーニング：カーブ'),
    (WS, 'FANFARE_HoleInOne@Gol', 'ゴルフ：スコア'),
    (WS, 'FANFARE_Albatross@Gol', 'ゴルフ：アルバトロス'),
    (WS, 'FANFARE_Eagle@Gol', 'ゴルフ：イーグル'),
    (WS, 'FANFARE_Birdie@Gol', 'ゴルフ：バーディ'),
    (WS, 'FANFARE_Par@Gol', 'ゴルフ：パー'),
    (WS, 'FANFARE_Bogey@Gol', 'ゴルフ：ボギー'),
    (WS, 'FANFARE_DoubleBogey@Gol', 'ゴルフ：ダブルボギー'),
    (WS, 'FANFARE_BallStop@Gol', 'ゴルフ：ボールが止まる'),
    (WS, 'FANFARE_TR_Training_Target@Gol', 'トレーニング：ゴルフ（ねらい打ち）'),
    (WS, 'FANFARE_Training_Target_PointGet_2D_0@Gol', 'トレーニング：ゴルフ（0点）'),
    (WS, 'FANFARE_Training_Target_PointGet_2D_5@Gol', 'トレーニング：ゴルフ 5'),
    (WS, 'FANFARE_Training_Target_PointGet_2D_25@Gol', 'トレーニング：ゴルフ 25'),
    (WS, 'FANFARE_Training_Target_PointGet_2D_50@Gol', 'トレーニング：ゴルフ 50'),
    (WS, 'FANFARE_Training_Target_PointGet_2D_75@Gol', 'トレーニング：ゴルフ 75'),
    (WS, 'FANFARE_Training_Target_PointGet_2D_100@Gol', 'トレーニング：ゴルフ 100'),
    (WS, 'Fanfare_VS@Box', 'ボクシング：対戦'),
    (WS, 'Fanfare_Round@Box', 'ボクシング：ラウンド'),
    (WS, 'Fanfare_Judgment@Box', 'ボクシング：判定'),
    # ---- Wii Sports Resort
    ('#', WSR),
    (WSR, 'STM_OMK_GATHER_OPENING', 'オープニング'),
    (WSR, 'STM_TITLE_FIRST', 'タイトル（はじめて）'),
    (WSR, 'STM_TITLE', 'タイトル'),
    (WSR, 'STM_TITLE_RET', 'タイトル（おかえり）'),
    (WSR, 'wisp2_title', 'タイトル（ひとつのトラック）'),
    (WSR, 'STM_TITLE_BASE', 'タイトル（ひとつのトラック・おかえり）'),
    (WSR, 'STM_SELECT_01', 'スポーツの選択'),
    (WSR, 'STM_CMN_TUTORIAL', '操作説明'),
    (WSR, 'STM_PRE_VS', '対戦のまえ'),
    (WSR, 'STM_VS_COMMON', '対戦'),
    (WSR, 'STM_VS_COMMON_CHAMP', '対戦（チャンピオン）'),
    (WSR, 'STM_OMK_IN_AIRPLANE_BGM', 'スカイダイビング：機内のラジオ'),
    (WSR, 'STM_OMK_END_DEMO', 'スカイダイビング：着地'),
    (WSR, 'STM_OMK_RESULT', 'スカイダイビング：結果発表'),
    (WSR, 'STM_SWF_VS', 'チャンバラ：対戦'),
    (WSR, 'STM_SWF_VS_CHAMP', 'チャンバラ：対戦（チャンピオン）'),
    (WSR, 'STM_SWF_DRUM_01', 'チャンバラ：太鼓'),
    (WSR, 'STM_SWF_DRUM_02', 'チャンバラ：太鼓 2'),
    (WSR, 'STM_SWF_IAI_DRUM_01', 'チャンバラ：居合斬りの太鼓'),
    (WSR, 'STM_SWF_IAI_DRUM_02', 'チャンバラ：居合斬りの太鼓 2'),
    (WSR, 'BGM_SWF_KUMITE', 'チャンバラ：連続斬り'),
    (WSR, 'STM_SWF_MATCH_REPLAY_1', 'チャンバラ：リプレイ（勝ち）'),
    (WSR, 'STM_SWF_MATCH_REPLAY_2', 'チャンバラ：リプレイ（負け）'),
    (WSR, 'STM_SWF_RESULT_WIN', 'チャンバラ：結果発表（勝ち）'),
    (WSR, 'STM_SWF_RESULT_WIN_VS', 'チャンバラ：結果発表（対戦の勝ち）'),
    (WSR, 'STM_SWF_RESULT_DRAW', 'チャンバラ：結果発表（引き分け）'),
    (WSR, 'STM_SWF_RESULT_LOSE', 'チャンバラ：結果発表（負け）'),
    (WSR, 'STM_WKB_OPENING', 'ウェイクボード：はじまり'),
    (WSR, 'STM_WKB_PLAYER_CHANGE', 'ウェイクボード：交代'),
    (WSR, 'STM_WKB_RESULT_SUCCESS', 'ウェイクボード：結果発表'),
    (WSR, 'STM_FLD_OPENING', 'フリスビー：はじまり'),
    (WSR, 'STM_FLD_PLAYER_CHANGE', 'フリスビー：交代'),
    (WSR, 'STM_FLD_ONE_PLAYER_END', 'フリスビー：おわり'),
    (WSR, 'STM_FLD_RESULT', 'フリスビー：結果発表'),
    (WSR, 'Fld_Jsk_result_win', 'フリスビー：結果発表（勝ち）'),
    (WSR, 'STM_ARC_OPENING', 'アーチェリー：はじまり'),
    (WSR, 'STM_ARC_PLAYER_CHANGE', 'アーチェリー：交代'),
    (WSR, 'Arc_stage_normal', 'アーチェリー：ステージ'),
    (WSR, 'STM_ARC_NEXT_STAGE', 'アーチェリー：次のステージ'),
    (WSR, 'STM_ARC_NEXT_STAGE_2', 'アーチェリー：次のステージ 2'),
    (WSR, 'STM_ARC_NEXT_STAGE_LAST', 'アーチェリー：最後のステージ'),
    (WSR, 'STM_ARC_ONE_PLAYER_END', 'アーチェリー：おわり'),
    (WSR, 'STM_ARC_RESULT', 'アーチェリー：結果発表'),
    (WSR, 'STM_BSK_3PT_OPENING', 'バスケットボール：はじまり'),
    (WSR, 'STM_BSK_3PT_PLAYER_CHANGE', 'バスケットボール：交代'),
    (WSR, 'STM_BSK_3PT_RESULT', 'バスケットボール：結果発表'),
    (WSR, 'STM_BSK_RESULT_DRAW', 'バスケットボール：結果発表（引き分け）'),
    (WSR, 'STM_BSK_RESULT_LOSE', 'バスケットボール：結果発表（負け）'),
    (WSR, 'STM_BSK_REPLAY', 'バスケットボール：リプレイ'),
    (WSR, 'STM_BSK_REPLAY_DUNK', 'バスケットボール：リプレイ（ダンク）'),
    (WSR, 'STM_BSK_REPLAY_LAST', 'バスケットボール：リプレイ（最後）'),
    (WSR, 'STM_PNG_RALLY_OPENING', '卓球：はじまり'),
    (WSR, 'STM_PNG_RALLY_PLAYER_CHANGE', '卓球：交代'),
    (WSR, 'STM_PNG_SCORE_P1', '卓球：リプレイ'),
    (WSR, 'STM_PNG_SCORE_P2', '卓球：リプレイ 2'),
    (WSR, 'Png_replay_adv1', '卓球：リプレイ（アドバンテージ）'),
    (WSR, 'Png_replay_adv2', '卓球：リプレイ（アドバンテージ 2）'),
    (WSR, 'STM_PNG_SCORE_P1_DEUCE', '卓球：リプレイ（デュース）'),
    (WSR, 'STM_PNG_SCORE_P2_DEUCE', '卓球：リプレイ（デュース 2）'),
    (WSR, 'STM_PNG_SCORE_P1_LAST', '卓球：リプレイ（最後）'),
    (WSR, 'STM_PNG_SCORE_P2_LAST', '卓球：リプレイ（最後 2）'),
    (WSR, 'STM_PNG_RESULT_WIN', '卓球：結果発表（勝ち）'),
    (WSR, 'STM_PNG_RESULT_LOSE', '卓球：結果発表（負け）'),
    (WSR, 'STM_GLF_OPENING', 'ゴルフ：はじまり'),
    (WSR, 'STM_GLF_REPLAY', 'ゴルフ：リプレイ'),
    (WSR, 'STM_GLF_RESULT', 'ゴルフ：結果発表'),
    (WSR, 'STM_BWL_OPENING', 'ボウリング：はじまり'),
    (WSR, 'STM_BWL_ALLEY_MUSIC', 'ボウリング：ボウリング場'),
    (WSR, 'STM_BWL_ALLEY_MUSIC_100PIN', 'ボウリング：100ピン'),
    (WSR, 'STM_BWL_ALLEY_MUSIC_WALL', 'ボウリング：カベありスペア'),
    (WSR, 'STM_BWL_RESULT', 'ボウリング：結果発表'),
    (WSR, 'STM_JSK_OPENING', '水上バイク：はじまり'),
    (WSR, 'STM_JSK_PLAYER_CHANGE', '水上バイク：交代'),
    (WSR, 'STM_JSK_RESULT', '水上バイク：結果発表'),
    (WSR, 'STM_CAN_OPENING', 'カヌー：はじまり'),
    (WSR, 'STM_CAN_RESULT_SUCCESS', 'カヌー：結果発表'),
    (WSR, 'STM_CAN_RESULT_FAILURE', 'カヌー：結果発表（失敗）'),
    (WSR, 'STM_BIC_OPENING', '自転車：はじまり'),
    (WSR, 'STM_BIC_VS_OPENING', '自転車：対戦のはじまり'),
    (WSR, 'BGM_BIC_MAIN', '自転車：レース'),
    (WSR, 'STM_BIC_VS_MAIN', '自転車：対戦レース'),
    (WSR, 'STM_BIC_RESULT_1', '自転車：結果発表'),
    (WSR, 'STM_BIC_RESULT_2', '自転車：結果発表 2'),
    (WSR, 'STM_PLN_OPENING', 'スカイスポーツ：はじまり'),
    (WSR, 'STM_PLN_MARCH_2', 'スカイスポーツ：船のマーチ'),
    (WSR, 'STM_PLN_SHIP_NIGHT', 'スカイスポーツ：夜の船'),
    (WSR, 'STM_PLN_CASTLE_WALTZ_1', 'スカイスポーツ：お城のワルツ'),
    (WSR, 'STM_PLN_CASTLE_NIGHT', 'スカイスポーツ：夜のお城'),
    (WSR, 'STM_PLN_COTTAGE_1', 'スカイスポーツ：コテージ'),
    (WSR, 'STM_PLN_COTTAGE_2', 'スカイスポーツ：コテージ 2'),
    (WSR, 'STM_PLN_RESULT_FREE', 'スカイスポーツ：結果発表'),
    (WSR, 'STM_PLN_RESULT_VS', 'スカイスポーツ：結果発表（対戦）'),
    (WSR, 'MPM_BGM_MENU', 'Wii モーションプラスの紹介'),
    ('#', WSR + ' ジングル'),
    (WSR, 'SE_CMN_JINGLE_INFORMATION', 'お知らせ'),
    (WSR, 'SE_CMN_JINGLE_NEW_RECORD', '新記録'),
    (WSR, 'SE_CMN_JINGLE_SUPER_PLAY', 'スーパープレイ'),
    (WSR, 'SE_CMN_JINGLE_COMPLETE', 'コンプリート'),
    (WSR, 'SE_CMN_JINGLE_BRONZE_CLASS', 'ブロンズ・シルバークラス'),
    (WSR, 'SE_CMN_JINGLE_SILVER_CLASS', 'シルバークラス'),
    (WSR, 'SE_CMN_JINGLE_GOLD_CLASS', 'ゴールド・プラチナクラス'),
    (WSR, 'SE_CMN_JINGLE_PLATINUM_CLASS', 'プラチナクラス'),
    (WSR, 'STM_SWF_ROUND', 'チャンバラ：ラウンド'),
    (WSR, 'STM_SWF_RESULT_01', 'チャンバラ：ポイント'),
    (WSR, 'STM_SWF_FINAL_STAGE', 'チャンバラ：最終ステージ'),
    (WSR, 'SE_JSK_MINI_FANFARE', '水上バイク：ファンファーレ'),
    (WSR, 'STM_ARC_STAGE_END_1', 'アーチェリー：ステージクリア'),
    (WSR, 'STM_ARC_STAGE_END_2', 'アーチェリー：ステージクリア 2'),
    (WSR, 'STM_ARC_STAGE_END_3', 'アーチェリー：ステージクリア 3'),
    (WSR, 'SE_PLN_MINI_FANFARE', 'スカイスポーツ：ファンファーレ'),
]

SPORTS = [('Tns', 'テニス'), ('Bsb', '野球'), ('Bow', 'ボウリング'), ('Gol', 'ゴルフ'), ('Box', 'ボクシング')]

# streams and labels that aren't music (silence) or are the same piece as one listed
SKIP = {'STM_SILENT', 'BGM_SILENT', 'STM_BSK_RESULT_WIN'}  # (the last: 3PT_RESULT's file)


# ---------------------------------------------------------------------------- sources

class Piece:
    """A piece found on the disc: where its samples come from."""

    def __init__(self, game, label, kind, **kw):
        self.game, self.label, self.kind = game, label, kind
        self.__dict__.update(kw)


def strm_path(r, brsar_path, s):
    """A stream sound's file: its external path, from the archive's folder."""
    e = r.files[s.file]['info_offset']
    off = be_u32(r.d, e + 0x10)
    if not off:
        return None
    rel = r.d[r.I + off:r.d.index(b'\0', r.I + off)].decode()
    return os.path.normpath(os.path.join(os.path.dirname(brsar_path), rel))


def rwsd_wave(r, s):
    """A wave sound's RWAV (the RWSD's sound s.wave_index: its first note's wave in the RWAR)
    and the note's pitch."""
    d = r.file_data(s.file)
    magic, _, _, blocks = brsar.block_header(d, 0)
    assert magic == 'RWSD', magic
    base = blocks[0][0] + 8
    ref = lambda o: base + be_u32(d, o + 4)
    sound = ref(base + 4 + 8 * s.wave_index)
    notes = ref(sound + 0x10)
    note = ref(notes + 4)
    wave_index = struct.unpack_from('>i', d, note)[0]
    pitch = struct.unpack_from('>f', d, note + 0x10)[0]
    info = ref(sound)
    pitch *= struct.unpack_from('>f', d, info)[0] or 1.0
    if len(blocks) > 1:  # (RWSD 1.2 and before: a WAVE block of wave infos over the raw wave data)
        return WaveInfo(d, blocks[1][0] + be_u32(d, blocks[1][0] + 0xC + 4 * wave_index), r.wave_data(s.file)), pitch
    return brsar.Rwar(r.wave_data(s.file)).wave(wave_index), pitch


class WaveInfo:
    """An older RWSD's wave: its WaveInfo (as an RWAV's INFO) at w in d, its samples in the file's
    wave data (from its data location, each channel's data offset on from there)."""

    def __init__(self, d, w, wave_data):
        self.format, self.loop, self.channels = d[w], d[w + 1], d[w + 2]
        self.rate = (d[w + 3] << 16) | be_u16(d, w + 4)
        loop_start, loop_end, ch_table, location = struct.unpack_from('>IIII', d, w + 8)
        if self.format == 2:
            self.loop_start = brsar.dsp_nibble_to_sample(loop_start)
            self.nsamples = brsar.dsp_nibble_to_sample(loop_end) + 1
        else:
            self.loop_start, self.nsamples = loop_start, loop_end + 1
        self.chans, self.raw, self.coefs = [], [], []
        for c in range(self.channels):
            ci = w + be_u32(d, w + ch_table + 4 * c)
            data_off, adpcm_off = be_u32(d, ci), be_u32(d, ci + 4)
            p = location + data_off
            if self.format == 2:
                a = w + adpcm_off
                frames = wave_data[p:p + (self.nsamples + 13) // 14 * 8]
                coefs = struct.unpack_from('>16h', d, a)
                self.raw.append(frames)
                self.coefs.append(coefs)
                x = brsar.decode_adpcm(frames, coefs, self.nsamples, be_s16(d, a + 0x24), be_s16(d, a + 0x26))
            elif self.format == 1:
                x = np.frombuffer(wave_data[p:p + 2 * self.nsamples], dtype='>i2')
            else:
                x = np.frombuffer(wave_data[p:p + self.nsamples], dtype=np.int8).astype(np.int16) * 256
            self.chans.append(x.astype(np.float32) / 32768.0)

    def pcm(self):
        return np.stack(self.chans)


def find_pieces(extract):
    """Every piece of music on the disc, by (game, label); a label of a game's sports archive
    that more than one has (the training results, the game-set fanfares) the first's, the others
    as LABEL@SPORT (as LIST names them)."""
    pieces = {}
    used_files = set()
    ws = os.path.join(extract, 'wii-sports', 'files', 'Sound')
    wsr = os.path.join(extract, 'wii-sports-resort', 'files')
    archives = [
        (WS, os.path.join(extract, 'launcher', 'files', 'Sound', 'DoublesSnd.brsar'), None),
        (WS, os.path.join(ws, 'RPSportsCommon', 'rp_Common_Sports.brsar'), None),
        (WS, os.path.join(ws, 'RPTnsScene', 'rp_Tns_sound.brsar'), 'Tns'),
        (WS, os.path.join(ws, 'RPBsbScene', 'EN', 'rp_Bsb_sound.brsar'), 'Bsb'),
        (WS, os.path.join(ws, 'RPBowScene', 'rp_Bow_sound.brsar'), 'Bow'),
        (WS, os.path.join(ws, 'RPGolScene', 'EN', 'rp_Gol_sound.brsar'), 'Gol'),
        (WS, os.path.join(ws, 'RPBoxScene', 'rp_Box_sound.brsar'), 'Box'),
        (WSR, os.path.join(wsr, 'Sound', 'Static', 'Sp2Snd.brsar'), None),
        (WSR, os.path.join(wsr, 'sys', 'mpls_movie', 'Sound', 'MotionPlusMovieSound.brsar'), None),
    ]
    for game, path, sport in archives:
        r = brsar.Rsar(path)
        players = {p['id']: p['label'] for p in r.players}
        for s in r.sounds:
            if not s.label or s.label in SKIP:
                continue
            player = players.get(s.player, '')
            bgm = 'BGM' in player or 'FANFARE' in player
            if s.type == 2:
                f = strm_path(r, path, s)
                if not f or not os.path.exists(f):
                    continue
                p = Piece(game, s.label, 'strm', path=f, tracks=s.strm_tracks)
            elif s.type == 3 and (bgm or s.label.startswith('FANFARE_')):
                p = Piece(game, s.label, 'wave', brsar=path, sound=s.id)
            elif s.type == 1 and (('BGM' in player and s.label.startswith('BGM_')) or 'MINI_FANFARE' in s.label):
                p = Piece(game, s.label, 'seq', brsar=path, sound=s.id)
            else:
                continue
            key = (game, s.label if (game, s.label) not in pieces else '%s@%s' % (s.label, sport))
            if sport and (game, '%s@%s' % (s.label, sport)) not in pieces:
                pieces[(game, '%s@%s' % (s.label, sport))] = p
            pieces.setdefault(key, p)
            if p.kind == 'strm':
                used_files.add(os.path.realpath(p.path))
    # streams no sound plays (Resort's alternative takes, its sword fight's entrances): by file name
    for game, folder in ((WSR, os.path.join(wsr, 'Sound', 'Static', 'stream')),):
        for name in sorted(os.listdir(folder)):
            f = os.path.join(folder, name)
            label = name.split('.')[0]
            if not name.endswith('.brstm') or os.path.realpath(f) in used_files or label in SKIP:
                continue
            pieces.setdefault((game, label), Piece(game, label, 'strm', path=f, tracks=None))
    return pieces


# ---------------------------------------------------------------------------- decoding

def read_brstm(path):
    """(float32 [channels, n], rate, loop start or None, tracks [(volume, [channels])], and for
    DSP-ADPCM its frames and coefficients per channel, else None)."""
    d = open(path, 'rb').read()
    assert d[:4] == b'RSTM', path
    head = be_u32(d, 0x10)
    data = be_u32(d, 0x20)
    H = head + 8
    ref = lambda i: H + be_u32(d, H + 8 * i + 4)
    s = ref(0)
    codec, loop, channels = d[s], d[s + 1], d[s + 2]
    rate = be_u16(d, s + 4)
    loop_start, total = be_u32(d, s + 8), be_u32(d, s + 0xC)
    blocks, block_size, block_samples, last_size, last_samples, last_padded = struct.unpack_from('>6I', d, s + 0x14)
    t = ref(1)
    ntracks, ttype = d[t], d[t + 1]
    tracks = []
    for i in range(ntracks):
        o = H + be_u32(d, t + 4 + 8 * i + 4)
        if ttype == 1:
            vol, n, chs = d[o], d[o + 8], list(d[o + 9:o + 9 + d[o + 8]])
        else:
            vol, n, chs = 127, d[o], list(d[o + 1:o + 1 + d[o]])
        tracks.append((vol, chs))
    c = ref(2)
    coefs = []
    for i in range(d[c]):
        ci = H + be_u32(d, c + 4 + 8 * i + 4)
        a = H + be_u32(d, ci + 4)
        coefs.append((struct.unpack_from('>16h', d, a), be_s16(d, a + 0x24), be_s16(d, a + 0x26)))
    start = data + 8 + be_u32(d, data + 8)
    per = [bytearray() for _ in range(channels)]
    o = start
    for b in range(blocks):
        size = last_padded if b == blocks - 1 else block_size
        used = last_size if b == blocks - 1 else block_size
        for ch in range(channels):
            per[ch] += d[o:o + used]
            o += size
    out = np.zeros((channels, total), dtype=np.float32)
    raw = {'frames': [bytes(per[ch][:(total + 13) // 14 * 8]) for ch in range(channels)],
           'coefs': [c[0] for c in coefs]} if codec == 2 else None
    for ch in range(channels):
        if codec == 1:
            x = np.frombuffer(bytes(per[ch][:2 * total]), dtype='>i2')
        elif codec == 0:
            x = np.frombuffer(bytes(per[ch][:total]), dtype=np.int8).astype(np.int16) * 256
        else:
            cf, y1, y2 = coefs[ch]
            x = brsar.decode_adpcm(bytes(per[ch]), cf, total, y1, y2)
        out[ch, :len(x)] = x.astype(np.float32) / 32768.0
    return out, rate, (loop_start if loop else None), tracks, raw


def resample(x, rate):
    """float32 [channels, n] at rate -> 32 kHz (ffmpeg's resampler)."""
    if rate == RATE:
        return x
    ch = x.shape[0]
    raw = np.ascontiguousarray(x.T).astype('<f4').tobytes()
    p = subprocess.run(['ffmpeg', '-v', 'error', '-f', 'f32le', '-ar', str(rate), '-ac', str(ch), '-i', '-',
                        '-ar', str(RATE), '-f', 'f32le', '-'], input=raw, capture_output=True, check=True)
    return np.frombuffer(p.stdout, dtype='<f4').reshape(-1, ch).T.copy()


def decode(p):
    """(float32 [1 or 2, n] at 32 kHz, loop start or None, bpm or None, beats a bar or None, raw):
    raw, for a piece the disc has as DSP-ADPCM at 32 kHz in one or two channels that play as they
    are, is {'frames': [bytes a channel], 'coefs': [16 a channel]}: its own frames, which the NSTM
    keeps (y is then exactly their samples)."""
    if p.kind == 'strm':
        x, rate, loop, tracks, raw = read_brstm(p.path)
        on = [t for i, t in enumerate(tracks) if p.tracks is None or p.tracks & (1 << i)] or tracks[:1]
        if len(on) == 1 and len(on[0][1]) <= 2 and on[0][0] == 127 and rate == RATE and raw:
            chs = on[0][1]
            return (x[chs], loop, None, None,
                    {'frames': [raw['frames'][c] for c in chs], 'coefs': [raw['coefs'][c] for c in chs]})
        if all(len(chs) == 1 for _, chs in on) and len(on) == 1:
            y = x[on[0][1]] * (on[0][0] / 127.0)
        else:
            y = np.zeros((2, x.shape[1]), dtype=np.float32)
            for vol, chs in on:
                g = vol / 127.0
                y[0] += x[chs[0]] * g
                y[1] += x[chs[-1]] * g
        y = resample(y, rate)
        return y, (int(round(loop * RATE / rate)) if loop is not None else None), None, None, None
    r = brsar.Rsar(p.brsar)
    s = r.sounds[p.sound]
    if p.kind == 'wave':
        w, pitch = rwsd_wave(r, s)
        x = w.pcm()
        loop = w.loop_start if w.loop else None
        rate = int(round(w.rate * pitch))
        raw = wave_adpcm(w) if rate == RATE and w.channels <= 2 else None
        return resample(x, rate), (int(round(loop * RATE / rate)) if loop is not None else None), None, None, raw
    # a sequence: twice through its loop, so that the loop's seam has the notes ringing on from
    # its end (the second pass starts with the first's tails); the loop is the second pass
    import sndrender
    sq = r.sequence(s.file)
    sim = brsar.simulate(sq.mml, s.seq_offset, loops=2, alloc_mask=s.alloc_track)
    jumps = sorted(t for lst in sim['loop_tick'].values() for t, _ in lst)
    tsec = brsar.tick_to_seconds(sim['tempo'], sim['timebase'])
    audio = sndrender.render(sim, r.bank(s.bank), reverb=True, volume=s.volume / 127.0, gain_db=None,
                             tail_s=0.0 if jumps else 2.0)
    bpm = sim['tempo'][-1][1] if sim['tempo'] else None
    if len(jumps) >= 2:
        first, second = jumps[0], next(t for t in jumps if t > jumps[0])
        end = int(round(tsec(second) * RATE))
        loop = int(round(tsec(first) * RATE))
        return audio[:, :end], loop, bpm, None, None
    return audio, None, bpm, None, None


def wave_adpcm(w):
    """A wave's own DSP-ADPCM frames and coefficients (decode()'s raw), or None."""
    if w.format != 2:
        return None
    n = (w.nsamples + 13) // 14 * 8
    if isinstance(w, WaveInfo):
        return {'frames': w.raw, 'coefs': w.coefs}
    return {'frames': [w.data[ci['data']:ci['data'] + n] for ci in w.channel_info],
            'coefs': [ci['adpcm']['coefs'] for ci in w.channel_info]}


# ---------------------------------------------------------------------------- level and tempo

def loudness_gain(y):
    """The gain that brings the loud parts' RMS (50 ms windows within 20 dB of the loudest)
    to LOUDNESS_DB, its peak kept under PEAK."""
    m = y.mean(axis=0)
    n = len(m) // 1600
    if n == 0:
        rms = float(np.sqrt(np.mean(m ** 2)) + 1e-9)
    else:
        w = np.sqrt(np.mean(m[:n * 1600].reshape(n, 1600) ** 2, axis=1)) + 1e-9
        loud = w[w > w.max() * 0.1]
        rms = float(np.sqrt(np.mean(loud ** 2)))
    g = 10 ** (LOUDNESS_DB / 20) / rms
    peak = float(np.abs(y).max()) + 1e-9
    return min(g, PEAK / peak)


def tempo(y, loop):
    """(bpm, first beat in seconds) from the onsets: spectral flux every 256 samples, its
    autocorrelation between 70 and 190 bpm (weighted towards 120), the phase where a comb of
    that period gathers the most flux."""
    m = y.mean(axis=0)
    seg = m[loop:] if loop is not None and len(m) - loop > RATE * 8 else m
    hop, win = 256, 1024
    n = (len(seg) - win) // hop
    if n < 200:
        return 0.0, 0.0
    idx = np.arange(win)[None, :] + hop * np.arange(n)[:, None]
    frames = seg[idx] * np.hanning(win)[None, :]
    mag = np.log1p(100 * np.abs(np.fft.rfft(frames, axis=1)))
    flux = np.maximum(np.diff(mag, axis=0), 0).sum(axis=1)
    k = 16
    flux = flux - np.convolve(flux, np.ones(k) / k, mode='same')
    flux = np.maximum(flux, 0)
    fps = RATE / hop
    ac = np.correlate(flux, flux, mode='full')[len(flux) - 1:]
    lags = np.arange(len(ac))
    lo, hi = int(fps * 60 / 190), int(fps * 60 / 70)
    bpm_of = lambda lag: 60 * fps / lag
    score = ac[lo:hi + 1] * np.exp(-0.5 * (np.log2(bpm_of(lags[lo:hi + 1]) / 120) / 0.6) ** 2)
    i = int(np.argmax(score)) + lo
    if lo < i < hi:  # parabolic peak
        a, b, c = ac[i - 1], ac[i], ac[i + 1]
        den = a - 2 * b + c
        lag = i + (0.5 * (a - c) / den if den else 0)
    else:
        lag = float(i)
    period = lag
    best, phase = -1, 0
    for off in np.arange(0, period, 0.5):
        pos = (off + period * np.arange(int((len(flux) - off) / period))).astype(int)
        v = flux[pos].sum()
        if v > best:
            best, phase = v, off
    start = (loop if seg is not m else 0) / RATE
    first = (start + phase * hop / RATE + win / 2 / RATE) % (period * hop / RATE)
    return float(bpm_of(lag)), float(first)


# ---------------------------------------------------------------------------- staging

def stage_one(args):
    key, p, out, enc = args
    try:
        return stage_piece(key, p, out, enc)
    except Exception as e:  # (one piece that can't be read doesn't stop the rest)
        return key, None, 'FAILED: %s: %s' % (type(e).__name__, e), 0.0, 0.0


def write_nstm(out, raw, samples, loop, gain, bpm, first, bar, pcm16):
    """An NSTM of a piece's own DSP-ADPCM frames (engine/audio/nstm.h), its level as a gain; the
    loop's history is the samples before its frame (pcm16: the frames' own samples)."""
    ch = len(raw['frames'])
    frames = (samples + 13) // 14
    head = struct.pack('<IHHIIIBBHffI', 0x4D54534E, 1, ch, RATE, samples, loop or 0, loop is not None, bar,
                       min(65535, int(round(gain * 4096))), bpm, first, 36 + 36 * ch)
    fs = (loop or 0) // 14 * 14
    chans = b''
    for c in range(ch):
        h1 = int(pcm16[c][fs - 1]) if fs >= 1 else 0
        h2 = int(pcm16[c][fs - 2]) if fs >= 2 else 0
        chans += struct.pack('<16hhh', *raw['coefs'][c], h1, h2)
    data = bytearray(frames * ch * 8)
    for c in range(ch):
        src = raw['frames'][c].ljust(frames * 8, b'\0')
        for f in range(frames):
            data[(f * ch + c) * 8:(f * ch + c) * 8 + 8] = src[f * 8:f * 8 + 8]
    with open(out, 'wb') as fh:
        fh.write(head + chans + bytes(data))
    return '%d samples x %d, %s, the disc\'s own ADPCM, gain %.2f' % (samples, ch, 'loops' if loop is not None else 'ends',
                                                                      gain)


def stage_piece(key, p, out, enc):
    y, loop, bpm, bar, raw = decode(p)
    g = loudness_gain(y)
    pcm16_own = np.round(y * 32768).astype(np.int32) if raw else None
    y = y * g
    pcm = np.clip(np.round(y * 32767), -32768, 32767).astype('<i2')
    digest = hashlib.sha1(pcm.tobytes()).hexdigest()
    if not bpm:
        bpm, first = tempo(y, loop)
    else:
        first = 0.0
    if bar is None:
        bar = 3 if 'WALTZ' in p.label.upper() else 4
    if raw:
        log = write_nstm(out, raw, y.shape[1], loop, g, bpm, first, bar, pcm16_own)
        return key, digest, log, pcm.shape[1] / RATE, bpm
    cmd = [enc, out, '--channels', str(pcm.shape[0]), '--bpm', '%.3f' % bpm, '--first-beat', '%.4f' % first,
           '--bar', str(bar)]
    if loop is not None:
        cmd += ['--loop', str(loop)]
    r = subprocess.run(cmd, input=np.ascontiguousarray(pcm.T).tobytes(), capture_output=True, check=True)
    return key, digest, r.stdout.decode().strip(), pcm.shape[1] / RATE, bpm


def file_name(game, label):
    return ('ws_' if game == WS else 'wsr_') + label.replace('@', '_').lower()


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('data')
    ap.add_argument('--extract', default=DEFAULT_EXTRACT)
    ap.add_argument('--enc', default=os.path.join(ROOT, 'build-host', 'nstm-enc'))
    ap.add_argument('--jobs', type=int, default=os.cpu_count())
    ap.add_argument('--only', nargs='*', help='stage just these labels (the list is written whole)')
    ap.add_argument('--list', action='store_true', help='show what would be staged and what LIST lacks')
    a = ap.parse_args()

    pieces = find_pieces(a.extract)
    # a Wii Sports label every sport's archive has (the tutorial, the game-set fanfares, the medals):
    # each sport's after the first's, named for its sport (copies of the first are dropped below)
    full = []
    for item in LIST:
        full.append(item)
        g, l = item[0], item[1]
        if g != '#' and '@' not in l:
            for sport, ja in SPORTS:
                if (g, '%s@%s' % (l, sport)) in pieces:
                    full.append((g, '%s@%s' % (l, sport), '%s（%s）' % (item[2], ja)))
    LIST[:] = full
    listed = [(g, l) for g, l, *_ in LIST if g != '#']
    missing = [k for k in listed if k not in pieces]
    unlisted = sorted(k for k in pieces if k not in set(listed) and not any(x[0] == k[0] and x[1].startswith(k[1] + '@') for x in listed))
    if missing:
        print('in LIST but not on the disc: %s' % missing)
    if unlisted:
        print('on the disc but not in LIST (not staged): %s' % unlisted)
    if a.list:
        for g, l, *rest in LIST:
            print(g, l, *rest, pieces[(g, l)].kind if (g, l) in pieces else '-')
        return

    folder = os.path.join(a.data, 'music', 'sports')
    os.makedirs(folder, exist_ok=True)
    jobs = []
    for g, l, *_ in LIST:
        if g == '#' or (g, l) not in pieces:
            continue
        if a.only and l not in a.only:
            continue
        jobs.append(((g, l), pieces[(g, l)], os.path.join(folder, file_name(g, l) + '.nstm'), a.enc))
    results = {}
    with ProcessPoolExecutor(max_workers=a.jobs) as ex:
        for key, digest, log, seconds, bpm in ex.map(stage_one, jobs):
            results[key] = digest
            if digest is None:
                print('%-48s %s' % ('%s/%s' % key, log))
            else:
                print('%-48s %6.1f s %5.1f bpm  %s' % ('%s/%s' % key, seconds, bpm, log.split(': ', 1)[-1]))

    # the list, without repeats (the first of the same samples stays)
    seen = {}
    lines = []
    for g, l, *rest in LIST:
        if g == '#':
            lines.append('#\t%s' % l)
            continue
        f = os.path.join(folder, file_name(g, l) + '.nstm')
        if not os.path.exists(f) or ((g, l) in results and results[(g, l)] is None):
            continue
        digest = results.get((g, l)) or hashlib.sha1(open(f, 'rb').read()).hexdigest()
        if digest in seen:
            print('%s/%s: the same as %s, listed once' % (g, l, seen[digest]))
            os.remove(f)
            continue
        seen[digest] = l
        jingle = 'ジングル' in (next((h for gg, h, *_ in reversed(LIST[:LIST.index((g, l, *rest))]) if gg == '#'), ''))
        lines.append('%s\t%s\t%s' % (os.path.basename(f), 'j' if jingle else '-', rest[0]))
    with open(os.path.join(a.data, 'music', 'sports.tsv'), 'w', encoding='utf-8') as fh:
        fh.write('\n'.join(lines) + '\n')
    total = sum(os.path.getsize(os.path.join(folder, n)) for n in os.listdir(folder))
    print('%d pieces, %.1f MB in %s' % (sum(1 for x in lines if not x.startswith('#')), total / 1e6, folder))


if __name__ == '__main__':
    main()
