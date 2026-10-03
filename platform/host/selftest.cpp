// NOMA_SELFTEST=name: run a decompiled routine on known inputs and compare with the expected
// output, before the game starts (checks of the decompilation against reference vectors).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "noma_decomp.h"
#include "runtime.h"
#include "engine/audio/ax_reference.h"
#include "engine/audio/ax_render.h"

namespace guest {

static bool check(const char* what, u32 addr, const char* hex) {
    u8 got[64], want[64];
    size_t n = strlen(hex) / 2;
    for (size_t i = 0; i < n; i++)
        sscanf(hex + 2 * i, "%2hhx", &want[i]);
    read_g(got, addr, (u32)n);
    bool ok = memcmp(got, want, n) == 0;
    char s[200] = {0};
    for (size_t i = 0; i < n; i++)
        snprintf(s + 2 * i, 3, "%02x", got[i]);
    log("selftest %s: %s (%s)", what, ok ? "ok" : "MISMATCH", s);
    return ok;
}

static void put_hex(u32 addr, const char* hex) {
    for (size_t i = 0; hex[2 * i]; i++) {
        unsigned v;
        sscanf(hex + 2 * i, "%2x", &v);
        store8(addr + (u32)i, (u8)v);
    }
}

// NET's AES (NETAESInit / NETAESDecrypt with the CBC mode table), FIPS-197 appendix C.1:
// with a zero IV the first CBC block is plain AES-128.
static void selftest_aes() {
    const u32 ctx = 0x81700000, key = 0x81700200, iv = 0x81700220, in = 0x81700240, out = 0x81700260;
    put_hex(key, "000102030405060708090a0b0c0d0e0f");
    put_hex(iv, "00000000000000000000000000000000");
    put_hex(in, "69c4e0d86a7b0430d8cdb78070b4c55a");
    g::wii_Aes_ctor(ctx, key, 16, iv);
    check("aes key schedule (last round key)", ctx + 0x10 + 40 * 4, "13111d7fe3944a17f307a78b4d2b30c5");
    g::NETAESDecrypt(ctx, out, in, 16);
    check("aes-128 decrypt (ctx, out, in)", out, "00112233445566778899aabbccddeeff");
    // SP 800-38A F.2.2, CBC-AES128.Decrypt: four chained blocks
    const u32 big_in = 0x81700300, big_out = 0x81700400;
    put_hex(key, "2b7e151628aed2a6abf7158809cf4f3c");
    put_hex(iv, "000102030405060708090a0b0c0d0e0f");
    put_hex(big_in, "7649abac8119b246cee98e9b12e9197d5086cb9b507219ee95db113a917678b2"
                    "73bed6b8e3c1743b7116e69e222295163ff1caa1681fac09120eca307586e1a7");
    g::wii_Aes_ctor(ctx, key, 16, iv);
    g::NETAESDecrypt(ctx, big_out, big_in, 64);
    check("aes-128-cbc decrypt, blocks 1-2", big_out,
          "6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51");
    check("aes-128-cbc decrypt, blocks 3-4", big_out + 32,
          "30c81c46a35ce411e5fbc1191a0a52eff69f2445df4f9b17ad2b417be66c3710");
}

static void put_str(u32 addr, const char* str) { write_g(addr, str, (u32)strlen(str) + 1); }

static void check_str(const char* what, u32 addr, const char* want) {
    char got[256];
    read_g(got, addr, sizeof(got));
    got[255] = 0;
    log("selftest %s: %s (\"%s\"%s%s)", what, strcmp(got, want) ? "MISMATCH" : "ok", got,
        strcmp(got, want) ? ", want " : "", strcmp(got, want) ? want : "");
}

// the game's snprintf (MSL vsnprintf through a varargs wrapper), as the User-Agent uses it
static void selftest_printf() {
    const u32 buf = 0x81700000, fmt = 0x81700200, s1 = 0x81700300, s2 = 0x81700320;
    put_str(s1, "14125");
    put_str(s2, "100923141009");
    put_str(fmt, "WM/%s/%s/%016llu");
    g::lib_snprintf(buf, 100, fmt, s1, s2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    check_str("snprintf %016llu of 0", buf, "WM/14125/100923141009/0000000000000000");
    g::lib_snprintf(buf, 100, fmt, s1, s2, 0, 0x00000123u, 0x456789ABu, 0, 0, 0, 0, 0, 0, 0, 0);
    check_str("snprintf %016llu of 0x123456789AB", buf, "WM/14125/100923141009/0001250999896491");
    put_str(fmt, "%d %u %x %5d|%-4d|");
    g::lib_snprintf(buf, 100, fmt, (u32)-42, 42, 0xBEEF, 7, 3, 0, 0, 0, 0, 0, 0, 0, 0);
    check_str("snprintf ints", buf, "-42 42 beef     7|3   |");
    put_str(fmt, "%lld %llx");
    g::lib_snprintf(buf, 100, fmt, 0, 0xFFFFFFFFu, 0xFFFFFFFEu, 0x1u, 0x2u, 0, 0, 0, 0, 0, 0, 0, 0);
    check_str("snprintf %lld -2, %llx 0x100000002", buf, "-2 100000002");
    put_str(fmt, "%.2f %g %5.1f");
    g::lib_snprintf(buf, 100, fmt, 0, 0, 0, 0, 0, 3.14159, 100.0, -2.71, 0, 0, 0, 0, 0);
    check_str("snprintf floats", buf, "3.14 100  -2.7");
}

// a decompiled function's result as a double: an integer result means its prototype lost the
// float return (the test then fails)
static double as_f64(double d) { return d; }
static double as_f64(u32) { return NAN; }
static double as_f64(u64) { return NAN; }

// MSL's libm (fdlibm) against the host's
static void selftest_math() {
    struct Case {
        const char* name;
        double (*host)(double);
        f64 (*guest)(f64);
    };
    static const Case unary[] = {
        {"sin", ::sin, [](f64 x) { return as_f64(g::sin(x)); }},     {"cos", ::cos, [](f64 x) { return as_f64(g::cos(x)); }},
        {"tan", ::tan, [](f64 x) { return as_f64(g::tan(x)); }},     {"atan", ::atan, [](f64 x) { return as_f64(g::atan(x)); }},
        {"asin", ::asin, [](f64 x) { return as_f64(g::asin(x)); }},  {"acos", ::acos, [](f64 x) { return as_f64(g::acos(x)); }},
        {"sqrt", ::sqrt, [](f64 x) { return as_f64(g::sqrt(x)); }},  {"log", ::log, [](f64 x) { return as_f64(g::log(x)); }},
        {"exp", ::exp, [](f64 x) { return as_f64(g::exp(x, 0, 0, 0, 0, 0, 0, 0)); }},
        {"floor", ::floor, [](f64 x) { return as_f64(g::floor(x)); }}, {"ceil", ::ceil, [](f64 x) { return as_f64(g::ceil(x)); }},
    };
    static const double xs[] = {0.0, 0.5, -0.75, 1.0, 2.5, -3.14159, 10.0, 123.456, 1e-5, 0.999};
    int bad = 0, total = 0;
    for (const Case& c : unary)
        for (double x : xs) {
            if ((!strcmp(c.name, "asin") || !strcmp(c.name, "acos")) && fabs(x) > 1)
                continue;
            if ((!strcmp(c.name, "sqrt") || !strcmp(c.name, "log")) && x <= 0)
                continue;
            double want = c.host(x), got = c.guest(x);
            total++;
            if (!(fabs(got - want) <= 1e-12 * fmax(1.0, fabs(want)))) {
                bad++;
                log("selftest math: %s(%g) = %.17g, want %.17g", c.name, x, got, want);
            }
        }
    struct Pair {
        double a, b;
    };
    static const Pair ps[] = {{1, 2}, {-1, -2}, {3.5, -0.25}, {0.1, 10}, {2, 0.5}, {7.5, 2}};
    for (const Pair& q : ps) {
        double w1 = ::atan2(q.a, q.b), g1 = as_f64(g::atan2(q.a, q.b));
        double w2 = ::fmod(q.a, q.b), g2 = as_f64(g::fmod(q.a, q.b));
        double w3 = q.a > 0 ? ::pow(q.a, q.b) : 0, g3 = q.a > 0 ? as_f64(g::pow(q.a, q.b)) : 0;
        total += 3;
        if (!(fabs(g1 - w1) <= 1e-12)) bad++, log("selftest math: atan2(%g,%g) = %.17g, want %.17g", q.a, q.b, g1, w1);
        if (!(fabs(g2 - w2) <= 1e-12)) bad++, log("selftest math: fmod(%g,%g) = %.17g, want %.17g", q.a, q.b, g2, w2);
        if (!(fabs(g3 - w3) <= 1e-12 * fmax(1.0, fabs(w3))))
            bad++, log("selftest math: pow(%g,%g) = %.17g, want %.17g", q.a, q.b, g3, w3);
    }
    log("selftest math: %d of %d %s", total - bad, total, bad ? "MISMATCH" : "ok");
}

// MSL string to number conversions
static void selftest_strtol() {
    const u32 str = 0x81700000, end = 0x81700100, out = 0x81700200, fmt = 0x81700300;
    struct C {
        const char* s;
        int base;
        long want;
    };
    static const C cs[] = {{"12345", 10, 12345}, {"-42", 10, -42}, {"0x1F", 16, 31}, {"0777", 0, 511}, {"  +9xyz", 10, 9}};
    int bad = 0;
    for (const C& c : cs) {
        put_str(str, c.s);
        s32 got = (s32)g::strtol(str, end, (u32)c.base);
        if (got != (s32)c.want)
            bad++, log("selftest strtol(\"%s\", %d) = %d, want %ld", c.s, c.base, got, c.want);
    }
    put_str(str, "9998");
    if ((s32)g::atoi(str) != 9998)
        bad++, log("selftest atoi(\"9998\") = %d", (s32)g::atoi(str));
    put_str(str, "4294967295");
    if (g::strtoul(str, end, 10) != 0xFFFFFFFFu)
        bad++, log("selftest strtoul(\"4294967295\") = %u", g::strtoul(str, end, 10));
    put_str(str, "17 0x20 abc");
    put_str(fmt, "%d %x %s");
    u32 n = g::sscanf(str, fmt, out, out + 4, out + 8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    char word[8] = {0};
    read_g(word, out + 8, 4);
    if (n != 3 || load32(out) != 17 || load32(out + 4) != 0x20 || strcmp(word, "abc"))
        bad++, log("selftest sscanf: %u fields, %u %u \"%s\"", n, load32(out), load32(out + 4), word);
    // sscanf %f / %lf: MSL's narrow __strtold core (__strtold)
    put_str(str, "3.25e2 0.1x");
    put_str(fmt, "%f %lf");
    n = g::sscanf(str, fmt, out, out + 8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    u32 fb = load32(out);
    f32 f;
    memcpy(&f, &fb, 4);
    u64 db = (u64)load32(out + 8) << 32 | load32(out + 12);
    double d;
    memcpy(&d, &db, 8);
    if (n != 2 || f != 325.0f || d != 0.1)
        bad++, log("selftest sscanf %%f %%lf: %u fields, %.9g %.17g", n, (double)f, d);
    // wcstof (the wide __wcstold core): wcstof, on a UTF-16 string
    const char* w = "3.25e2xyz";
    for (u32 i = 0; i <= strlen(w); i++)
        store16(str + 2 * i, (u8)w[i]);
    double sf = as_f64(g::wcstof(str, end));
    if (sf != 325.0 || load32(end) != str + 12)
        bad++, log("selftest wcstof(L\"3.25e2xyz\") = %g, end +%d", sf, (int)(load32(end) - str));
    log("selftest strtol/atoi/strtoul/sscanf/wcstof: %s", bad ? "MISMATCH" : "ok");
}

// the CodeWarrior 64-bit runtime helpers (r3:r4 op r5:r6)
static void selftest_int64() {
    struct C {
        u64 a, b;
    };
    static const C cs[] = {{0x123456789ABull, 10}, {1250999896491ull, 1000}, {0xFFFFFFFFFFFFFFFFull, 7},
                           {0x100000002ull, 16}, {100, 0x100000000ull}, {0x8000000000000000ull, 3}};
    int bad = 0;
    for (const C& c : cs) {
        u64 q = g::__div2u((u32)(c.a >> 32), (u32)c.a, (u32)(c.b >> 32), (u32)c.b);
        u64 r = g::__mod2u((u32)(c.a >> 32), (u32)c.a, (u32)(c.b >> 32), (u32)c.b);
        // NOMA_U64 packing: r3 (the high word) in the low half of the C value
        u64 qv = ((u64)(u32)q << 32) | (u32)(q >> 32), rv = ((u64)(u32)r << 32) | (u32)(r >> 32);
        if (qv != c.a / c.b || rv != c.a % c.b)
            bad++, log("selftest int64: %llx / %llx = %llx r %llx, want %llx r %llx", (unsigned long long)c.a,
                       (unsigned long long)c.b, (unsigned long long)qv, (unsigned long long)rv,
                       (unsigned long long)(c.a / c.b), (unsigned long long)(c.a % c.b));
    }
    struct S {
        s64 a, b;
    };
    static const S ss[] = {{-1250999896491ll, 1000}, {1250999896491ll, -7}, {-100, -3}, {-0x7FFFFFFFFFFFFFFFll, 2},
                           {0x123456789ll, 0x10000}};
    for (const S& c : ss) {
        u64 ua = (u64)c.a, ub = (u64)c.b;
        u64 q = g::__div2i((u32)(ua >> 32), (u32)ua, (u32)(ub >> 32), (u32)ub);
        u64 r = g::__mod2i((u32)(ua >> 32), (u32)ua, (u32)(ub >> 32), (u32)ub);
        s64 qv = (s64)(((u64)(u32)q << 32) | (u32)(q >> 32)), rv = (s64)(((u64)(u32)r << 32) | (u32)(r >> 32));
        if (qv != c.a / c.b || rv != c.a % c.b)
            bad++, log("selftest int64: %lld / %lld = %lld r %lld, want %lld r %lld", (long long)c.a, (long long)c.b,
                       (long long)qv, (long long)rv, (long long)(c.a / c.b), (long long)(c.a % c.b));
    }
    for (u32 sh : {0u, 1u, 31u, 32u, 33u, 63u}) {
        u64 v = 0x8123456789ABCDEFull;
        u64 l = g::__shl2i((u32)(v >> 32), (u32)v, sh);
        u64 lv = ((u64)(u32)l << 32) | (u32)(l >> 32);
        if (lv != (v << sh))
            bad++, log("selftest int64: %llx << %u = %llx, want %llx", (unsigned long long)v, sh,
                       (unsigned long long)lv, (unsigned long long)(v << sh));
    }
    log("selftest int64 __div2u/__mod2u/__div2i/__mod2i/__shl2i: %s", bad ? "MISMATCH" : "ok");
}

// OSCalendarTimeToTicks: calendar arithmetic over 64-bit ticks (bus clock 243 MHz)
static void selftest_time() {
    const u32 td = 0x81700000;
    // {sec, min, hour, mday, mon (0-11), year, wday, yday, msec, usec}
    const s32 f[10] = {56, 34, 12, 2, 9, 2026, 0, 0, 789, 0};
    for (int i = 0; i < 10; i++)
        store32(td + 4 * i, (u32)f[i]);
    u64 t = g::OSCalendarTimeToTicks(td);
    u64 tv = ((u64)(u32)t << 32) | (u32)(t >> 32);
    const u64 want = 51288776579931750ull;  // (2026-10-02 12:34:56 - 2000-01-01) s * 60.75 MHz + 789 ms
    log("selftest OSCalendarTimeToTicks: %s (%llu%s)", tv == want ? "ok" : "MISMATCH", (unsigned long long)tv,
        tv == want ? "" : ", want 51288776579931750");
    // and back: the same ticks as a calendar (a Friday, the 275th day of the year)
    for (int i = 0; i < 10; i++)
        store32(td + 4 * i, 0xDEADBEEF);
    g::OSTicksToCalendarTime((u32)(want >> 32), (u32)want, td);
    const s32 back[10] = {56, 34, 12, 2, 9, 2026, 5, 274, 789, 0};
    bool ok = true;
    char got[128];
    int n = 0;
    for (int i = 0; i < 10; i++) {
        s32 v = (s32)load32(td + 4 * i);
        ok &= v == back[i];
        n += snprintf(got + n, sizeof(got) - n, "%s%d", i ? " " : "", v);
    }
    log("selftest OSTicksToCalendarTime: %s (%s%s)", ok ? "ok" : "MISMATCH", got,
        ok ? "" : "; want 56 34 12 2 9 2026 5 274 789 0");
}

// a Mii database from a converted 3DS Mii: RFL's own CRC routine must find it intact, and its
// validity checks must take the Mii (the channel's Mii list skips any they reject)
static void selftest_rfl() {
    // a 3DS Mii: female, born 3/14, light blue, named "Noma", with the Mii Maker's default parts
    u8 m3[0x5C] = {3};
    auto put_le = [&](u32 at, u32 v, int n) {
        for (int i = 0; i < n; i++)
            m3[at + i] = (u8)(v >> (8 * i));
    };
    put_le(0x0C, 0x12345678, 4);  // Mii ID
    static const u8 mac[6] = {0x00, 0x1F, 0x32, 0xAB, 0xCD, 0xEF};
    memcpy(m3 + 0x10, mac, 6);
    put_le(0x18, 1 | 3 << 1 | 14 << 5 | 8 << 10, 2);
    const char* name = "Noma";
    for (int i = 0; name[i]; i++)
        m3[0x1A + 2 * i] = (u8)name[i];
    m3[0x2E] = 64, m3[0x2F] = 64, m3[0x30] = 2 << 1 | 1 << 5, m3[0x32] = 30, m3[0x33] = 2;
    // eyes: type 2, scale 4, y-scale 3, rotation 4, spacing 2, height 12
    put_le(0x34, 2 | 4 << 9 | 3 << 13 | 4 << 16 | 2 << 21 | 12 << 25, 4);
    // eyebrows: type 6, color 1, scale 4, y-scale 3, rotation 6, spacing 2, height 10
    put_le(0x38, 6 | 1 << 5 | 4 << 8 | 3 << 12 | 6 << 16 | 2 << 20 | 10 << 24, 4);
    put_le(0x3C, 1 | 4 << 5 | 9 << 9, 2);         // nose: type 1, scale 4, height 9
    put_le(0x3E, 3 | 4 << 9 | 3 << 13, 2);        // mouth: type 3, scale 4, y-scale 3
    put_le(0x40, 13, 2);                          // mouth height 13
    put_le(0x42, 4 << 6 | 10 << 10, 2);           // beard: scale 4, height 10
    put_le(0x44, 4 << 7 | 10 << 11, 2);           // glasses: scale 4, height 10
    put_le(0x46, 4 << 1 | 20 << 5 | 2 << 10, 2);  // mole (off): scale 4, x 2, y 20
    u8 w[0x4A];
    mii_3ds_to_wii(m3, w);
    const u8* list[1] = {w};
    std::vector<u8> db = rfl_make_database(list, 1);
    const u32 at = 0x81000000;
    write_g(at, db.data(), (u32)db.size());
    u32 crc = g::RFLiCalculateCRC(at, (u32)db.size()) & 0xFFFF;
    // RFL's reading of a slot (RFLiGetCharInfo): the character data as RFL's info, then its checks
    const u32 info = 0x81100000;
    memset_g(info, 0, 0x100);
    g::RFLiConvertRaw2Info(at + 4, info);
    bool usable = g::RFLiCheckValidInfo(info) != 0 && g::RFLiIsValidID(info + 0x46) != 0;
    bool ok = rfl_database_valid(db) && crc == 0 && load16(at + 4) == (u16)(1 << 14 | 3 << 10 | 14 << 5 | 8 << 1) &&
              load16(at + 4 + 2) == 'N' && usable;
    log("selftest RFL_DB.dat: %s (game's CRC over the file %04X, Mii word %04X, RFL %s the Mii)",
        ok ? "ok" : "MISMATCH", crc, load16(at + 4), usable ? "takes" : "REJECTS");
    // NOMA_WRITE_RFLDB=path: keep it (a test database for runs without the 3DS Mii Selector)
    if (const char* out = getenv("NOMA_WRITE_RFLDB")) {
        if (FILE* f = fopen(out, "wb")) {
            fwrite(db.data(), 1, db.size(), f);
            fclose(f);
        }
    }
}

// the SDK's paired-single PSMTXQuat (PSMTXQuat): reads both halves of registers that scalar
// single-precision instructions wrote (fres, fadds), which Broadway fills in both
static void selftest_mtx() {
    auto bits = [](f32 f) {
        u32 b;
        memcpy(&b, &f, 4);
        return b;
    };
    auto flt = [](u32 b) {
        f32 f;
        memcpy(&f, &b, 4);
        return f;
    };
    static const double qs[][4] = {
        {0, 0, 0, 1}, {0.5, 0.5, 0.5, 0.5}, {0.1, 0.2, 0.3, 0.927362}, {-0.6, 0.3, 0.2, 0.7}, {1, 2, 3, 4},
    };
    const u32 m = 0x81700000, q = 0x81700040;
    int bad = 0;
    for (const auto& v : qs) {
        for (int i = 0; i < 4; i++)
            store32(q + 4 * i, bits((f32)v[i]));
        g::PSMTXQuat(m, q);
        double x = v[0], y = v[1], z = v[2], w = v[3];
        double s = 2 / (x * x + y * y + z * z + w * w);
        double xs = x * s, ys = y * s, zs = z * s, wx = w * xs, wy = w * ys, wz = w * zs;
        double xx = x * xs, xy = x * ys, xz = x * zs, yy = y * ys, yz = y * zs, zz = z * zs;
        const double want[12] = {1 - (yy + zz), xy - wz, xz + wy, 0, xy + wz, 1 - (xx + zz),
                                 yz - wx, 0, xz - wy, yz + wx, 1 - (xx + yy), 0};
        for (int i = 0; i < 12; i++) {
            double got = flt(load32(m + 4 * i));
            if (!(fabs(got - want[i]) <= 1e-5)) {
                bad++;
                log("selftest mtx: PSMTXQuat(%g,%g,%g,%g)[%d][%d] = %.9g, want %.9g", x, y, z, w, i / 4, i % 4,
                    got, want[i]);
            }
        }
    }
    log("selftest mtx: PSMTXQuat %s", bad ? "MISMATCH" : "ok");
}

// ---- AX voices: engine/audio/ax_render.cpp against the sample-by-sample definition
// (engine/audio/ax_reference.h) on random voices, several frames each: sample for sample and PB
// word for PB word, they must agree.
namespace axref = engine::audio::reference;

static void selftest_ax() {
    // the samples straddle a 16 MB page boundary (the renderer reads RAM a page at a time)
    const u32 data = 0x90FF8000, data_size = 0x10000, pb_ref = 0x81700000, pb_nat = 0x81700400;
    axref::Rng rnd{12345};
    for (u32 i = 0; i < data_size; i++)
        store8(data + i, (u8)rnd());
    int bad = 0, frames = 0;
    for (int t = 0; t < 4000 && bad < 5; t++) {
        u16 w[0xD6 / 2];
        axref::random_voice(rnd, w, data, data_size);
        u16 fmt = w[0x70 / 2];
        for (u32 k = 0; k < 0xD6 / 2; k++)
            store16(pb_ref + 2 * k, w[k]), store16(pb_nat + 2 * k, w[k]);
        for (int f = 0; f < 4 && bad < 5; f++, frames++) {
            static s32 ref_bus[12][axref::SPF];
            static engine::audio::AxBuses nat_bus;
            memset(ref_bus, 0, sizeof(ref_bus));
            memset(nat_bus, 0, sizeof(nat_bus));
            axref::Pb pb;
            pb.read(pb_ref);
            axref::process_voice(pb, ref_bus);
            pb.write();
            engine::audio::ax_render_voice(pb_nat, nat_bus);
            bool same = memcmp(ref_bus, nat_bus, sizeof(ref_bus)) == 0;
            for (u32 k = 0; k < 0xD6 && same; k += 2)
                same = load16(pb_ref + k) == load16(pb_nat + k);
            if (!same) {
                bad++;
                log("selftest ax: voice %d frame %d differs (format %02X, src %u, ratio %08X)", t, f, fmt,
                    w[0x08 / 2], (u32)w[0xA6 / 2] << 16 | w[0xA8 / 2]);
            }
        }
    }
    log("selftest ax: %d voice frames, %s", frames, bad ? "MISMATCH" : "ok");

    // speed, on voices like the room's: looped ADPCM resampled by about 1.1, into main L and R
    auto room_voice = [&](u32 pb) {
        u32 phys = data & 0x1FFFFFFF;
        for (u32 k = 0; k < 0xD6; k += 2)
            store16(pb + k, 0);
        store16(pb + 0x10, 1), store32(pb + 0x0C, 3), store16(pb + 0x14, 0x6000), store16(pb + 0x18, 0x5000);
        store16(pb + 0x6A, 0x48EF), store16(pb + 0x6E, 1);
        store32(pb + 0x72, phys * 2 + 2), store32(pb + 0x76, phys * 2 + 0x1F000), store32(pb + 0x7A, phys * 2 + 2);
        for (u32 k = 0; k < 16; k++)
            store16(pb + 0x7E + 2 * k, (u16)(k & 1 ? -1200 : 2400));
        store32(pb + 0xA6, 0x11EF0);
    };
    room_voice(pb_ref), room_voice(pb_nat);
    const int FRAMES = 20000;
    static s32 bus[12][axref::SPF];
    u64 t0 = host_time_us();
    for (int f = 0; f < FRAMES; f++) {
        axref::Pb pb;
        pb.read(pb_ref);
        axref::process_voice(pb, bus);
        pb.write();
    }
    u64 t1 = host_time_us();
    for (int f = 0; f < FRAMES; f++)
        engine::audio::ax_render_voice(pb_nat, *(engine::audio::AxBuses*)bus);
    u64 t2 = host_time_us();
    log("selftest ax: a room voice's frame takes %.2f us (sample by sample: %.2f us)",
        (double)(t2 - t1) / FRAMES, (double)(t1 - t0) / FRAMES);
}

bool run_selftest(const std::string& name) {
    g_sp = 0x81710000;
    if (name == "aes")
        selftest_aes();
    else if (name == "printf")
        selftest_printf();
    else if (name == "math")
        selftest_math();
    else if (name == "strtol")
        selftest_strtol();
    else if (name == "int64")
        selftest_int64();
    else if (name == "time")
        selftest_time();
    else if (name == "rfl")
        selftest_rfl();
    else if (name == "mtx")
        selftest_mtx();
    else if (name == "ax")
        selftest_ax();
    else if (name == "all") {
        selftest_rfl();
        selftest_time();
        selftest_int64();
        selftest_aes();
        selftest_printf();
        selftest_math();
        selftest_strtol();
        selftest_mtx();
        selftest_ax();
    }
    else
        return false;
    return true;
}

}  // namespace guest
