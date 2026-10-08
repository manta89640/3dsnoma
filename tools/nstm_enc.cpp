// nstm-enc: 32 kHz PCM to an NSTM stream (engine/audio/nstm.h), DSP-ADPCM encoded.
//
//   nstm-enc OUT --channels N [--loop START] [--bpm BPM] [--first-beat SECONDS] [--bar BEATS] < PCM
//
// PCM: signed 16-bit little-endian, N channels interleaved, 32 kHz (tools/music/stage_music.py
// makes it). --loop: the piece loops from START (a sample) at its end; without it, it ends.
//
// The encoder. Each channel gets its own 8 predictor pairs, found the way a vector quantiser is
// trained: every 14-sample frame has the statistics of a 2nd-order linear prediction of its samples
// from the two before (sums of products), a frame goes to the pair that predicts it best, and each
// pair becomes the least-squares predictor of its frames (weighted by 1 / the frame's error, so
// that the cost is the frames' log error: the bits their scales take, not the loud frames alone),
// a dozen rounds from a spread of seeds. The frames are then encoded in order against the decoder's
// own history (what the DSP will have decoded, not the original samples), each with the pair and
// scale (around the one its prediction's largest error needs) that decode closest to it.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "engine/audio/nstm.h"

using namespace engine::audio;

namespace {

struct Stats {
    double aa = 0, ab = 0, bb = 0, ya = 0, yb = 0, yy = 0;
    void add(const Stats& s, double w) {
        aa += w * s.aa;
        ab += w * s.ab;
        bb += w * s.bb;
        ya += w * s.ya;
        yb += w * s.yb;
        yy += w * s.yy;
    }
    // the squared error of predicting y = c1 a + c2 b
    double error(double c1, double c2) const {
        return yy - 2 * c1 * ya - 2 * c2 * yb + c1 * c1 * aa + 2 * c1 * c2 * ab + c2 * c2 * bb;
    }
    // the least-squares (c1, c2); false if the system is singular
    bool solve(double& c1, double& c2) const {
        double det = aa * bb - ab * ab;
        if (std::fabs(det) < 1e-9 * (aa * bb + 1))
            return false;
        c1 = (ya * bb - yb * ab) / det;
        c2 = (yb * aa - ya * ab) / det;
        return true;
    }
};

const double SEEDS[8][2] = {{0, 0},         {0.9375, 0},  {1.796875, -0.8125}, {1.53125, -0.859375},
                            {1.9, -0.9375}, {1.2, -0.35}, {0.5, 0},            {1.97, -0.985}};

double clamp_coef(double c) { return c > 15.99 ? 15.99 : c < -16 ? -16 : c; }

void design(const std::vector<int16_t>& x, int16_t coefs[16]) {
    size_t n = x.size(), frames = (n + 13) / 14;
    std::vector<Stats> st(frames);
    for (size_t f = 0; f < frames; f++) {
        Stats& s = st[f];
        for (size_t i = f * 14; i < std::min(n, f * 14 + 14); i++) {
            double y = x[i], a = i >= 1 ? x[i - 1] : 0, b = i >= 2 ? x[i - 2] : 0;
            s.aa += a * a;
            s.ab += a * b;
            s.bb += b * b;
            s.ya += y * a;
            s.yb += y * b;
            s.yy += y * y;
        }
    }
    double c[8][2];
    std::memcpy(c, SEEDS, sizeof(c));
    std::vector<int> best(frames);
    std::vector<double> err(frames);
    for (int round = 0; round < 12; round++) {
        Stats pooled[8];
        int members[8] = {};
        size_t worst = 0;
        for (size_t f = 0; f < frames; f++) {
            int k = 0;
            double e = st[f].error(c[0][0], c[0][1]);
            for (int j = 1; j < 8; j++) {
                double ej = st[f].error(c[j][0], c[j][1]);
                if (ej < e) {
                    e = ej;
                    k = j;
                }
            }
            best[f] = k;
            err[f] = e;
            if (e > err[worst])
                worst = f;
            pooled[k].add(st[f], 1.0 / (std::max(e, 0.0) + 14 * 4.0));
            members[k]++;
        }
        for (int j = 0; j < 8; j++) {
            double c1, c2;
            if (members[j] && pooled[j].solve(c1, c2)) {
                c[j][0] = clamp_coef(c1);
                c[j][1] = clamp_coef(c2);
            } else if (!members[j] && st[worst].solve(c1, c2)) {  // an empty pair: the worst frame's own
                c[j][0] = clamp_coef(c1);
                c[j][1] = clamp_coef(c2);
                err[worst] = 0;
            }
        }
    }
    for (int j = 0; j < 8; j++) {
        coefs[2 * j] = (int16_t)std::lround(c[j][0] * 2048);
        coefs[2 * j + 1] = (int16_t)std::lround(c[j][1] * 2048);
    }
}

// one frame (count samples of x, zero-padded) against the history h1, h2; the bytes to out
void encode_frame(const int16_t* x, int count, const int16_t coefs[16], int16_t& h1, int16_t& h2, uint8_t out[8]) {
    int16_t in[14] = {};
    std::memcpy(in, x, count * sizeof(int16_t));
    double best_err = 1e300;
    uint8_t best[8] = {};
    int16_t best_h1 = h1, best_h2 = h2;
    for (int p = 0; p < 8; p++) {
        int c1 = coefs[2 * p], c2 = coefs[2 * p + 1];
        // the scale its open-loop prediction needs
        int need = 0;
        {
            int a = h1, b = h2;
            for (int k = 0; k < 14; k++) {
                int pred = (c1 * a + c2 * b + 1024) >> 11;
                int r = std::abs(in[k] - pred);
                while (need < 12 && r > 7 * (1 << need))
                    need++;
                b = a;
                a = in[k];
            }
        }
        for (int s = std::max(0, need - 1); s <= std::min(12, need + 1); s++) {
            int scale = 1 << s;
            uint8_t bytes[8] = {(uint8_t)(p << 4 | s)};
            int a = h1, b = h2;
            double e = 0;
            for (int k = 0; k < 14; k++) {
                int base = 1024 + c1 * a + c2 * b;
                long target = (long)in[k] * 2048 - base;
                int q = (int)std::lround((double)target / (scale * 2048.0));
                int best_q = 0, best_out = 0;
                long best_d = -1;
                for (int t = q - 1; t <= q + 1; t++) {
                    int qq = t < -8 ? -8 : t > 7 ? 7 : t;
                    int v = (qq * scale * 2048 + base) >> 11;
                    v = v > 32767 ? 32767 : v < -32768 ? -32768 : v;
                    long d = std::labs((long)v - in[k]);
                    if (best_d < 0 || d < best_d) {
                        best_d = d;
                        best_q = qq;
                        best_out = v;
                    }
                }
                e += (double)best_d * best_d;
                bytes[1 + (k >> 1)] |= (uint8_t)((best_q & 0xF) << (k & 1 ? 0 : 4));
                b = a;
                a = best_out;
                if (e >= best_err)
                    break;
            }
            if (e < best_err) {
                best_err = e;
                std::memcpy(best, bytes, 8);
                best_h1 = (int16_t)a;
                best_h2 = (int16_t)b;
            }
        }
    }
    std::memcpy(out, best, 8);
    h1 = best_h1;
    h2 = best_h2;
}

int usage() {
    std::fprintf(stderr, "usage: nstm-enc OUT --channels N [--loop START] [--bpm BPM] [--first-beat S] [--bar N] < PCM\n");
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2)
        return usage();
    std::string out_path = argv[1];
    int channels = 0;
    long loop = -1;
    double bpm = 0, first_beat = 0;
    int bar = 4;
    for (int i = 2; i + 1 < argc; i += 2) {
        std::string k = argv[i];
        if (k == "--channels")
            channels = std::atoi(argv[i + 1]);
        else if (k == "--loop")
            loop = std::atol(argv[i + 1]);
        else if (k == "--bpm")
            bpm = std::atof(argv[i + 1]);
        else if (k == "--first-beat")
            first_beat = std::atof(argv[i + 1]);
        else if (k == "--bar")
            bar = std::atoi(argv[i + 1]);
        else
            return usage();
    }
    if (channels < 1 || channels > 2)
        return usage();

    std::vector<int16_t> pcm;
    int16_t buf[8192];
    size_t got;
    while ((got = std::fread(buf, sizeof(int16_t), 8192, stdin)) > 0)
        pcm.insert(pcm.end(), buf, buf + got);
    size_t samples = pcm.size() / channels;
    if (!samples) {
        std::fprintf(stderr, "nstm-enc: no samples\n");
        return 1;
    }
    if (loop >= (long)samples)
        loop = -1;

    std::vector<std::vector<int16_t>> ch(channels, std::vector<int16_t>(samples));
    for (size_t i = 0; i < samples; i++)
        for (int c = 0; c < channels; c++)
            ch[c][i] = pcm[i * channels + c];

    NstmHeader h = {};
    h.magic = NSTM_MAGIC;
    h.version = NSTM_VERSION;
    h.channels = (uint16_t)channels;
    h.rate = NSTM_RATE;
    h.samples = (uint32_t)samples;
    h.loop_start = loop >= 0 ? (uint32_t)loop : 0;
    h.loops = loop >= 0;
    h.beats_per_bar = (uint8_t)bar;
    h.bpm = (float)bpm;
    h.first_beat = (float)first_beat;
    h.data_offset = sizeof(NstmHeader) + channels * sizeof(NstmChannel);

    std::vector<NstmChannel> info(channels);
    size_t frames = (samples + 13) / 14;
    size_t loop_frame = h.loop_start / 14;
    std::vector<uint8_t> data(frames * channels * 8);
    for (int c = 0; c < channels; c++) {
        design(ch[c], info[c].coefs);
        int16_t h1 = 0, h2 = 0;
        for (size_t f = 0; f < frames; f++) {
            if (f == loop_frame) {
                info[c].loop_hist1 = h1;
                info[c].loop_hist2 = h2;
            }
            int count = (int)std::min<size_t>(14, samples - f * 14);
            encode_frame(&ch[c][f * 14], count, info[c].coefs, h1, h2, &data[(f * channels + c) * 8]);
        }
    }

    // the error, for the log: decode it all again
    double sig = 0, noise = 0;
    for (int c = 0; c < channels; c++) {
        int16_t h1 = 0, h2 = 0, dec[14];
        for (size_t f = 0; f < frames; f++) {
            nstm_decode_frame(&data[(f * channels + c) * 8], info[c].coefs, h1, h2, dec);
            for (size_t k = 0; k < 14 && f * 14 + k < samples; k++) {
                double v = ch[c][f * 14 + k];
                sig += v * v;
                noise += (dec[k] - v) * (dec[k] - v);
            }
        }
    }

    FILE* f = std::fopen(out_path.c_str(), "wb");
    if (!f) {
        std::perror(out_path.c_str());
        return 1;
    }
    std::fwrite(&h, sizeof(h), 1, f);
    std::fwrite(info.data(), sizeof(NstmChannel), channels, f);
    std::fwrite(data.data(), 1, data.size(), f);
    std::fclose(f);
    std::printf("%s: %zu samples x %d, %s, SNR %.1f dB\n", out_path.c_str(), samples, channels,
                loop >= 0 ? "loops" : "ends", noise > 0 ? 10 * std::log10(sig / noise) : 99.0);
    return 0;
}
