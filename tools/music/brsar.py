#!/usr/bin/env python3
"""nw4r::snd's sound archives (BRSAR) and the files in them: RSAR, RSEQ, RBNK, RWAR/RWAV, RWSD.

Reads a Wii sound archive (as Wii no Ma's snd/douga.brsar: RSAR 1.4, RBNK 1.2, RSEQ 1.0, RWAR 1.0,
RWAV 1.2), and models how nw4r::snd's sequencer plays its sequences:

  list    the sounds: ID, label, type, player, volume, file, bank, sequence offset
  files   the files: ID, kind, size, where they live (group, item)
  groups  the groups and their items (where each file's data and wave data lie)
  banks   each bank's instruments: program -> key/velocity regions -> InstParam -> wave
  waves   a bank's waves to .wav files (DSP-ADPCM decoded), to audition the instruments
  seq     a sequence (by sound label or ID) as readable MML, or (--notes) played through a model
          of nw4r's sequencer (simulate(): tracks in SeqPlayer's order, the allocated-track mask,
          calls, loops, variables, the damper, volume moves) into a note list (ticks, track, key,
          velocity, length, release tick, program, volume, pan, envelope, sends)
  dump    a file's raw bytes (main or wave data)

Companions: rseq.py (RSEQ assembler, exact disassembler, score compiler), sndrender.py (a numpy
sampler playing simulate()'s notes with a bank's waves), check.py (a score's harmony, loop seam,
voices and levels). The archive is the user's own (never part of this repository): by default
extracted/fs/00000026/snd/douga.brsar as tools/unpack_all.py extracts the channel, or NOMA_BRSAR,
or --brsar.

Formats, as the channel's nw4r::snd reads them (addresses are the Wii no Ma DOL's):

  RSAR   header: SYMB / INFO / FILE block offsets and sizes at 0x10..0x24.
         SYMB+8: u32 string table offset (from SYMB+8) -> {u32 n, u32 offset[n]} (from SYMB+8).
         INFO+8: DataRef[6] {u8 refType, u8 dataType, u16, u32 offset from INFO+8} to the
         tables {u32 n, DataRef[n]}: sounds, banks, players, files, groups, then the
         SoundArchivePlayerInfo (u16 seqSounds, seqTracks, strmSounds, strmTracks, strmChannels,
         waveSounds, waveTracks).
         sound (SoundCommonInfo, 0x2C): +0 string, +4 file, +8 player, +0xC DataRef 3D param,
           +0x14 u8 volume, +0x15 u8 playerPriority, +0x16 u8 type (1 SEQ, 2 STRM, 3 WAVE),
           +0x17 u8 remoteFilter, +0x18 DataRef detail, +0x20 u32 userParam[2], +0x28 u8 panMode,
           +0x29 panCurve, +0x2A actorPlayerId.
         SeqSoundInfo: +0 u32 data offset (in the RSEQ's MML), +4 bank, +8 u32 allocTrack mask,
           +0xC u8 channelPriority, +0xD u8 releasePriorityFix.
         WaveSoundInfo: +0 s32 subNo, +4 allocTrack, +8 u8 channelPriority, +9 releasePrioFix.
         StrmSoundInfo: +0 u32 startPosition, +4 u16 allocChannelCount, +6 u16 allocTrackFlag.
         bank: +0 string, +4 file.  player: +0 string, +4 u8 playableSoundCount, +8 u32 heapSize.
         file: +0 u32 size, +4 u32 waveDataSize, +8 s32 entryNum, +0xC DataRef extFilePath,
           +0x14 DataRef -> {n, DataRef[n] -> FilePos {u32 group, u32 index}}.
         group: +0 string, +4 s32 entryNum, +8 DataRef extFilePath, +0x10 u32 offset (in the
           RSAR), +0x14 size, +0x18 waveDataOffset, +0x1C waveDataSize, +0x20 DataRef items ->
           GroupItemInfo {u32 file, offset, size, waveDataOffset, waveDataSize, reserved}
           (offsets from the group's offset / waveDataOffset).
  RSEQ   header DATA at 0x10, LABL at 0x18. DATA+8: u32 offset of the MML (from DATA, 0xC).
         LABL+8: {u32 n, u32 offset[n] (from LABL+8)} -> {u32 MML offset, u32 len, name}.
  RBNK   DATA+8: {u32 n, DataRef[n]} one per program; dataType 1 InstParam, 2 RangeTable
         {u8 n, u8 key[n], pad to 4 from the table, DataRef[n]} (key <= key[i] picks i),
         3 IndexTable {u8 min, u8 max, u16, DataRef[max-min+1]}, 4 none. A program's table
         splits by key, the next level by velocity (BankFileReader::GetInstParam, 8046BD10).
         InstParam (0x30): +0 s32 wave index, +4 attack, decay, sustain, release, +8 hold,
         +9 wave location (0 index into the wave archive), +0xA noteOffType (0 release,
         1 ignore), +0xB alternateAssign, +0xC originalKey, +0xD volume, +0xE pan,
         +0xF surroundPan, +0x10 f32 tune (ReadInstInfo, 8046BE20).
         With no WAVE block (nblocks 1) the waves are the group item's wave data: an RWAR.
  RWAR   TABL+8: {u32 n, {DataRef (from the DATA block), u32 size}[n]} -> RWAV files.
  RWAV   INFO+8: WaveInfo {u8 format (0 PCM8, 1 PCM16, 2 ADPCM), u8 loop, u8 channels,
         u8 rate>>16, u16 rate, u8 dataLocationType, u8, u32 loopStart, u32 loopEnd (DSP
         addresses: nibbles for ADPCM), u32 channel table offset, u32 dataLocation, u32}.
         Channel table: u32 offset[n] -> {u32 data offset, u32 adpcm offset, u32 vol[4], u32}.
         Samples: WaveInfo + dataLocation + 8 + channel data offset (WaveFileReader, 80484670).
         ADPCM info: s16 coef[16], u16 gain, ps, yn1, yn2, loop ps, loop yn1, loop yn2.

Usage:  brsar.py [--brsar FILE] list|files|groups|banks|waves|seq|dump ...   (see --help)
"""
import argparse
import os
import struct
import sys
import wave

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
# the channel's sound archive, as tools/unpack_all.py extracts it (or NOMA_BRSAR, or --brsar)
DEFAULT_BRSAR = os.environ.get('NOMA_BRSAR') or os.path.join(ROOT, 'extracted/fs/00000026/snd/douga.brsar')

SOUND_TYPES = {1: 'SEQ', 2: 'STRM', 3: 'WAVE'}


def be_u16(d, o):
    return struct.unpack_from('>H', d, o)[0]


def be_s16(d, o):
    return struct.unpack_from('>h', d, o)[0]


def be_u32(d, o):
    return struct.unpack_from('>I', d, o)[0]


def be_s32(d, o):
    return struct.unpack_from('>i', d, o)[0]


def be_f32(d, o):
    return struct.unpack_from('>f', d, o)[0]


def block_header(d, o):
    """(magic, version, size, [(block offset, size)...]) of an nw4r binary file at o."""
    magic = d[o:o + 4].decode('latin1')
    bom, version, size, hsize, nblocks = struct.unpack_from('>HHIHH', d, o + 4)
    blocks = [struct.unpack_from('>II', d, o + 0x10 + 8 * i) for i in range(nblocks)]
    return magic, version, size, blocks


# ---------------------------------------------------------------------------- RSAR


class Sound:
    __slots__ = ('id', 'label', 'file', 'player', 'volume', 'player_priority', 'type', 'remote_filter',
                 'user_param', 'pan_mode', 'pan_curve', 'actor_player', 'info_offset',
                 'seq_offset', 'bank', 'alloc_track', 'channel_priority', 'release_priority_fix',
                 'wave_index', 'strm_start', 'strm_channels', 'strm_tracks')

    def __init__(self):
        for s in self.__slots__:
            setattr(self, s, None)

    @property
    def type_name(self):
        return SOUND_TYPES.get(self.type, '?%d' % self.type)


class Rsar:
    """A BRSAR in memory: sounds, banks, players, files and groups, and their data."""

    def __init__(self, path=DEFAULT_BRSAR, data=None):
        self.path = path
        self.d = d = data if data is not None else open(path, 'rb').read()
        magic, self.version, size, blocks = block_header(d, 0)
        if magic != 'RSAR':
            raise ValueError('not an RSAR: %r' % magic)
        (self.symb, _), (self.info, _), (self.file_block, _) = blocks[:3]
        # SYMB
        s8 = self.symb + 8
        strings = s8 + be_u32(d, s8)
        n = be_u32(d, strings)
        self.strings = []
        for i in range(n):
            p = s8 + be_u32(d, strings + 4 + 4 * i)
            self.strings.append(d[p:d.index(b'\0', p)].decode('latin1'))
        # INFO
        self.I = I = self.info + 8
        tables = [I + be_u32(d, I + 8 * k + 4) for k in range(6)]

        def entries(t):
            return [I + be_u32(d, t + 4 + 8 * i + 4) for i in range(be_u32(d, t))]

        self.sound_entries = entries(tables[0])
        self.bank_entries = entries(tables[1])
        self.player_entries = entries(tables[2])
        self.file_entries = entries(tables[3])
        self.group_entries = entries(tables[4])
        p = tables[5]
        self.player_info = dict(zip(('seq_sounds', 'seq_tracks', 'strm_sounds', 'strm_tracks', 'strm_channels',
                                     'wave_sounds', 'wave_tracks'), struct.unpack_from('>7H', d, p)))
        self.sounds = [self._sound(i, e) for i, e in enumerate(self.sound_entries)]
        self.banks = [{'id': i, 'label': self.string(be_u32(d, e)), 'file': be_u32(d, e + 4), 'info_offset': e}
                      for i, e in enumerate(self.bank_entries)]
        self.players = [{'id': i, 'label': self.string(be_u32(d, e)), 'playable': d[e + 4],
                         'heap': be_u32(d, e + 8), 'info_offset': e} for i, e in enumerate(self.player_entries)]
        self.groups = []
        for i, e in enumerate(self.group_entries):
            g = {'id': i, 'label': self.string(be_u32(d, e)), 'offset': be_u32(d, e + 0x10),
                 'size': be_u32(d, e + 0x14), 'wave_offset': be_u32(d, e + 0x18), 'wave_size': be_u32(d, e + 0x1C),
                 'info_offset': e, 'items': []}
            t = I + be_u32(d, e + 0x24)
            for k in range(be_u32(d, t)):
                it = I + be_u32(d, t + 4 + 8 * k + 4)
                f, off, size, woff, wsize, _ = struct.unpack_from('>6I', d, it)
                g['items'].append({'file': f, 'offset': off, 'size': size, 'wave_offset': woff, 'wave_size': wsize,
                                   'info_offset': it})
            self.groups.append(g)
        self.files = []
        for i, e in enumerate(self.file_entries):
            f = {'id': i, 'size': be_u32(d, e), 'wave_size': be_u32(d, e + 4), 'ext': be_u32(d, e + 0x10),
                 'info_offset': e, 'pos': []}
            t = I + be_u32(d, e + 0x18)
            for k in range(be_u32(d, t)):
                fp = I + be_u32(d, t + 4 + 8 * k + 4)
                f['pos'].append(struct.unpack_from('>II', d, fp))
            self.files.append(f)
        self.by_label = {s.label: s for s in self.sounds if s.label}

    def string(self, i):
        return self.strings[i] if i < len(self.strings) else None

    def _sound(self, i, e):
        d = self.d
        s = Sound()
        s.id, s.info_offset = i, e
        s.label = self.string(be_u32(d, e))
        s.file, s.player = be_u32(d, e + 4), be_u32(d, e + 8)
        s.volume, s.player_priority, s.type, s.remote_filter = d[e + 0x14], d[e + 0x15], d[e + 0x16], d[e + 0x17]
        s.user_param = struct.unpack_from('>II', d, e + 0x20)
        s.pan_mode, s.pan_curve, s.actor_player = d[e + 0x28], d[e + 0x29], d[e + 0x2A]
        x = self.I + be_u32(d, e + 0x1C)
        if s.type == 1:
            s.seq_offset, s.bank, s.alloc_track = struct.unpack_from('>III', d, x)
            s.channel_priority, s.release_priority_fix = d[x + 0xC], d[x + 0xD]
        elif s.type == 3:
            s.wave_index, s.alloc_track = struct.unpack_from('>iI', d, x)
            s.channel_priority, s.release_priority_fix = d[x + 8], d[x + 9]
        elif s.type == 2:
            s.strm_start, s.strm_channels, s.strm_tracks = struct.unpack_from('>IHH', d, x)
        return s

    def sound(self, key):
        """A sound by label or id (int, or a string of digits / 0x...)."""
        if isinstance(key, int):
            return self.sounds[key]
        if key in self.by_label:
            return self.by_label[key]
        try:
            return self.sounds[int(key, 0)]
        except ValueError:
            raise KeyError('no sound %r' % key)

    def file_location(self, file_id):
        """(group, item) holding a file (its first position), or None."""
        f = self.files[file_id]
        if not f['pos']:
            return None
        g, i = f['pos'][0]
        return self.groups[g], self.groups[g]['items'][i]

    def file_offset(self, file_id):
        """Offsets in the RSAR of a file's main data and wave data (MemorySoundArchive's
        detail_GetFileAddress / detail_GetWaveDataFileAddress), or (None, None)."""
        loc = self.file_location(file_id)
        if not loc:
            return None, None
        g, it = loc
        main = g['offset'] + it['offset']
        wave = g['wave_offset'] + it['wave_offset'] if it['wave_size'] else None
        return main, wave

    def file_data(self, file_id):
        main, _ = self.file_offset(file_id)
        if main is None:
            return b''
        it = self.file_location(file_id)[1]
        return self.d[main:main + it['size']]

    def wave_data(self, file_id):
        _, w = self.file_offset(file_id)
        if w is None:
            return b''
        it = self.file_location(file_id)[1]
        return self.d[w:w + it['wave_size']]

    def file_kind(self, file_id):
        b = self.file_data(file_id)
        return b[:4].decode('latin1') if len(b) >= 4 else '-'

    def bank(self, bank_id):
        b = self.banks[bank_id]
        return Rbnk(self.file_data(b['file']), self.wave_data(b['file']), label=b['label'])

    def sequence(self, file_id):
        return Rseq(self.file_data(file_id))


# ---------------------------------------------------------------------------- RWAV / RWAR

ADPCM_FRAME = 8   # bytes: a header byte and 14 nibbles
ADPCM_SAMPLES = 14


def dsp_nibble_to_sample(n):
    """AxVoice::GetSampleByDspAddress for ADPCM (80468E40): a nibble address to a sample."""
    return (n // 16) * 14 + (n % 16) - 2


def decode_adpcm(data, coefs, nsamples, hist1=0, hist2=0):
    """DSP-ADPCM to int16: frames of a predictor/scale byte and 14 nibbles."""
    c = [(coefs[2 * i], coefs[2 * i + 1]) for i in range(8)]
    out = np.zeros(nsamples, dtype=np.int16)
    o = out  # local
    n = 0
    nframes = (nsamples + 13) // 14
    for f in range(nframes):
        base = f * 8
        if base >= len(data):
            break
        ps = data[base]
        c1, c2 = c[(ps >> 4) & 7]
        scale = 1 << (ps & 0xF)
        frame = data[base + 1:base + 8]
        for k in range(14):
            if n >= nsamples:
                break
            byte = frame[k >> 1]
            nib = (byte >> 4) if not (k & 1) else (byte & 0xF)
            if nib >= 8:
                nib -= 16
            s = ((nib * scale) << 11) + 1024 + c1 * hist1 + c2 * hist2
            s >>= 11
            if s > 32767:
                s = 32767
            elif s < -32768:
                s = -32768
            o[n] = s
            hist2, hist1 = hist1, s
            n += 1
    return out


class Rwav:
    """A RWAV file: its parameters and decoded samples (float32 -1..1, per channel)."""

    FORMATS = {0: 'PCM8', 1: 'PCM16', 2: 'ADPCM'}

    def __init__(self, data):
        self.data = data
        magic, self.version, size, blocks = block_header(data, 0)
        if magic != 'RWAV':
            raise ValueError('not an RWAV: %r' % magic)
        w = blocks[0][0] + 8
        self.info_offset = w
        d = data
        self.format, self.loop, self.channels = d[w], d[w + 1], d[w + 2]
        self.rate = (d[w + 3] << 16) | be_u16(d, w + 4)
        self.location_type = d[w + 6]
        self.loop_start_addr, self.loop_end_addr, ch_table, self.data_location = struct.unpack_from('>IIII', d, w + 8)
        self.channel_info = []
        for c in range(self.channels):
            ci = w + be_u32(d, w + ch_table + 4 * c)
            data_off, adpcm_off = be_u32(d, ci), be_u32(d, ci + 4)
            adpcm = None
            if self.format == 2 and adpcm_off:
                a = w + adpcm_off
                adpcm = {'coefs': struct.unpack_from('>16h', d, a), 'gain': be_u16(d, a + 0x20),
                         'ps': be_u16(d, a + 0x22), 'yn1': be_s16(d, a + 0x24), 'yn2': be_s16(d, a + 0x26),
                         'loop_ps': be_u16(d, a + 0x28), 'loop_yn1': be_s16(d, a + 0x2A),
                         'loop_yn2': be_s16(d, a + 0x2C)}
            self.channel_info.append({'data': w + self.data_location + 8 + data_off, 'adpcm': adpcm})
        if self.format == 2:
            self.loop_start = dsp_nibble_to_sample(self.loop_start_addr)
            self.nsamples = dsp_nibble_to_sample(self.loop_end_addr) + 1
        elif self.format == 1:
            self.loop_start, self.nsamples = self.loop_start_addr, self.loop_end_addr + 1
        else:
            self.loop_start, self.nsamples = self.loop_start_addr, self.loop_end_addr + 1
        self._pcm = None

    def pcm(self):
        """float32 array [channels, samples]."""
        if self._pcm is None:
            chans = []
            for ci in self.channel_info:
                p = ci['data']
                if self.format == 2:
                    nbytes = (self.nsamples + 13) // 14 * 8
                    a = ci['adpcm']
                    s = decode_adpcm(self.data[p:p + nbytes], a['coefs'], self.nsamples, a['yn1'], a['yn2'])
                    chans.append(s.astype(np.float32) / 32768.0)
                elif self.format == 1:
                    s = np.frombuffer(self.data[p:p + 2 * self.nsamples], dtype='>i2')
                    chans.append(s.astype(np.float32) / 32768.0)
                else:
                    s = np.frombuffer(self.data[p:p + self.nsamples], dtype=np.int8)
                    chans.append(s.astype(np.float32) / 128.0)
            self._pcm = np.stack(chans)
        return self._pcm

    def describe(self):
        return '%s %dch %d Hz, %d samples (%.3f s)%s' % (
            self.FORMATS.get(self.format, '?'), self.channels, self.rate, self.nsamples, self.nsamples / self.rate,
            ', loop from %d' % self.loop_start if self.loop else '')


class Rwar:
    def __init__(self, data):
        self.data = data
        magic, self.version, size, blocks = block_header(data, 0)
        if magic != 'RWAR':
            raise ValueError('not an RWAR: %r' % magic)
        (tabl, _), (dat, _) = blocks[:2]
        n = be_u32(data, tabl + 8)
        self.entries = []
        for i in range(n):
            e = tabl + 0xC + 0xC * i
            off, size = be_u32(data, e + 4), be_u32(data, e + 8)
            self.entries.append((dat + off, size))
        self._waves = {}

    def __len__(self):
        return len(self.entries)

    def wave(self, i):
        if i not in self._waves:
            o, size = self.entries[i]
            self._waves[i] = Rwav(self.data[o:o + size])
        return self._waves[i]


# ---------------------------------------------------------------------------- RBNK


class InstParam:
    __slots__ = ('offset', 'wave', 'attack', 'decay', 'sustain', 'release', 'hold', 'location', 'note_off',
                 'alternate', 'original_key', 'volume', 'pan', 'span', 'tune')

    def __init__(self, d, o):
        self.offset = o
        self.wave = be_s32(d, o)
        (self.attack, self.decay, self.sustain, self.release, self.hold, self.location, self.note_off,
         self.alternate, self.original_key, self.volume, self.pan, self.span) = d[o + 4:o + 0x10]
        self.tune = be_f32(d, o + 0x10)

    def short(self):
        return ('wave %d key %s vol %d pan %d ADSR %d/%d/%d/%d hold %d tune %.4f%s' % (
            self.wave, note_name(self.original_key), self.volume, self.pan, self.attack, self.decay, self.sustain,
            self.release, self.hold, self.tune, ' noteoff-ignored' if self.note_off else ''))


class Rbnk:
    """A bank: per program a tree of key / velocity regions ending in InstParams."""

    def __init__(self, data, wave_data=b'', label=None):
        self.data, self.label = data, label
        magic, self.version, size, blocks = block_header(data, 0)
        if magic != 'RBNK':
            raise ValueError('not an RBNK: %r' % magic)
        self.data_block = blocks[0][0]
        self.base = self.data_block + 8
        self.wave_block = blocks[1][0] if len(blocks) > 1 and blocks[1][0] else None
        self.nprograms = be_u32(data, self.base)
        self.rwar = Rwar(wave_data) if wave_data[:4] == b'RWAR' else None

    def ref(self, o):
        """(dataType, address in data) of the DataRef at o (offsets from DATA+8)."""
        rtype, dtype = self.data[o], self.data[o + 1]
        return dtype, self.base + be_u32(self.data, o + 4)

    def program_ref(self, prg):
        if not 0 <= prg < self.nprograms:
            return 4, None
        return self.ref(self.base + 4 + 8 * prg)

    def _sub(self, dtype, at, value):
        """BankFileReader::GetReferenceToSubRegion (8046BFC0): the DataRef for value."""
        d = self.data
        if dtype == 2:
            n = d[at]
            for i in range(n):
                if value <= d[at + 1 + i]:
                    return self.ref(at + ((n + 4) & ~3) + 8 * i)
            return 4, None
        if dtype == 3:
            lo, hi = d[at], d[at + 1]
            if not lo <= value <= hi:
                return 4, None
            return self.ref(at + 4 + 8 * (value - lo))
        return 4, None

    def inst(self, prg, key, velocity=127):
        """BankFileReader::GetInstParam (8046BD10): the InstParam for a note, or None."""
        dtype, at = self.program_ref(prg)
        if dtype == 4:
            return None
        if dtype != 1:
            dtype, at = self._sub(dtype, at, key)
            if dtype == 4:
                return None
        if dtype != 1:
            dtype, at = self._sub(dtype, at, velocity)
            if dtype == 4:
                return None
        return InstParam(self.data, at) if dtype == 1 else None

    def regions(self, prg):
        """[(key lo, key hi, vel lo, vel hi, InstParam)] of a program."""
        out = []
        dtype, at = self.program_ref(prg)
        if dtype == 4:
            return out

        def split(dtype, at):
            d = self.data
            if dtype == 1:
                return [(0, 127, InstParam(d, at))]
            res, lo = [], 0
            if dtype == 2:
                n = d[at]
                for i in range(n):
                    hi = d[at + 1 + i]
                    sd, sa = self.ref(at + ((n + 4) & ~3) + 8 * i)
                    res.append((lo, hi, sd, sa))
                    lo = hi + 1
            elif dtype == 3:
                a, b = d[at], d[at + 1]
                for k in range(a, b + 1):
                    sd, sa = self.ref(at + 4 + 8 * (k - a))
                    res.append((k, k, sd, sa))
            return res

        if dtype == 1:
            return [(0, 127, 0, 127, InstParam(self.data, at))]
        for klo, khi, sd, sa in split(dtype, at):
            if sd == 4:
                continue
            if sd == 1:
                out.append((klo, khi, 0, 127, InstParam(self.data, sa)))
                continue
            for vlo, vhi, vd, va in split(sd, sa):
                if vd == 1:
                    out.append((klo, khi, vlo, vhi, InstParam(self.data, va)))
        return out

    def wave(self, index):
        return self.rwar.wave(index) if self.rwar else None


# ---------------------------------------------------------------------------- RSEQ / MML

NOTE_NAMES = ['c', 'c#', 'd', 'd#', 'e', 'f', 'f#', 'g', 'g#', 'a', 'a#', 'b']


def note_name(key):
    """60 -> 'c4' (nw4r's cn4), 0 -> 'c-1'."""
    return '%s%d' % (NOTE_NAMES[key % 12], key // 12 - 1)


# MML commands (nw4r::snd::detail::MmlParser, engine/snd/seq.cpp): name, argument kind
#   u8, s8, s16, vmidi (variable-length), and special ones handled in the decoder
CMD_U8 = {
    0xB0: 'timebase', 0xB1: 'env_hold', 0xB2: 'monophonic', 0xB3: 'velocity_range', 0xB4: 'biquad_type',
    0xB5: 'biquad_value',
    0xC0: 'pan', 0xC1: 'volume', 0xC2: 'main_volume', 0xC3: 'transpose', 0xC4: 'pitchbend', 0xC5: 'bendrange',
    0xC6: 'prio', 0xC7: 'notewait', 0xC8: 'tie', 0xC9: 'porta', 0xCA: 'mod_depth', 0xCB: 'mod_speed',
    0xCC: 'mod_type', 0xCD: 'mod_range', 0xCE: 'porta_sw', 0xCF: 'porta_time',
    0xD0: 'attack', 0xD1: 'decay', 0xD2: 'sustain', 0xD3: 'release', 0xD4: 'loop_start', 0xD5: 'volume2',
    0xD6: 'printvar', 0xD7: 'span', 0xD8: 'lpf_cutoff', 0xD9: 'fxsend_a', 0xDA: 'fxsend_b', 0xDB: 'mainsend',
    0xDC: 'init_pan', 0xDD: 'mute', 0xDE: 'fxsend_c', 0xDF: 'damper',
}
CMD_S8 = {0xC3, 0xC4}  # transpose, pitchbend
CMD_S16 = {0xE0: 'mod_delay', 0xE1: 'tempo', 0xE3: 'sweep_pitch'}
CMD_NONE = {0xFB: 'env_reset', 0xFC: 'loop_end', 0xFD: 'ret', 0xFF: 'fin'}
CMD_EXT_VAR = {0x80: 'setvar', 0x81: 'addvar', 0x82: 'subvar', 0x83: 'mulvar', 0x84: 'divvar', 0x85: 'shiftvar',
               0x86: 'randvar', 0x87: 'andvar', 0x88: 'orvar', 0x89: 'xorvar', 0x8A: 'notvar', 0x8B: 'modvar',
               0x90: 'cmp_eq', 0x91: 'cmp_ge', 0x92: 'cmp_gt', 0x93: 'cmp_le', 0x94: 'cmp_lt', 0x95: 'cmp_ne'}
CMD_BY_NAME = {}
for _t in (CMD_U8, CMD_S16, CMD_NONE):
    for _k, _v in _t.items():
        CMD_BY_NAME[_v] = _k
CMD_BY_NAME.update({'wait': 0x80, 'prg': 0x81, 'opentrack': 0x88, 'jump': 0x89, 'call': 0x8A,
                    'alloctrack': 0xFE, 'userproc': 0xF0E0})
EXT_BY_NAME = {v: k for k, v in CMD_EXT_VAR.items()}


class Rseq:
    """A RSEQ file: its MML data and labels."""

    def __init__(self, data):
        self.data = data
        magic, self.version, size, blocks = block_header(data, 0)
        if magic != 'RSEQ':
            raise ValueError('not an RSEQ: %r' % magic)
        (self.data_block, self.data_size), (self.labl_block, _) = blocks[:2]
        self.mml_base = self.data_block + be_u32(data, self.data_block + 8)
        self.mml = data[self.mml_base:self.data_block + self.data_size]
        self.labels = []
        if self.labl_block:
            l8 = self.labl_block + 8
            for i in range(be_u32(data, l8)):
                p = l8 + be_u32(data, l8 + 4 + 4 * i)
                off, n = be_u32(data, p), be_u32(data, p + 4)
                self.labels.append((data[p + 8:p + 8 + n].decode('latin1'), off))

    def label_at(self, off):
        return [n for n, o in self.labels if o == off]


class MmlReader:
    """Decodes one MML command at a time (MmlParser::Parse), returning a dict."""

    def __init__(self, mml):
        self.m = mml

    def vmidi(self, p):
        v = 0
        while True:
            b = self.m[p]
            p += 1
            v = (v << 7) | (b & 0x7F)
            if not b & 0x80:
                return v, p

    def arg(self, p, kind):
        m = self.m
        if kind == 'u8':
            return m[p], p + 1
        if kind == 's8':
            v = m[p]
            return (v - 256 if v >= 128 else v), p + 1
        if kind == 's16':
            return struct.unpack_from('>h', m, p)[0], p + 2
        if kind == 'vmidi':
            return self.vmidi(p)
        if kind == 'random':
            lo, hi = struct.unpack_from('>hh', m, p)
            return ('random', lo, hi), p + 4
        if kind == 'variable':
            return ('var', m[p]), p + 1
        raise ValueError(kind)

    def decode(self, p):
        """{'at', 'end', 'op', 'name', 'args', 'if', 'note'...} of the command at p."""
        m = self.m
        c = {'at': p, 'if': False}
        cmd = m[p]
        p += 1
        if cmd == 0xA2:
            c['if'] = True
            cmd = m[p]
            p += 1
        type2 = None
        if cmd in (0xA3, 0xA4, 0xA5):
            type2 = {0xA3: 's16', 0xA4: 'random', 0xA5: 'variable'}[cmd]
            cmd = m[p]
            p += 1
        override = None
        if cmd in (0xA0, 0xA1):
            override = 'random' if cmd == 0xA0 else 'variable'
            cmd = m[p]
            p += 1

        def kind(normal):
            return override or normal

        c['op'] = cmd
        c['prefix'] = (type2, override)
        if cmd < 0x80:
            vel = m[p]
            length, p = self.arg(p + 1, kind('vmidi'))
            c.update(name='note', key=cmd, velocity=vel, length=length, args=[cmd, vel, length])
        elif cmd == 0x80:
            v, p = self.arg(p, kind('vmidi'))
            c.update(name='wait', args=[v])
        elif cmd == 0x81:
            v, p = self.arg(p, kind('vmidi'))
            c.update(name='prg', args=[v])
        elif cmd == 0x88:
            no = m[p]
            off = (m[p + 1] << 16) | (m[p + 2] << 8) | m[p + 3]
            p += 4
            c.update(name='opentrack', args=[no, off])
        elif cmd in (0x89, 0x8A):
            off = (m[p] << 16) | (m[p + 1] << 8) | m[p + 2]
            p += 3
            c.update(name='jump' if cmd == 0x89 else 'call', args=[off])
        elif 0x80 <= cmd < 0x90:
            c.update(name='op_%02x' % cmd, args=[])
        elif 0xB0 <= cmd < 0xE0:
            v, p = self.arg(p, kind('s8' if cmd in CMD_S8 else 'u8'))
            args = [v]
            if type2:
                v2, p = self.arg(p, type2)
                args.append(v2)
            c.update(name=CMD_U8.get(cmd, 'op_%02x' % cmd), args=args)
        elif 0x90 <= cmd < 0xA0:
            c.update(name='op_%02x' % cmd, args=[])
        elif 0xE0 <= cmd < 0xF0:
            v, p = self.arg(p, kind('s16'))
            c.update(name=CMD_S16.get(cmd, 'op_%02x' % cmd), args=[v])
        elif cmd == 0xFE:
            c.update(name='alloctrack', args=[be_u16(m, p)])
            p += 2
        elif cmd == 0xF0:
            ext = m[p]
            p += 1
            if ext & 0xF0 == 0xE0:
                v, p = self.arg(p, kind('s16'))
                c.update(name='userproc', args=[v & 0xFFFF] if isinstance(v, int) else [v], ext=ext)
            elif ext & 0xF0 in (0x80, 0x90):
                var = m[p]
                v, p = self.arg(p + 1, kind('s16'))
                c.update(name=CMD_EXT_VAR.get(ext, 'ext_%02x' % ext), args=[var, v], ext=ext)
            else:
                c.update(name='ext_%02x' % ext, args=[], ext=ext)
        else:
            c.update(name=CMD_NONE.get(cmd, 'op_%02x' % cmd), args=[])
        c['end'] = p
        return c


def fmt_arg(a):
    if isinstance(a, tuple):
        if a[0] == 'random':
            return 'r(%d..%d)' % (a[1], a[2])
        return 'var%d' % a[1]
    return str(a)


def format_command(c):
    pre = 'if ' if c['if'] else ''
    if c['name'] == 'note':
        return '%s%-4s v%d %s' % (pre, note_name(c['key']), c['velocity'], fmt_arg(c['length']))
    if c['name'] in ('jump', 'call'):
        return '%s%s @%04X' % (pre, c['name'], c['args'][0])
    if c['name'] == 'opentrack':
        return '%sopentrack %d, @%04X' % (pre, c['args'][0], c['args'][1])
    if c['name'] == 'alloctrack':
        return '%salloctrack 0x%04X' % (pre, c['args'][0])
    t2 = c['prefix'][0]
    name = c['name'] + ('_t' if t2 == 's16' else '_tr' if t2 == 'random' else '_tv' if t2 == 'variable' else '')
    if c['name'] == 'prg':
        return '%sprg %s' % (pre, fmt_arg(c['args'][0]))
    return pre + name + (' ' + ', '.join(fmt_arg(a) for a in c['args']) if c['args'] else '')


def disassemble(mml, starts, labels=()):
    """Readable MML of the code reachable from starts ([(offset, name)]): a listing with
    offsets, jump/call targets labelled."""
    rd = MmlReader(mml)
    seen = {}
    names = {}
    for off, n in labels:
        names.setdefault(off, []).append(n)
    todo = []
    for off, n in starts:
        names.setdefault(off, []).append(n)
        todo.append(off)
    targets = set(o for o, _ in starts)
    while todo:
        p = todo.pop()
        while 0 <= p < len(mml) and p not in seen:
            c = rd.decode(p)
            seen[p] = c
            if c['name'] == 'opentrack':
                t = c['args'][1]
                names.setdefault(t, []).append('track%d' % c['args'][0])
                targets.add(t)
                todo.append(t)
            elif c['name'] in ('jump', 'call'):
                t = c['args'][0]
                targets.add(t)
                todo.append(t)
            if c['name'] in ('fin', 'ret') and not c['if'] or (c['name'] == 'jump' and not c['if']):
                break
            p = c['end']
    lines = []
    prev_end = None
    for p in sorted(seen):
        c = seen[p]
        if prev_end is not None and p != prev_end:
            lines.append('')
        if p in names or p in targets:
            lines.append('%s:' % (' / '.join(names.get(p, [])) or 'L_%04X' % p))
        lines.append('  %04X  %-28s ; %s' % (p, format_command(c), mml[p:c['end']].hex(' ')))
        prev_end = c['end']
    return '\n'.join(lines)


# ---- a model of the sequencer: the notes a sequence plays


class TrackState:
    def __init__(self, no, pc):
        self.no, self.pc = no, pc
        self.wait = 0
        self.stack = []          # [(kind 'call'/'loop', return address, count)]
        self.vars = [0] * 16
        self.cmp = False
        self.notewait = True
        self.tie = False
        self.program = 0
        self.volume, self.volume2, self.pan, self.init_pan, self.span = 127, 127, 0, 0, 0
        self.transpose, self.bend, self.bendrange = 0, 0, 2
        self.velocity_range = 127
        self.adsr = [255, 255, 255, 255]
        self.hold = 255
        self.mod = {'depth': 0, 'speed': 16, 'type': 0, 'range': 1, 'delay': 0}
        self.porta = False
        self.porta_key = 60
        self.porta_time = 0
        self.sweep = 0
        self.mono = False
        self.mute = False
        self.ended = False
        self.loops = 0           # backward jumps / infinite loop ends taken
        self.opened = True
        self.wait_note_end = False
        self.last_key = None
        self.fx = [0, 0, 0]       # fxsend a/b/c (0..127)
        self.main_send = 127
        self.lpf = 0
        self.volume_move = None  # (from, start tick, ticks): `volume` with a move time

    def volume_at(self, tick):
        if not self.volume_move:
            return self.volume
        start_value, start, ticks = self.volume_move
        if ticks <= 0 or tick - start >= ticks:
            return self.volume
        return start_value + (tick - start) * (self.volume - start_value) // ticks


def simulate(mml, start, max_ticks=48 * 4 * 400, loops=1, player_vars=None, seed=1, alloc_mask=None):
    """Plays a sequence from start through a model of nw4r's SeqPlayer/SeqTrack (tick by tick)
    and returns {'notes': [...], 'tempo': [(tick, bpm)], 'timebase', 'ticks', 'loop_tick',
    'events': [...]}; tracks stop after `loops` loop-backs (a backward jump or the end of an
    infinite loop), the whole after every track has, or at max_ticks.

    As SeqPlayer::ParseNextTick (80473740) does, every tick runs tracks 0..15 in order, so a
    track opened by a lower one starts in the same tick. alloc_mask: the tracks the sound
    allocates (the sound info's allocTrack, or the sequence's leading `alloctrack`); opening
    any other track does nothing, as in SeqPlayer::GetPlayerTrack. None: start at `start`, and
    if the MML there is an alloctrack command, take its mask (MmlParser::ParseAllocTrack,
    804718B0), else every track.

    Each note gets 'off_tick': where its channel is released. A note's length counts down a
    tick at a time and releases it at 0 unless the track's damper (CC64, `damper` >= 64) is
    down, in which case it is released on the first tick the damper is up again
    (SeqTrack::UpdateChannelLength); notes of length 0 or below (ties) and notes whose
    instrument ignores note-off are left to the renderer."""
    if alloc_mask is None:
        if mml[start] == 0xFE:
            alloc_mask = (mml[start + 1] << 8) | mml[start + 2]
            start += 3
        else:
            alloc_mask = 0xFFFF
    rd = MmlReader(mml)
    rng = np.random.RandomState(seed)
    gvars = [0] * 16
    pvars = list(player_vars or [0] * 16)
    tracks = {0: TrackState(0, start)}
    tempo, timebase, main_volume = 120, 48, 127
    out = {'notes': [], 'tempo': [(0, 120)], 'events': [], 'loop_tick': {}, 'track_programs': {}}

    def var_ref(t, i):
        if i < 0x10:
            return pvars, i
        if i < 0x20:
            return gvars, i - 0x10
        return t.vars, i - 0x20

    def value(t, a):
        if isinstance(a, tuple):
            if a[0] == 'random':
                return int(rng.randint(a[1], a[2] + 1))
            arr, i = var_ref(t, a[1])
            return arr[i]
        return a

    damper = {}   # track -> [(tick, down)]
    tick = 0
    while tick < max_ticks:
        for no in range(16):
            t = tracks.get(no)
            if t is None or t.ended:
                continue
            if t.wait > 0:
                t.wait -= 1
                if t.wait > 0:
                    continue
            guard = 0
            while t.wait == 0 and not t.ended:
                guard += 1
                if guard > 10000:
                    t.ended = True
                    break
                c = rd.decode(t.pc)
                t.pc = c['end']
                if c['if'] and not t.cmp:
                    continue
                n = c['name']
                if n == 'note':
                    length = value(t, c['length'])
                    key = max(0, min(127, c['key'] + t.transpose))
                    vel = c['velocity'] * t.velocity_range // 127
                    if not t.mute:
                        out['notes'].append({'tick': tick, 'track': no, 'key': key, 'velocity': vel,
                                             'length': length, 'program': t.program,
                                             'volume': t.volume_at(tick),
                                             'volume2': t.volume2, 'pan': t.pan + t.init_pan, 'span': t.span,
                                             'bend': t.bend * t.bendrange / 128.0, 'adsr': list(t.adsr),
                                             'hold': t.hold, 'tie': t.tie, 'mono': t.mono,
                                             'porta': (t.porta_key if t.porta else None), 'porta_time': t.porta_time,
                                             'mod': dict(t.mod), 'sweep': t.sweep,
                                             'main_volume': main_volume, 'fx': list(t.fx),
                                             'main_send': t.main_send, 'lpf': t.lpf})
                        out['track_programs'].setdefault(no, set()).add(t.program)
                    t.porta_key = key
                    if t.notewait:
                        t.wait = length
                elif n == 'wait':
                    t.wait = value(t, c['args'][0])
                elif n == 'prg':
                    t.program = value(t, c['args'][0])
                elif n == 'opentrack':
                    k, off = c['args']
                    if k <= 15 and alloc_mask >> k & 1 and k != no:
                        tracks[k] = TrackState(k, off)
                elif n == 'jump':
                    target = c['args'][0]
                    if target < c['at']:
                        t.loops += 1
                        out['loop_tick'].setdefault(no, []).append((tick, target))
                        if t.loops >= loops:
                            t.ended = True
                            break
                    t.pc = target
                elif n == 'call':
                    if len(t.stack) < 3:
                        t.stack.append(('call', t.pc, 0))
                        t.pc = c['args'][0]
                elif n == 'ret':
                    while t.stack:
                        kind, ret, _ = t.stack.pop()
                        if kind == 'call':
                            t.pc = ret
                            break
                elif n == 'loop_start':
                    if len(t.stack) < 3:
                        t.stack.append(('loop', t.pc, value(t, c['args'][0])))
                elif n == 'loop_end':
                    if t.stack and t.stack[-1][0] == 'loop':
                        kind, ret, count = t.stack[-1]
                        if count == 0:
                            t.loops += 1
                            out['loop_tick'].setdefault(no, []).append((tick, ret))
                            if t.loops >= loops:
                                t.ended = True
                                break
                            t.pc = ret
                        else:
                            count -= 1
                            if count == 0:
                                t.stack.pop()
                            else:
                                t.stack[-1] = (kind, ret, count)
                                t.pc = ret
                elif n == 'fin':
                    t.ended = True
                elif n == 'tempo':
                    tempo = max(0, min(1023, value(t, c['args'][0])))
                    out['tempo'].append((tick, tempo))
                elif n == 'timebase':
                    timebase = value(t, c['args'][0])
                elif n == 'main_volume':
                    main_volume = value(t, c['args'][0])
                elif n == 'volume':
                    # with a second argument, a move there over that many ticks (MoveParam: the
                    # value at a note is where the move has got to)
                    now = t.volume_at(tick)
                    t.volume = value(t, c['args'][0])
                    t.volume_move = (now, tick, c['args'][1]) if len(c['args']) > 1 else None
                elif n == 'volume2':
                    t.volume2 = value(t, c['args'][0])
                elif n == 'pan':
                    t.pan = value(t, c['args'][0]) - 64
                elif n == 'init_pan':
                    t.init_pan = value(t, c['args'][0]) - 64
                elif n == 'span':
                    t.span = value(t, c['args'][0])
                elif n == 'transpose':
                    t.transpose = value(t, c['args'][0])
                elif n == 'pitchbend':
                    t.bend = value(t, c['args'][0])
                elif n == 'bendrange':
                    t.bendrange = value(t, c['args'][0])
                elif n == 'velocity_range':
                    t.velocity_range = value(t, c['args'][0])
                elif n == 'notewait':
                    t.notewait = bool(value(t, c['args'][0]))
                elif n == 'tie':
                    t.tie = bool(value(t, c['args'][0]))
                elif n == 'monophonic':
                    t.mono = bool(value(t, c['args'][0]))
                elif n in ('attack', 'decay', 'sustain', 'release'):
                    t.adsr[['attack', 'decay', 'sustain', 'release'].index(n)] = value(t, c['args'][0])
                elif n == 'env_hold':
                    t.hold = value(t, c['args'][0])
                elif n == 'env_reset':
                    t.adsr, t.hold = [255] * 4, 255
                elif n == 'mod_depth':
                    t.mod['depth'] = value(t, c['args'][0])
                elif n == 'mod_speed':
                    t.mod['speed'] = value(t, c['args'][0])
                elif n == 'mod_type':
                    t.mod['type'] = value(t, c['args'][0])
                elif n == 'mod_range':
                    t.mod['range'] = value(t, c['args'][0])
                elif n == 'mod_delay':
                    t.mod['delay'] = value(t, c['args'][0])
                elif n == 'porta':
                    t.porta, t.porta_key = True, value(t, c['args'][0]) + t.transpose
                elif n == 'porta_sw':
                    t.porta = bool(value(t, c['args'][0]))
                elif n == 'porta_time':
                    t.porta_time = value(t, c['args'][0])
                elif n == 'sweep_pitch':
                    t.sweep = value(t, c['args'][0])
                elif n == 'mute':
                    t.mute = bool(value(t, c['args'][0]))
                elif n == 'damper':
                    down = value(t, c['args'][0]) >= 64
                    damper.setdefault(no, []).append((tick, down))
                elif n in ('fxsend_a', 'fxsend_b', 'fxsend_c'):
                    t.fx['abc'.index(n[-1])] = value(t, c['args'][0])
                elif n == 'mainsend':
                    t.main_send = value(t, c['args'][0])
                elif n == 'lpf_cutoff':
                    t.lpf = value(t, c['args'][0]) - 64
                elif n in CMD_EXT_VAR.values():
                    var, x = c['args']
                    x = value(t, x)
                    arr, i = var_ref(t, var)
                    cur = arr[i]
                    ops = {'setvar': lambda: x, 'addvar': lambda: cur + x, 'subvar': lambda: cur - x,
                           'mulvar': lambda: cur * x, 'divvar': lambda: int(cur / x) if x else cur,
                           'shiftvar': lambda: cur << x if x >= 0 else cur >> -x,
                           'randvar': lambda: int(rng.randint(0, abs(x) + 1)) * (-1 if x < 0 else 1),
                           'andvar': lambda: cur & x, 'orvar': lambda: cur | x, 'xorvar': lambda: cur ^ x,
                           'notvar': lambda: ~x, 'modvar': lambda: cur % x if x else cur}
                    if n in ops:
                        arr[i] = ((ops[n]() + 0x8000) & 0xFFFF) - 0x8000
                    else:
                        t.cmp = {'cmp_eq': cur == x, 'cmp_ge': cur >= x, 'cmp_gt': cur > x, 'cmp_le': cur <= x,
                                 'cmp_lt': cur < x, 'cmp_ne': cur != x}[n]
                else:
                    out['events'].append((tick, no, n, c['args']))
        if all(t.ended for t in tracks.values()):
            break
        tick += 1
    out['ticks'] = tick
    out['timebase'] = timebase
    out['alloc_mask'] = alloc_mask
    out['damper'] = damper
    for nt in out['notes']:
        nt['off_tick'] = release_tick(nt, damper.get(nt['track'], ()))
    return out


def release_tick(note, damper_events):
    """The tick a note's channel is released at (None: never by its length), given its track's
    damper changes [(tick, down)] (SeqTrack::UpdateChannelLength: a channel whose length has
    run out is released on the first tick the damper is up)."""
    if note['length'] <= 0:
        return None
    end = note['tick'] + note['length']
    down = False
    for tk, d in damper_events:   # (the lengths count down before the tick's commands run)
        if tk >= end:
            break
        down = d
    if not down:
        return end
    for tk, d in damper_events:
        if tk >= end and not d:
            return tk + 1
    return None


def tick_to_seconds(tempo_map, timebase):
    """A function tick -> seconds for a tempo map [(tick, bpm)] (in the order the sequence set
    them: of several at one tick, the last holds)."""
    tm = sorted(tempo_map, key=lambda x: x[0])   # (stable: keeps the order within a tick)
    bounds = []
    sec = 0.0
    for i, (tk, bpm) in enumerate(tm):
        bounds.append((tk, sec, bpm))
        if i + 1 < len(tm):
            nxt = tm[i + 1][0]
            sec += (nxt - tk) * 60.0 / (max(bpm, 1) * timebase)

    def f(tick):
        k = 0
        for i, (tk, s, b) in enumerate(bounds):
            if tk <= tick:
                k = i
        tk, s, b = bounds[k]
        return s + (tick - tk) * 60.0 / (max(b, 1) * timebase)
    return f


# ---------------------------------------------------------------------------- output helpers


def write_wav(path, pcm, rate):
    """pcm: float array [channels, samples] or [samples], -1..1."""
    pcm = np.asarray(pcm, dtype=np.float32)
    if pcm.ndim == 1:
        pcm = pcm[None, :]
    data = (np.clip(pcm.T, -1, 1) * 32767).astype('<i2')
    with wave.open(path, 'wb') as w:
        w.setnchannels(pcm.shape[0])
        w.setsampwidth(2)
        w.setframerate(int(rate))
        w.writeframes(data.tobytes())


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--brsar', default=DEFAULT_BRSAR)
    sub = ap.add_subparsers(dest='cmd', required=True)
    s = sub.add_parser('list', help='the sounds')
    s.add_argument('--type', choices=['SEQ', 'STRM', 'WAVE'])
    sub.add_parser('files', help='the files and where they are')
    sub.add_parser('groups', help='the groups and their items')
    s = sub.add_parser('banks', help="each bank's instruments")
    s.add_argument('bank', nargs='?', help='label or id (default: all)')
    s = sub.add_parser('waves', help="a bank's waves to .wav")
    s.add_argument('bank')
    s.add_argument('out')
    s = sub.add_parser('seq', help='a sequence as MML or notes')
    s.add_argument('sound', help='label or id')
    s.add_argument('--notes', action='store_true', help='play it through the sequencer model: a note list')
    s.add_argument('--loops', type=int, default=1)
    s.add_argument('--all-labels', action='store_true', help='disassemble from every label of the file')
    s = sub.add_parser('dump', help="a file's bytes")
    s.add_argument('file', type=lambda x: int(x, 0))
    s.add_argument('out')
    s.add_argument('--wave', action='store_true', help='its wave data instead')
    a = ap.parse_args(argv)
    r = Rsar(a.brsar)

    if a.cmd == 'list':
        print('RSAR %x.%02x: %d sounds, %d banks, %d players, %d files, %d groups; player info %s' % (
            r.version >> 8, r.version & 0xFF, len(r.sounds), len(r.banks), len(r.players), len(r.files),
            len(r.groups), r.player_info))
        for sd in r.sounds:
            if a.type and sd.type_name != a.type:
                continue
            pl = r.players[sd.player]['label'] if sd.player < len(r.players) else '?'
            extra = ''
            if sd.type == 1:
                extra = 'bank %s  seq@%04X  tracks %04X  chprio %d' % (
                    r.banks[sd.bank]['label'] if sd.bank < len(r.banks) else sd.bank, sd.seq_offset, sd.alloc_track,
                    sd.channel_priority)
            elif sd.type == 3:
                extra = 'wave %d' % sd.wave_index
            print('%3d %-34s %-4s %-20s vol %3d prio %3d file %2d (%s) %s' % (
                sd.id, sd.label, sd.type_name, pl, sd.volume, sd.player_priority, sd.file, r.file_kind(sd.file),
                extra))
    elif a.cmd == 'files':
        for f in r.files:
            main_o, wave_o = r.file_offset(f['id'])
            users = [sd.label for sd in r.sounds if sd.file == f['id']] + \
                    ['bank ' + b['label'] for b in r.banks if b['file'] == f['id']]
            print('%2d %-4s size %6X wave %6X at %s %s  %s' % (
                f['id'], r.file_kind(f['id']), f['size'], f['wave_size'],
                '%06X' % main_o if main_o is not None else '-',
                '(wave %06X)' % wave_o if wave_o is not None else '', ', '.join(users[:6]) +
                (' +%d' % (len(users) - 6) if len(users) > 6 else '')))
    elif a.cmd == 'groups':
        for g in r.groups:
            print('group %d %s: at %06X size %X, waves at %06X size %X, %d items' % (
                g['id'], g['label'], g['offset'], g['size'], g['wave_offset'], g['wave_size'], len(g['items'])))
            for it in g['items']:
                print('   file %2d +%06X size %6X wave +%06X size %6X' % (
                    it['file'], it['offset'], it['size'], it['wave_offset'], it['wave_size']))
    elif a.cmd == 'banks':
        for b in r.banks:
            if a.bank and a.bank not in (b['label'], str(b['id'])):
                continue
            bk = r.bank(b['id'])
            print('bank %d %s (file %d): %d programs, %d waves' % (
                b['id'], b['label'], b['file'], bk.nprograms, len(bk.rwar) if bk.rwar else 0))
            for prg in range(bk.nprograms):
                regs = bk.regions(prg)
                if not regs:
                    continue
                print('  prg %d:' % prg)
                for klo, khi, vlo, vhi, ip in regs:
                    wv = bk.wave(ip.wave) if ip.wave >= 0 else None
                    print('    keys %-4s..%-4s vel %3d..%3d  %s  [%s]' % (
                        note_name(klo), note_name(khi), vlo, vhi, ip.short(), wv.describe() if wv else '-'))
    elif a.cmd == 'waves':
        b = next(b for b in r.banks if a.bank in (b['label'], str(b['id'])))
        bk = r.bank(b['id'])
        os.makedirs(a.out, exist_ok=True)
        users = {}
        for prg in range(bk.nprograms):
            for klo, khi, vlo, vhi, ip in bk.regions(prg):
                users.setdefault(ip.wave, []).append('p%d_%s' % (prg, note_name(ip.original_key).replace('#', 's')))
        for i in range(len(bk.rwar)):
            wv = bk.wave(i)
            name = '%03d_%s.wav' % (i, '_'.join(sorted(set(users.get(i, ['unused'])))[:3]))
            write_wav(os.path.join(a.out, name), wv.pcm(), wv.rate)
            print(name, wv.describe())
    elif a.cmd == 'seq':
        sd = r.sound(a.sound)
        if sd.type != 1:
            sys.exit('%s is not a sequence' % sd.label)
        sq = r.sequence(sd.file)
        if a.notes:
            res = simulate(sq.mml, sd.seq_offset, loops=a.loops, alloc_mask=sd.alloc_track)
            print('; %s: %d ticks (timebase %d), tempo %s' % (sd.label, res['ticks'], res['timebase'],
                                                             res['tempo'][:8]))
            print('; loops per track: %s' % {k: v[:2] for k, v in res['loop_tick'].items()})
            for n in res['notes']:
                print('%6d t%-2d prg %3d %-4s v%3d len %4d vol %3d pan %+3d' % (
                    n['tick'], n['track'], n['program'], note_name(n['key']), n['velocity'], n['length'],
                    n['volume'], n['pan']))
        else:
            starts = [(sd.seq_offset, sd.label)]
            if a.all_labels:
                starts += [(o, n) for n, o in sq.labels]
            print('; %s: file %d, bank %s, MML at file+%X (%d bytes), labels %s' % (
                sd.label, sd.file, r.banks[sd.bank]['label'], sq.mml_base, len(sq.mml),
                ', '.join('%s@%X' % l for l in sq.labels)))
            print(disassemble(sq.mml, starts, sq.labels))
    elif a.cmd == 'dump':
        data = r.wave_data(a.file) if a.wave else r.file_data(a.file)
        open(a.out, 'wb').write(data)
        print('%d bytes' % len(data))


if __name__ == '__main__':
    main()
