// NOMA_SELFTEST=name: run a decompiled routine on known inputs and compare with the expected
// output, before the game starts (checks of the decompilation against reference vectors).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "noma_decomp.h"
#include "runtime.h"
#include "engine/audio/ax_reference.h"
#include "engine/audio/ax_render.h"
#include "engine/cx/cx_reference.h"
#include "hw/gx/raster.h"
#include "hw/gx/texture.h"
#include "platform/3ds/pica_tex.h"

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
    // a Mii picked in the Mii Selector goes in with the others: another one into the next slot,
    // the same one again (Mii and console ID) into its own slot, and the file stays valid
    u8 w2[0x4A];
    memcpy(w2, w, sizeof(w2));
    w2[0x1B] ^= 1, w2[0x02] = 0, w2[0x03] = 'M';  // another Mii ID, named "M"
    std::vector<u8> two = rfl_add_mii(db, w2);
    w2[0x16] = 99;  // ... and again, taller
    std::vector<u8> again = rfl_add_mii(two, w2);
    auto slot = [](const std::vector<u8>& d, int i) { return &d[4 + 0x4A * i]; };
    bool add_ok = rfl_database_valid(two) && rfl_database_valid(again) && memcmp(slot(two, 0), w, 0x4A) == 0 &&
                  memcmp(slot(two, 1), w2, 0x16) == 0 && memcmp(slot(again, 0), w, 0x4A) == 0 &&
                  memcmp(slot(again, 1), w2, 0x4A) == 0 && slot(again, 2)[2] == 0 && slot(again, 2)[3] == 0;
    log("selftest RFL_DB.dat adding a Mii: %s", add_ok ? "ok" : "MISMATCH");
    // the 3DS's Mii Maker database synced in: a Mii already there is updated in its slot, new ones
    // take the empty slots, a Mii the list doesn't have stays; in a full database a new one takes
    // the slot of the first Mii not in the list
    u8 maker[3][0x4A];
    memcpy(maker[0], w2, 0x4A), maker[0][0x16] = 50;                     // slot 1's Mii, changed
    memcpy(maker[1], w, 0x4A), maker[1][0x1B] ^= 2, maker[1][0x03] = 'A';  // new
    memcpy(maker[2], w, 0x4A), maker[2][0x1B] ^= 4, maker[2][0x03] = 'B';  // new
    std::vector<u8> synced = rfl_sync_miis(again, &maker[0][0], 3);
    bool sync_ok = rfl_database_valid(synced) && memcmp(slot(synced, 0), w, 0x4A) == 0 &&
                   memcmp(slot(synced, 1), maker[0], 0x4A) == 0 && memcmp(slot(synced, 2), maker[1], 0x4A) == 0 &&
                   memcmp(slot(synced, 3), maker[2], 0x4A) == 0 && slot(synced, 4)[2] == 0 && slot(synced, 4)[3] == 0 &&
                   rfl_sync_miis(synced, &maker[0][0], 3) == synced;
    std::vector<u8> hundred(100 * 0x4A);
    const u8* ptrs[100];
    for (int i = 0; i < 100; i++) {
        u8* m = &hundred[0x4A * i];
        memcpy(m, w, 0x4A), m[0x1A] = (u8)(i + 1), m[0x03] = (u8)('a' + i % 26);
        ptrs[i] = m;
    }
    std::vector<u8> full = rfl_sync_miis(rfl_make_database(ptrs, 100), &maker[1][0], 1);
    sync_ok = sync_ok && rfl_database_valid(full) && memcmp(slot(full, 0), maker[1], 0x4A) == 0 &&
              memcmp(slot(full, 1), ptrs[1], 0x4A) == 0;
    log("selftest RFL_DB.dat syncing the 3DS's Miis: %s", sync_ok ? "ok" : "MISMATCH");
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
    const u32 data = 0x90FF8000, data_size = 0x10000, pb_ref = 0x81700000, pb_nat4 = 0x81700400;
    axref::Rng rnd{12345};
    axref::test_data(rnd, data, data_size);
    int bad = 0, frames = 0;
    for (int t = 0; t < 4000 && bad < 5; t++) {
        u16 w[0xD6 / 2];
        if (!axref::edge_voice(t, w, data))
            axref::random_voice(rnd, w, data, data_size);
        u16 fmt = w[0x70 / 2];
        // (every fourth voice's PB 2-byte aligned only: rendered from an aligned copy)
        const u32 pb_nat = t % 4 == 3 ? pb_nat4 + 0x202 : pb_nat4;
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
    const u32 pb_nat = pb_nat4;
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

// ---- AX frames: runtime/hw/dsp_ax.cpp's command lists (SETUP, the voices, OUTPUT) against the
// definitions in engine/audio/ax_reference.h, frame after frame: the buses' ramps (as a rule all
// zero, now and then not), voices of every kind (the PB list), the master volume at 1.0, at
// another volume, and changing (a ramp over the frame); the output, the surround buffer and every
// PB must agree.
void dsp_ax_command_list(u32 list_addr, u32 words);

static void selftest_axframe() {
    const u32 data = 0x90FF8000, data_size = 0x10000;
    const u32 list = 0x81710000, setup_buf = 0x81710200, surround = 0x81710400, lr = 0x81710600;
    const u32 surround_ref = 0x81710800, lr_ref = 0x81710A00, pbs_nat = 0x81720000, pbs_ref = 0x81730000;
    axref::Rng rnd{4242};
    axref::test_data(rnd, data, data_size);
    static s32 ref_buf[12][axref::SPF];
    u16 last_master = 0x8000, volume = 0x8000;
    int bad = 0, frames = 0, voice_no = 0;
    for (int f = 0; f < 600 && bad < 5; f++, frames++) {
        // the set-up: zero, or ramps
        bool ramps = rnd() % 4 == 0;
        for (u32 b = 0; b < 12; b++) {
            u32 v = ramps && rnd() % 2 ? rnd() * 37u : 0;
            store16(setup_buf + 6 * b, (u16)(v >> 16)), store16(setup_buf + 6 * b + 2, (u16)v);
            store16(setup_buf + 6 * b + 4, (u16)(ramps && rnd() % 2 ? rnd() : 0));
        }
        // a new voice list now and then (PBs 0x140 apart, as the SDK lays them out)
        static u32 voices = 0;
        if (f % 8 == 0) {
            voices = rnd() % 6;
            for (u32 v = 0; v < voices; v++) {
                u16 w[0xD6 / 2];
                if (!axref::edge_voice(voice_no++, w, data))
                    axref::random_voice(rnd, w, data, data_size);
                for (u32 k = 0; k < 0x140; k += 2) {
                    u16 x = k < 0xD6 ? w[k / 2] : 0;
                    store16(pbs_nat + 0x140 * v + k, x), store16(pbs_ref + 0x140 * v + k, x);
                }
                u32 next = v + 1 < voices ? 0x140 * (v + 1) : 0;
                store32(pbs_nat + 0x140 * v, next ? pbs_nat + next : 0);
                store32(pbs_ref + 0x140 * v, next ? pbs_ref + next : 0);
            }
        }
        // the master volume: as a rule 1.0; now and then another, held or changed every frame
        u32 k = rnd() % 8;
        volume = k < 5 ? 0x8000 : k == 5 ? volume : (u16)(rnd() % 3 ? rnd() % 0x10000 : rnd() % 0x8001);
        u32 a = list;
        auto cmd16 = [&](u16 v) { store16(a, v), a += 2; };
        auto cmd32 = [&](u32 v) { cmd16((u16)(v >> 16)), cmd16((u16)v); };
        cmd16(0x00), cmd32(setup_buf);
        cmd16(0x04), cmd32(voices ? pbs_nat : 0);
        cmd16(0x0B), cmd16(volume), cmd32(surround), cmd32(lr);
        cmd16(0x0E);
        dsp_ax_command_list(list, (a - list) / 2);
        // the definition
        axref::setup(ref_buf, setup_buf);
        for (u32 v = 0; v < voices; v++) {
            axref::Pb pb;
            pb.read(pbs_ref + 0x140 * v);
            axref::process_voice(pb, ref_buf);
            pb.write();
        }
        axref::output(ref_buf, last_master, surround_ref, lr_ref, volume);
        bool same = true;
        for (u32 i = 0; i < 4 * axref::SPF && same; i += 4)
            same = load32(surround + i) == load32(surround_ref + i) && load32(lr + i) == load32(lr_ref + i);
        for (u32 i = 0; i < 0x140 * voices && same; i += 2)  // (but each list's links)
            same = i % 0x140 < 4 || load16(pbs_nat + i) == load16(pbs_ref + i);
        if (!same) {
            bad++;
            log("selftest axframe: frame %d differs (%u voices, master volume %04X, buses %s)", f, voices, volume,
                ramps ? "ramping" : "zero");
        }
    }
    log("selftest axframe: %d frames, %s", frames, bad ? "MISMATCH" : "ok");
}

namespace cxref = engine::cx::reference;

// A random LZ10 (ex = false) or LZ11 stream of about n bytes, made token by token. kind 0: valid;
// 1: its last match runs past the end; 2: random bytes after the header. lit_bias: literals in 8
// tokens (-1: any).
static std::vector<u8> lz_random_stream(axref::Rng& rnd, bool ex, u32 n, int kind, u32* out_size,
                                        int lit_bias = -1) {
    std::vector<u8> out, s;
    u32 size = n;
    s.push_back(ex ? 0x11 : 0x10);
    if (rnd() & 1) {  // the long header: 0 in the short size, the size in 4 more bytes
        s.insert(s.end(), {0, 0, 0, (u8)size, (u8)(size >> 8), (u8)(size >> 16), (u8)(size >> 24)});
    } else {
        s.insert(s.end(), {(u8)size, (u8)(size >> 8), (u8)(size >> 16)});
    }
    *out_size = size;
    if (kind == 2) {
        for (u32 i = 0; i < n; i++)
            s.push_back((u8)rnd());
        return s;
    }
    if (lit_bias < 0)
        lit_bias = (int)(rnd() % 8);  // streams from mostly literals to mostly matches
    size_t flag_at = 0;
    for (int bit = 8; out.size() < size; bit++) {
        if (bit == 8) {
            flag_at = s.size();
            s.push_back(0);
            bit = 0;
        }
        u32 left = size - (u32)out.size();
        if (out.empty() || (int)(rnd() % 8) < lit_bias) {
            u8 b = (u8)(rnd() % 6 ? rnd() : out.empty() ? 0 : out.back());
            s.push_back(b), out.push_back(b);
            continue;
        }
        s[flag_at] |= 0x80 >> bit;
        u32 dist = 1 + rnd() % std::min<u32>(4096, (u32)out.size());
        if (rnd() % 4 == 0)
            dist = 1 + rnd() % std::min<u32>(4, (u32)out.size());  // overlapping copies
        u32 length, form = rnd() % 16;
        if (!ex)
            length = 3 + rnd() % 16;
        else if (form < 10)
            length = 3 + rnd() % 14;
        else if (form < 14)
            length = 0x11 + rnd() % 0x100;
        else
            length = 0x111 + rnd() % (form == 14 ? 0x1000 : 0x10000);
        if (length > left && !(kind == 1 && out.size() + 64 > size))
            length = left < 3 ? 0 : left;  // the stream's end: a literal, or an exact last match
        if (length == 0 || (!ex && length > 18) || (ex && length > 0x10110)) {
            s[flag_at] &= ~(0x80 >> bit);
            u8 b = (u8)rnd();
            s.push_back(b), out.push_back(b);
            continue;
        }
        u32 d = dist - 1;
        if (!ex) {
            s.push_back((u8)((length - 3) << 4 | d >> 8));
        } else if (length <= 16 && length >= 3) {
            s.push_back((u8)((length - 1) << 4 | d >> 8));
        } else if (length <= 0x110) {
            u32 l = length - 0x11;
            s.push_back((u8)(l >> 4)), s.push_back((u8)((l & 0xF) << 4 | d >> 8));
        } else {
            u32 l = length - 0x111;
            s.push_back((u8)(0x10 | l >> 12)), s.push_back((u8)(l >> 4)), s.push_back((u8)((l & 0xF) << 4 | d >> 8));
        }
        s.push_back((u8)d);
        for (u32 i = 0; i < length; i++)
            out.push_back(out[out.size() - dist]);
    }
    for (u32 pad = rnd() % 48; pad; pad--)  // what follows the data in the file (more than 0x20: an error)
        s.push_back((u8)rnd());
    return s;
}

// A random Huffman stream of n symbols (bit_size 4 or 8) with a random tree in CX's layout: the
// size byte, the root at +1, each node's children as a pair (offset in pairs from its own pair,
// bit 7: the left child is a leaf, bit 6: the right). kind 0: valid; 1: random tree bytes; 2:
// random bits after a valid tree. Returns no stream if the random tree didn't fit the layout.
// Symbols are drawn alike (mode 0), by random bits from the root (1: short codes as common as
// in real data, a glyph sheet's average about 2 bits), or (2) like a glyph sheet: a symbol with a
// code of one bit, in long runs, among the others drawn by random bits.
static std::vector<u8> huff_random_stream(axref::Rng& rnd, u32 bit_size, u32 n, int kind, u32* out_size,
                                          int mode = 0) {
    struct Node {
        int child[2] = {-1, -1};
        int sym = -1;
    };
    std::vector<Node> nodes;
    std::vector<int> pool;
    u32 symbols = 2 + rnd() % ((1u << bit_size) - 1);
    for (u32 i = 0; i < symbols; i++) {
        nodes.push_back(Node{{-1, -1}, (int)(rnd() % (1u << bit_size))});
        pool.push_back((int)i);
    }
    int background = -1;  // mode 2: the symbol with a code of one bit, joined last
    if (mode == 2) {
        background = pool.back();
        pool.pop_back();
        if (pool.empty())
            return {};
    }
    while (pool.size() > 1) {  // random merges: from balanced to deep, lopsided codes
        size_t a = rnd() % pool.size();
        int na = pool[a];
        pool.erase(pool.begin() + (long)a);
        size_t b = rnd() % 4 == 0 ? pool.size() - 1 : rnd() % pool.size();
        int nb = pool[b];
        pool.erase(pool.begin() + (long)b);
        nodes.push_back(Node{{na, nb}, -1});
        pool.push_back((int)nodes.size() - 1);
    }
    if (background >= 0) {
        nodes.push_back(rnd() & 1 ? Node{{background, pool[0]}, -1} : Node{{pool[0], background}, -1});
        pool[0] = (int)nodes.size() - 1;
    }
    // the layout, breadth first: the slot of each node, its children's pair after it
    std::vector<u8> t(2, 0);  // [0] the size byte, [1] the root
    std::vector<std::pair<int, size_t>> queue{{pool[0], 1}};
    std::vector<std::vector<int>> code(nodes.size());
    for (size_t q = 0; q < queue.size(); q++) {
        auto [id, slot] = queue[q];
        const Node& nd = nodes[(size_t)id];
        if (nd.sym >= 0)
            continue;
        // sometimes a gap before the children (unused bytes, zeros: they pass CXiVerifyHuffmanTable)
        if (rnd() % 8 == 0 && (t.size() - (slot & ~(size_t)1)) / 2 < 0x3F)
            t.insert(t.end(), {0, 0});
        size_t pair = t.size();
        u32 off = (u32)((pair - (slot & ~(size_t)1)) / 2 - 1);
        if (off > 0x3F)
            return {};
        t.resize(pair + 2);
        u8 node = (u8)off;
        for (int b = 0; b < 2; b++) {
            const Node& c = nodes[(size_t)nd.child[b]];
            code[(size_t)nd.child[b]] = code[(size_t)id];
            code[(size_t)nd.child[b]].push_back(b);
            if (c.sym >= 0)
                node |= (u8)(0x80 >> b), t[pair + (size_t)b] = (u8)c.sym;
            queue.push_back({nd.child[b], pair + (size_t)b});
        }
        t[slot] = node;  // (an internal child's own byte is written when its turn comes)
    }
    if (t.size() > 0x200)
        return {};
    t[0] = (u8)(t.size() / 2 - 1);
    if (kind == 1)
        for (size_t i = 1; i < t.size(); i++)
            t[i] = (u8)rnd();
    std::vector<int> leaves;
    for (size_t i = 0; i < nodes.size(); i++)
        if (nodes[i].sym >= 0)
            leaves.push_back((int)i);
    u32 size = n * bit_size / 8;
    *out_size = size;
    std::vector<u8> s{(u8)(0x20 | bit_size), (u8)size, (u8)(size >> 8), (u8)(size >> 16)};
    s.insert(s.end(), t.begin(), t.end());
    u32 w = 0, bits = 0;
    auto put = [&](int bit) {
        w |= (u32)bit << (31 - bits);
        if (++bits == 32) {
            s.insert(s.end(), {(u8)w, (u8)(w >> 8), (u8)(w >> 16), (u8)(w >> 24)});
            w = 0, bits = 0;
        }
    };
    for (u32 i = 0; i < n + 64; i++) {  // (more symbols than needed: the last word is complete)
        if (kind == 2) {
            put((int)(rnd() & 1));
            continue;
        }
        int leaf = leaves[rnd() % leaves.size()];
        if (mode == 2) {  // runs of the background (on average 40), single other symbols between
            static bool s_in_run;
            s_in_run = s_in_run ? rnd() % 40 != 0 : rnd() % 2 == 0;
            if (s_in_run)
                leaf = background;
        }
        if (mode && leaf != background)
            for (leaf = pool[0]; nodes[(size_t)leaf].sym < 0;)
                leaf = nodes[(size_t)leaf].child[rnd() & 1];
        for (int b : code[(size_t)leaf])
            put(b);
    }
    return s;
}

// One compressed stream (LZ or Huffman) decoded by the native and the streaming code in the same
// random pieces: false at the first return value, context or output byte (or one around the
// output) that differs
static bool cx_compare(axref::Rng& rnd, const u8* data, u32 n, const char* name) {
    const u32 in = 0x90100000, out_ref = 0x90400000, out_nat = 0x90B00000, margin = 0x1000;
    const u32 ctx_ref = 0x81710000, ctx_nat = 0x81720000;  // (contexts of 0x220 bytes)
    bool huffman = (data[0] & 0xF0) == 0x20;
    u32 size = data[1] | data[2] << 8 | data[3] << 16;
    if (n > 0x600000 || size > 0x600000 || !size)
        return true;
    write_g(in, data, n);
    for (u32 i = 0; i < size + 2 * margin; i += 4) {
        u32 v = rnd();
        store32(out_ref - margin + i, v), store32(out_nat - margin + i, v);
    }
    for (u32 c : {ctx_ref, ctx_nat}) {
        for (u32 k = 0; k < 0x220; k += 4)
            store32(c + k, 0);
        store32(c, c == ctx_ref ? out_ref : out_nat);
        if (huffman)  // CXInitUncompContextHuffman (0x802FFBC0)
            store32(c + 0x0C, c + 0x20), store16(c + 0x18, (u16)-1), store8(c + 0x1D, 8);
        else  // CXInitUncompContextLZ (0x802FFB80)
            store8(c + 0x10, 3), store8(c + 0x13, 8);
    }
    u32 piece_max = std::vector<u32>{1, 33, 4096, 0x20000, 0x1000000}[rnd() % 5];
    for (u32 at = 0; at < n;) {
        u32 len = std::min<u32>(n - at, 1 + rnd() % piece_max);
        s32 r_ref = 0, r_nat = 0;
        if (huffman) {
            cxref::read_uncomp_huffman(ctx_ref, in + at, len);
            g::CXReadUncompHuffman(ctx_nat, in + at, len);
        } else {
            r_ref = (s32)cxref::read_uncomp_lz(ctx_ref, in + at, len);
            r_nat = (s32)g::CXReadUncompLZ(ctx_nat, in + at, len);
        }
        bool same = r_ref == r_nat && load32(ctx_ref) - out_ref == load32(ctx_nat) - out_nat;
        for (u32 k = 4; k < 0x220 && same; k++)
            same = huffman && k >= 0x0C && k < 0x10 ? load32(ctx_ref + 0x0C) - ctx_ref == load32(ctx_nat + 0x0C) - ctx_nat
                                                    : load8(ctx_ref + k) == load8(ctx_nat + k);
        if (!same) {
            log("selftest cx: %s: differs after input %u..%u", name, at, at + len);
            return false;
        }
        at += len;
        if (huffman ? (s32)load32(ctx_ref + 0x04) <= 0 : r_ref <= 0)
            break;
    }
    for (u32 i = 0; i < size + 2 * margin; i += 4)
        if (load32(out_ref - margin + i) != load32(out_nat - margin + i)) {
            log("selftest cx: %s: output differs at %d", name, (s32)(i - margin));
            return false;
        }
    return true;
}

// The game's own compressed data, from the contents in ./content (or NOMA_SELFTEST_DATA/content):
// every LZ file in their archives and every Huffman glyph sheet of the fonts
static void selftest_cx_game_data(axref::Rng& rnd) {
    const char* dir = getenv("NOMA_SELFTEST_DATA");
    std::string base = std::string(dir ? dir : ".") + "/content/";
    int lz = 0, sheets = 0, bad = 0;
    for (u32 index = 0; index < 32 && bad < 3; index++) {
        char name[32];
        snprintf(name, sizeof(name), "%08x.app", index);
        FILE* f = fopen((base + name).c_str(), "rb");
        if (!f)
            continue;
        std::vector<u8> d;
        u8 buf[65536];
        for (size_t got; (got = fread(buf, 1, sizeof(buf), f)) > 0;)
            d.insert(d.end(), buf, buf + got);
        fclose(f);
        auto be = [&](size_t at) { return (u32)d[at] << 24 | d[at + 1] << 16 | d[at + 2] << 8 | d[at + 3]; };
        if (d.size() < 32 || be(0) != 0x55AA382D)
            continue;
        u32 root = be(4), nodes = be(root + 8);
        for (u32 i = 1; i < nodes && bad < 3; i++) {
            size_t e = root + i * 12;
            if (d[e] != 0)
                continue;
            u32 off = be(e + 4), size = be(e + 8);
            if ((u64)off + size > d.size() || size < 8)
                continue;
            const u8* p = d.data() + off;
            std::string what = std::string(name) + "@" + std::to_string(off);
            if ((p[0] == 0x10 || p[0] == 0x11) && p[1] | p[2] | p[3]) {
                lz++;
                bad += !cx_compare(rnd, p, size, what.c_str());
            } else if (size > 0x20 && !memcmp(p, "RFNA", 4)) {
                for (u32 k = 0; k + 4 <= size && bad < 3; k += 4)  // the sheets: Huffman, 64 KB each
                    if ((p[k] == 0x28 || p[k] == 0x24) && p[k + 1] == 0 && p[k + 2] == 0 && p[k + 3] == 1) {
                        sheets++;
                        bad += !cx_compare(rnd, p + k, size - k, (what + "+" + std::to_string(k)).c_str());
                    }
            }
        }
    }
    if (!lz && !sheets)
        log("selftest cx: no game data in %s (run where content/ is, or set NOMA_SELFTEST_DATA)", base.c_str());
    else
        log("selftest cx: game data: %d LZ files, %d Huffman glyph sheets, %s", lz, sheets, bad ? "MISMATCH" : "ok");
}

// The CX natives' fast paths against the streaming code they started from (cx_reference.h):
// random streams fed in random pieces, comparing each return value, the context and the output
// buffer with what lies around it (nothing may be written past the end); then the game's own data.
static void selftest_cx() {
    const u32 in = 0x90100000, ctx_ref = 0x81700000, ctx_nat = 0x81700040;
    const u32 out_ref = 0x90400000, out_nat = 0x90800000, margin = 0x10000;
    axref::Rng rnd{777};
    int bad = 0, streams = 0;
    u64 calls = 0;
    for (int t = 0; t < 3000 && bad < 5; t++, streams++) {
        bool ex = rnd() & 1;
        int kind = t % 10 == 9 ? 2 : t % 10 == 8 ? 1 : 0;
        u32 n = t % 50 == 0 ? 0x40000 + rnd() % 0x40000 : 1 + rnd() % (rnd() % 4 ? 600 : 0x4000), size;
        std::vector<u8> s = lz_random_stream(rnd, ex, n, kind, &size);
        write_g(in, s.data(), (u32)s.size());
        u32 force = rnd() % 5 == 0 ? 1 + rnd() % size : 0;
        for (u32 i = 0; i < size + 2 * margin; i += 4) {
            u32 v = rnd();
            store32(out_ref - margin + i, v), store32(out_nat - margin + i, v);
        }
        for (u32 c : {ctx_ref, ctx_nat}) {  // CXInitUncompContextLZ (0x802FFB80)
            for (u32 k = 0; k < 0x18; k += 4)
                store32(c + k, 0);
            store32(c + 0x00, c == ctx_ref ? out_ref : out_nat);
            store32(c + 0x08, force);
            store8(c + 0x10, 3);
            store8(c + 0x13, 8);
        }
        u32 piece_max = std::vector<u32>{1, 7, 33, 100, 4096, 0x100000}[rnd() % 6];
        for (u32 at = 0; at < s.size() && bad < 5;) {
            u32 len = std::min<u32>((u32)s.size() - at, 1 + rnd() % piece_max);
            calls++;
            s32 r_ref = (s32)cxref::read_uncomp_lz(ctx_ref, in + at, len);
            s32 r_nat = (s32)g::CXReadUncompLZ(ctx_nat, in + at, len);
            bool same = r_ref == r_nat && load32(ctx_ref) - out_ref == load32(ctx_nat) - out_nat;
            for (u32 k = 4; k < 0x15 && same; k++)
                same = load8(ctx_ref + k) == load8(ctx_nat + k);
            if (!same) {
                bad++;
                log("selftest cx: LZ%d stream %d (kind %d, %u bytes) differs at input %u: returns %d / %d",
                    ex ? 11 : 10, t, kind, size, at, r_ref, r_nat);
                break;
            }
            at += len;
            if (r_ref <= 0)
                break;
        }
        for (u32 i = 0; i < size + 2 * margin && !bad; i += 4)
            if (load32(out_ref - margin + i) != load32(out_nat - margin + i)) {
                bad++;
                log("selftest cx: LZ%d stream %d (kind %d, %u bytes): output differs at %d", ex ? 11 : 10, t, kind,
                    size, (s32)(i - margin));
            }
    }
    log("selftest cx: %d LZ streams in %llu pieces, %s", streams, (unsigned long long)calls, bad ? "MISMATCH" : "ok");

    bad = 0, streams = 0, calls = 0;
    for (int t = 0; t < 3000 && bad < 5; t++) {
        u32 bit_size = rnd() & 1 ? 8 : 4, size;
        int kind = t % 10 == 9 ? 2 : t % 10 == 8 ? 1 : 0;
        u32 n = 8 * (1 + rnd() % (t % 50 == 0 ? 0x10000 : rnd() % 4 ? 200 : 4000));
        std::vector<u8> s = huff_random_stream(rnd, bit_size, n, kind, &size, t % 3);
        if (s.empty())
            continue;
        streams++;
        write_g(in, s.data(), (u32)s.size());
        // a tree that doesn't verify ends the data (the game gives up; fed on, both walk it
        // anywhere in memory)
        if (!g::CXiVerifyHuffmanTable(in + 4, bit_size))
            s.resize(4 + (load8(in + 4) + 1u) * 2);
        u32 force = rnd() % 5 == 0 ? 1 + rnd() % size : 0;
        for (u32 i = 0; i < size + 2 * margin; i += 4) {
            u32 v = rnd();
            store32(out_ref - margin + i, v), store32(out_nat - margin + i, v);
        }
        const u32 hctx_ref = 0x81710000, hctx_nat = 0x81720000;  // (contexts of 0x220 bytes)
        for (u32 c : {hctx_ref, hctx_nat}) {  // CXInitUncompContextHuffman (0x802FFBC0)
            for (u32 k = 0; k < 0x220; k += 4)
                store32(c + k, 0);
            store32(c + 0x00, c == hctx_ref ? out_ref : out_nat);
            store32(c + 0x08, force);
            store32(c + 0x0C, c + 0x20);
            store16(c + 0x18, (u16)-1);
            store8(c + 0x1D, 8);
        }
        u32 piece_max = std::vector<u32>{1, 5, 64, 600, 0x2000, 0x100000}[rnd() % 6];
        for (u32 at = 0; at < s.size() && bad < 5;) {
            u32 len = std::min<u32>((u32)s.size() - at, 1 + rnd() % piece_max);
            calls++;
            cxref::read_uncomp_huffman(hctx_ref, in + at, len);
            g::CXReadUncompHuffman(hctx_nat, in + at, len);
            bool same = load32(hctx_ref) - out_ref == load32(hctx_nat) - out_nat &&
                        load32(hctx_ref + 0x0C) - hctx_ref == load32(hctx_nat + 0x0C) - hctx_nat;
            for (u32 k = 4; k < 0x220 && same; k++)
                same = k >= 0x0C && k < 0x10 ? true : load8(hctx_ref + k) == load8(hctx_nat + k);
            if (!same) {
                bad++;
                log("selftest cx: Huffman%u stream %d (kind %d, %u bytes) differs at input %u", bit_size, t, kind,
                    size, at);
                break;
            }
            at += len;
            if ((s32)load32(hctx_ref + 0x04) <= 0)
                break;
        }
        for (u32 i = 0; i < size + 2 * margin && !bad; i += 4)
            if (load32(out_ref - margin + i) != load32(out_nat - margin + i)) {
                bad++;
                log("selftest cx: Huffman%u stream %d (kind %d, %u bytes): output differs at %d", bit_size, t, kind,
                    size, (s32)(i - margin));
            }
    }
    log("selftest cx: %d Huffman streams in %llu pieces, %s", streams, (unsigned long long)calls,
        bad ? "MISMATCH" : "ok");
    selftest_cx_game_data(rnd);

    // speed, on 2 MB of LZ10 like the game's (half literals, 3.5:1), fed in 8 KB pieces as files are read
    u32 size;
    axref::Rng r2{99};
    std::vector<u8> s = lz_random_stream(r2, false, 2u << 20, 0, &size, 4);
    write_g(in, s.data(), (u32)s.size());
    u64 us[2];
    for (int which = 0; which < 2; which++) {
        u32 c = which ? ctx_nat : ctx_ref;
        u64 t0 = host_time_us();
        for (int rep = 0; rep < 4; rep++) {
            for (u32 k = 0; k < 0x18; k += 4)
                store32(c + k, 0);
            store32(c, which ? out_nat : out_ref), store8(c + 0x10, 3), store8(c + 0x13, 8);
            for (u32 at = 0; at < s.size(); at += 0x2000) {
                u32 len = std::min<u32>((u32)s.size() - at, 0x2000);
                if (which)
                    g::CXReadUncompLZ(c, in + at, len);
                else
                    cxref::read_uncomp_lz(c, in + at, len);
            }
        }
        us[which] = host_time_us() - t0;
    }
    log("selftest cx: LZ10 at %.0f MB/s (byte by byte: %.0f MB/s; %.1f:1)", 4.0 * size / us[1], 4.0 * size / us[0],
        (double)size / s.size());
}

// lib_allocator_Fill (engine/system/allocator.cpp) against the loop it replaces (0x8018A13C): the
// fill value's bits in every word, 0xFF in the last size & 3 bytes, nothing around touched, the end
// returned; any fill value, sizes from 0 to 1 MB, aligned and unaligned blocks
static void selftest_fill() {
    axref::Rng rnd{4242};
    const u32 base = 0x90200000, margin = 64;
    std::vector<u8> want;
    int bad = 0, cases = 0;
    for (int t = 0; t < 3000 && bad < 5; t++, cases++) {
        u32 value = t % 3 == 0 ? 0x7FFFFFFF : rnd();
        u32 size = t % 100 == 0 ? rnd() % 0x100000 : t % 7 == 0 ? rnd() % 8 : rnd() % 70000;
        u32 dst = base + margin + (t % 5 == 0 ? rnd() % 4 : (rnd() % 64) * 4);
        store32(GA_lib_allocator_sFillValue, value);
        want.resize(size + 2 * margin);
        for (u32 i = 0; i < want.size(); i++)
            want[i] = (u8)rnd();
        write_g(dst - margin, want.data(), (u32)want.size());
        for (u32 i = 0; i + 4 <= (size & ~3u); i += 4)  // the loop: a word at a time, then bytes
            want[margin + i] = (u8)(value >> 24), want[margin + i + 1] = (u8)(value >> 16),
            want[margin + i + 2] = (u8)(value >> 8), want[margin + i + 3] = (u8)value;
        for (u32 i = size & ~3u; i < size; i++)
            want[margin + i] = 0xFF;
        u32 end = g::lib_allocator_Fill(dst, size);
        std::vector<u8> got(want.size());
        read_g(got.data(), dst - margin, (u32)got.size());
        if (end != dst + size || got != want) {
            bad++;
            size_t at = std::mismatch(got.begin(), got.end(), want.begin()).first - got.begin();
            log("selftest fill: %u bytes at %08X (value %08X): returns %08X, first difference at %d", size, dst,
                value, end, (int)at - (int)margin);
        }
    }
    log("selftest fill: %d fills, %s", cases, bad ? "MISMATCH" : "ok");
}

// wii_Thread_GetUnusedStackSize (engine/system/thread.cpp) against the guest's loop: from the stack's
// base a word at a time while it holds 0xDEADBABE, the last word never counted; stacks of 0 to 64 KB at
// any alignment, used from the top down to any depth, with words left untouched inside the used part
// and stray writes inside the fill
static void selftest_stack() {
    axref::Rng rnd{777};
    const u32 thread = 0x90180000, area = 0x90200000;
    int bad = 0, cases = 0;
    for (int t = 0; t < 4000 && bad < 5; t++, cases++) {
        u32 size = t % 50 == 0 ? rnd() % 16 : rnd() % 0x10000;
        u32 base = area + (t % 4 == 0 ? rnd() % 4 : (rnd() % 64) * 8);
        for (u32 i = 0; i + 4 <= size + 8; i += 4)
            store32(base + i, 0xDEADBABE);
        u32 depth = rnd() % 3 == 0 ? 0 : rnd() % (size + 1);  // bytes used from the top
        for (u32 a = base + size - depth; a + 4 <= base + size; a += 4)
            if (rnd() % 8)
                store32(a & ~3u, rnd());
        for (int k = rnd() % 3; k > 0 && size >= 4; k--)
            store8(base + rnd() % size, (u8)rnd());
        store32(thread + 0x318, base), store32(thread + 0x31C, size);
        u32 want = 0;
        for (u32 p = base;;) {
            u32 w = load32(p);
            p += 4;
            if (w != 0xDEADBABE || p >= base + size)
                break;
            want++;
        }
        want *= 4;
        u32 got = g::wii_Thread_GetUnusedStackSize(thread);
        if (got != want) {
            bad++;
            log("selftest stack: %u bytes at %08X: %u, the loop %u", size, base, got, want);
        }
    }
    log("selftest stack: %d stacks, %s", cases, bad ? "MISMATCH" : "ok");
}

// OSCalcCRC16 (engine/os/crc.cpp, a byte at a time) against the SDK's loop it replaces (half a byte
// at a time through the DOL's table of 16): random blocks of 0 B to 1 MB at any alignment, and the
// channel's save file size
// gx_copy_to_texture_lookup (the 3DS's EFB copies from its read-back) against gx_copy_to_texture on
// random copies: every format, full and half size, intensity, rectangles cut by the block grid
namespace {
const u32 *s_efb_px, *s_efb_col, *s_efb_row;
void efb_fetch(u32 x, u32 y, s32* out) {
    u32 p = s_efb_px[s_efb_col[x & 1023] + s_efb_row[y & 1023]];
    out[0] = (s32)(p >> 24), out[1] = (s32)((p >> 16) & 255), out[2] = (s32)((p >> 8) & 255), out[3] = (s32)(p & 255);
}
}  // namespace
static void selftest_efb() {
    u32 rng = 12345;
    auto next = [&] { return rng = rng * 1103515245u + 12345u, rng >> 8; };
    std::vector<u32> px(240 * 400), col(1024), row(1024);
    std::vector<u8> a, b;
    u32 bad = 0, n = 0;
    for (int t = 0; t < 3000; t++) {
        for (u32& p : px)
            p = next() ^ next() << 16;
        for (u32 i = 0; i < 1024; i++)
            col[i] = std::min<u32>(i * 400 / 640, 399) * 240, row[i] = 239 - std::min<u32>(i * 240 / 456, 239);
        EfbCopy c;
        static const u32 fmts[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
        c.fmt = fmts[t % 13];
        c.w = 1 + next() % 300, c.h = 1 + next() % 300, c.sx = next() % 340, c.sy = next() % 200;
        c.half = t % 5 == 1, c.intensity = t % 7 == 2;
        u32 w = c.half ? std::max<u32>(1, c.w / 2) : c.w, h = c.half ? std::max<u32>(1, c.h / 2) : c.h;
        c.stride = (w + 7) / 8 * 64;
        size_t size = (size_t)c.stride * ((h + 7) / 4 + 2);
        a.assign(size, 0xCD), b.assign(size, 0xCD);
        s_efb_px = px.data(), s_efb_col = col.data(), s_efb_row = row.data();
        gx_copy_to_texture(c, efb_fetch, a.data());
        gx_copy_to_texture_lookup(c, px.data(), col.data(), row.data(), b.data());
        n++;
        if (a != b && bad++ < 4)
            log("selftest efb: fmt %u %ux%u half %u intensity %u differs", c.fmt, c.w, c.h, (u32)c.half, (u32)c.intensity);
    }
    log("selftest efb: %u copies, %s", n, bad ? "DIFFERENT" : "ok");
}

// The 3DS renderer's texture conversions (platform/3ds/pica_tex.h): straight from the GX tiles must
// give the texture memory that decoding then tiling gives, for every format, size and palette; CMPR
// as ETC1/ETC1A4 is lossy: decoded with the PICA's layout (as Azahar's decoder reads it) it must
// stay close to the CMPR texels, and its alpha exact
static void selftest_tex() {
    using namespace pica_tex;
    u32 rng = 7;
    auto next = [&] { return rng = rng * 1103515245u + 12345u, rng >> 8 ^ rng << 13; };
    auto p2 = [](u32 v) { u32 p = 8; while (p < v && p < 1024) p <<= 1; return p; };
    auto fmt_of = [](u32 f, u32 tlut) -> Fmt {
        if (f == 8 || f == 9 || f == 10) return tlut == 0 ? LA8 : tlut == 1 ? RGB565 : RGBA8;
        switch (f) { case 0: return L4; case 1: return L8; case 2: return LA4; case 3: return LA8; case 4: return RGB565; case 14: return RGBA5551; default: return RGBA8; }
    };
    const u32 fmts[] = {0, 1, 2, 3, 4, 5, 6, 8, 9, 10, 14};
    u8* tmem = gx_tmem_for_tests();
    std::vector<u8> src, a, b;
    std::vector<u32> rgba;
    u32 bad = 0, n = 0;
    for (int t = 0; t < 2000; t++) {
        u32 f = fmts[t % 11], w = 1 + next() % (t % 7 == 0 ? 400 : 120), h = 1 + next() % (t % 5 == 0 ? 300 : 100);
        u32 W = p2(w), H = p2(h), tlut_fmt = next() % 3, tlut_addr = (next() % 1500) << 9;
        for (u32 i = 0; i < 0x10000; i++) tmem[(tlut_addr + i) & ((1u << 20) - 1)] = (u8)next();
        src.resize(gx_texture_bytes(f, w, h));
        for (u8& c : src) c = (u8)next();
        Fmt pf = fmt_of(f, tlut_fmt);
        size_t nb = (size_t)W * H * texel_bits(pf) / 8;
        a.assign(nb, 0xCD), b.assign(nb, 0xCD), rgba.assign((size_t)w * h, 0);
        gx_decode_texture_tlut(src.data(), f, w, h, tlut_addr, tlut_fmt, rgba.data());
        from_rgba(rgba.data(), w, h, a.data(), W, H, pf, false);
        bool ok = from_gx(src.data(), f, w, h, tmem, GX_TMEM_MASK, tlut_addr, tlut_fmt, b.data(), W, H, pf);
        n++;
        if ((!ok || a != b) && bad++ < 4) log("selftest tex: format %u %ux%u differs", f, w, h);
    }
    log("selftest tex: %u textures straight from GX tiles, %s", n, bad ? "DIFFERENT" : "ok");

    // The textures as the PICA reads them, a reader of its own (Azahar's decoder: 8x8 tiles in rows
    // of W / 8, a tile's texels in Morton order, its memory rows from the texture's top, t = 1, down,
    // so that image row y, sampled at t = y / H, is memory row H - 1 - y; little-endian texels), not
    // pica_tex's layout tables: every texel of W x H must be the GX decoder's texel in the precision
    // of the format it is kept in, the image's edge texel past its right and bottom edges (GX's
    // clamp), a repeating texture's nearest texel (from_rgba's resampling), each smaller image of a
    // grey texture the 2x2 average of the one before inside and its border's own texels on the
    // border (mip_down); a luminance texture with its alpha read from its red, as the combiners read
    // it (tev::remap_luminance).
    auto morton = [](u32 x, u32 y) {
        static const u8 xs[8] = {0x00, 0x01, 0x04, 0x05, 0x10, 0x11, 0x14, 0x15};
        static const u8 ys[8] = {0x00, 0x02, 0x08, 0x0A, 0x20, 0x22, 0x28, 0x2A};
        return (u32)xs[x & 7] + ys[y & 7];
    };
    auto rgba_of = [](u32 r, u32 g, u32 bl, u32 al) { return r | g << 8 | bl << 16 | al << 24; };
    auto c4 = [](u32 v) { return v * 17; };
    auto c5 = [](u32 v) { return v << 3 | v >> 2; };
    auto c6 = [](u32 v) { return v << 2 | v >> 4; };
    // texel (x, image row y) of a W x H PICA texture in format f
    auto read_pica = [&](const u8* tex, Fmt f, u32 W, u32 H, u32 x, u32 y) -> u32 {
        const u32 my = H - 1 - y, m = morton(x, my);
        const u8* tile = tex + ((size_t)(my / 8) * (W / 8) + x / 8) * 64 * texel_bits(f) / 8;
        switch (f) {
        case RGBA8: {
            const u8* p = tile + 4 * m;
            return rgba_of(p[3], p[2], p[1], p[0]);
        }
        case RGB565: {
            const u32 v = tile[2 * m] | tile[2 * m + 1] << 8;
            return rgba_of(c5(v >> 11), c6(v >> 5 & 63), c5(v & 31), 255);
        }
        case RGBA5551: {
            const u32 v = tile[2 * m] | tile[2 * m + 1] << 8;
            return rgba_of(c5(v >> 11), c5(v >> 6 & 31), c5(v >> 1 & 31), (v & 1) * 255);
        }
        case LA8: return rgba_of(tile[2 * m + 1], tile[2 * m + 1], tile[2 * m + 1], tile[2 * m]);
        case LA4: return rgba_of(c4(tile[m] >> 4), c4(tile[m] >> 4), c4(tile[m] >> 4), c4(tile[m] & 15));
        case L8: return rgba_of(tile[m], tile[m], tile[m], tile[m]);
        case L4: {
            const u32 l = c4(m & 1 ? tile[m / 2] >> 4 : tile[m / 2] & 15);
            return rgba_of(l, l, l, l);
        }
        default: return 0;
        }
    };
    // a GX texel (texture.cpp's RGBA8) in the precision of format f
    auto kept_as = [&](u32 c, Fmt f) -> u32 {
        const u32 r = c & 255, g = c >> 8 & 255, bl = c >> 16 & 255, al = c >> 24;
        switch (f) {
        case RGB565: return rgba_of(c5(r >> 3), c6(g >> 2), c5(bl >> 3), 255);
        case RGBA5551: return rgba_of(c5(r >> 3), c5(g >> 3), c5(bl >> 3), al >= 128 ? 255 : 0);
        case LA8: return rgba_of(r, r, r, al);
        case LA4: return rgba_of(c4(r >> 4), c4(r >> 4), c4(r >> 4), c4(al >> 4));
        case L8: return rgba_of(r, r, r, r);
        case L4: return rgba_of(c4(r >> 4), c4(r >> 4), c4(r >> 4), c4(r >> 4));
        default: return c;
        }
    };
    static const u32 k_sizes[] = {1, 2, 3, 4, 5, 7, 8, 9, 12, 15, 16, 17, 20, 24, 31, 32, 33, 40, 50, 63, 64, 65,
                                  74, 96, 100, 127, 128, 129, 200, 255, 256, 300, 512};
    const u32 n_sizes = sizeof(k_sizes) / sizeof(k_sizes[0]);
    u32 read_n = 0, read_bad = 0, texels_bad = 0;
    std::vector<u32> lv_a, lv_b;
    std::vector<u8> lv;
    for (u32 fi = 0; fi < 11; fi++)
        for (u32 si = 0; si < n_sizes; si++)
            for (u32 pass = 0; pass < 3; pass++) {  // clamped, repeating, (POT grey ones) with smaller images
                const u32 f = fmts[fi], w = k_sizes[si];
                const u32 h = pass == 2 ? (fi & 1 ? w : std::max(64u, w / 2)) : k_sizes[(si * 7 + fi * 3 + pass * 11) % n_sizes];
                const u32 W = p2(w), H = p2(h), tlut_fmt = (si + pass) % 3, tlut_addr = (next() % 1500) << 9;
                const bool repeat = pass == 1, mips = pass == 2;
                if (mips && (f > 3 || W != w || H != h || w < 64 || h < 64))
                    continue;
                for (u32 i = 0; i < 0x10000; i++) tmem[(tlut_addr + i) & ((1u << 20) - 1)] = (u8)next();
                src.resize(gx_texture_bytes(f, w, h));
                for (u8& c : src) c = (u8)next();
                const Fmt pf = fmt_of(f, tlut_fmt);
                b.assign((size_t)W * H * texel_bits(pf) / 8, 0xCD), rgba.assign((size_t)w * h, 0);
                gx_decode_texture_tlut(src.data(), f, w, h, tlut_addr, tlut_fmt, rgba.data());
                // (as texture_for: the GX tiles straight in, a repeating texture of another size resampled)
                if (repeat && (W != w || H != h))
                    from_rgba(rgba.data(), w, h, b.data(), W, H, pf, true);
                else
                    from_gx(src.data(), f, w, h, tmem, GX_TMEM_MASK, tlut_addr, tlut_fmt, b.data(), W, H, pf);
                u32 wrong = 0, first_x = 0, first_y = 0, got0 = 0, want0 = 0;
                auto compare = [&](const u8* tex, u32 lw, u32 lh, u32 LW, u32 LH, const u32* img, bool resampled) {
                    for (u32 y = 0; y < LH; y++)
                        for (u32 x = 0; x < LW; x++) {
                            const u32 sx = resampled ? x * lw / LW : std::min(x, lw - 1);
                            const u32 sy = resampled ? y * lh / LH : std::min(y, lh - 1);
                            const u32 got = read_pica(tex, pf, LW, LH, x, y), want = kept_as(img[sy * lw + sx], pf);
                            if (got != want && !wrong++)
                                first_x = x, first_y = y, got0 = got, want0 = want;
                        }
                };
                compare(b.data(), w, h, W, H, rgba.data(), repeat && (W != w || H != h));
                if (mips) {  // (each level from the one before, as gx_pica.cpp's build_mips makes them)
                    lv_a = rgba;
                    for (u32 lw = w / 2, lh = h / 2; lw >= 8 && lh >= 8; lw /= 2, lh /= 2) {
                        lv_b.assign((size_t)lw * lh, 0);
                        mip_down(lv_a.data(), lw * 2, lh * 2, lv_b.data());
                        // the reference, a channel at a time, rounded: inside the 2x2 average, on the
                        // border the border's own texels (pairs down an edge column, across an edge
                        // row; the corners kept), so that a clamp past the edge gives the full-size
                        // image's edge at every size
                        const u32 pw = lw * 2;
                        auto at = [&](u32 x, u32 y) { return lv_a[(size_t)y * pw + x]; };
                        auto mean = [](std::initializer_list<u32> c) {
                            u32 v = 0;
                            for (int k = 0; k < 32; k += 8) {
                                u32 s = 0;
                                for (u32 p : c) s += p >> k & 255;
                                v |= ((s + (u32)c.size() / 2) / (u32)c.size()) << k;
                            }
                            return v;
                        };
                        for (u32 y = 0; y < lh; y++)
                            for (u32 x = 0; x < lw; x++) {
                                const bool ex = x == 0 || x == lw - 1, ey = y == 0 || y == lh - 1;
                                const u32 sx0 = x == 0 ? 0 : x == lw - 1 ? pw - 1 : 2 * x;
                                const u32 sy0 = y == 0 ? 0 : y == lh - 1 ? 2 * lh - 1 : 2 * y;
                                const u32 want = ex && ey ? at(sx0, sy0)
                                                 : ex     ? mean({at(sx0, 2 * y), at(sx0, 2 * y + 1)})
                                                 : ey     ? mean({at(2 * x, sy0), at(2 * x + 1, sy0)})
                                                          : mean({at(2 * x, 2 * y), at(2 * x + 1, 2 * y), at(2 * x, 2 * y + 1), at(2 * x + 1, 2 * y + 1)});
                                if (lv_b[(size_t)y * lw + x] != want && !wrong++)
                                    first_x = x, first_y = y, got0 = lv_b[(size_t)y * lw + x], want0 = want;
                            }
                        lv.assign((size_t)lw * lh * texel_bits(pf) / 8, 0xCD);
                        from_rgba(lv_b.data(), lw, lh, lv.data(), lw, lh, pf, false);
                        compare(lv.data(), lw, lh, lw, lh, lv_b.data(), false);
                        std::swap(lv_a, lv_b);
                    }
                }
                read_n++;
                texels_bad += wrong;
                if (wrong && read_bad++ < 8)
                    log("selftest tex: format %u %ux%u (PICA %u, %ux%u%s) read back wrong at %u texels, the first (%u, %u): "
                        "%08X, the GX texel %08X",
                        f, w, h, (u32)pf, W, H, repeat ? ", repeating" : mips ? ", smaller images" : "", wrong, first_x,
                        first_y, got0, want0);
            }
    log("selftest tex: %u textures read back as the PICA reads them, %u texels wrong: %s", read_n, texels_bad,
        read_bad ? "MISMATCH" : "ok");

    // ETC1: texture-like CMPR (endpoints a few steps apart, now and then flat or three-color)
    static const int MODT[8][2] = {{2, 8}, {5, 17}, {9, 29}, {13, 42}, {18, 60}, {24, 80}, {33, 106}, {47, 183}};
    double se = 0;
    long samples = 0;
    int flat_max = 0, alpha_bad = 0;
    for (int t = 0; t < 120; t++) {
        u32 w = 8 * (1 + next() % 24), h = 8 * (1 + next() % 16), W = p2(w), H = p2(h);
        const bool flat = t % 4 == 0;
        src.assign((size_t)(w / 8) * (h / 8) * 32, 0);
        for (size_t i = 0; i < src.size(); i += 8) {
            u32 c0 = next() & 0xFFFF;
            int r = (c0 >> 11) & 31, g = (c0 >> 5) & 63, bl = c0 & 31;
            u32 c1 = flat ? c0 : (u32)(std::min(31, r + (int)(next() % 4)) << 11 | std::min(63, g + (int)(next() % 8)) << 5 | std::min(31, bl + (int)(next() % 4)));
            if (c0 < c1) std::swap(c0, c1);
            if (t % 7 == 3 && (i / 8) % 3 == 0) std::swap(c0, c1);
            src[i] = (u8)(c0 >> 8), src[i + 1] = (u8)c0, src[i + 2] = (u8)(c1 >> 8), src[i + 3] = (u8)c1;
            for (int k = 4; k < 8; k++) src[i + k] = (u8)next();
        }
        const bool alpha = cmpr_has_alpha(src.data(), w, h);
        std::vector<u8> out(alpha ? (size_t)W * H : (size_t)W * H / 2);
        from_gx(src.data(), 14, w, h, nullptr, 0, 0, 0, out.data(), W, H, alpha ? ETC1A4 : ETC1);
        for (u32 y = 0; y < H; y++)
            for (u32 x = 0; x < W; x++) {
                // the source texel (edge texels repeated past the image), and the ETC1 one at its place
                u32 ix = std::min(x, w - 1), iy = std::min(y, h - 1);
                const u8* q = cmpr_sub_block(src.data(), w, ix, iy);
                u32 pal[4];
                cmpr_palette(q, pal);
                const u32 c = pal[cmpr_index(q, ix, iy)];
                const u32 mx = x, my = H - 1 - y;
                const u8* blk = out.data() + (((my / 8) * (W / 8) + mx / 8) * 4 + ((mx % 8) / 4) + 2 * ((my % 8) / 4)) * (alpha ? 16 : 8);
                const u32 fx = mx % 4, fy = my % 4, texel = 4 * fx + fy;
                if (alpha) {
                    u64 av;
                    memcpy(&av, blk, 8), blk += 8;
                    alpha_bad += (int)((av >> (4 * texel)) & 15) * 17 != (int)(c >> 24 >> 4) * 17;
                }
                u64 v;
                memcpy(&v, blk, 8);
                u32 xx = fx, yy = fy;
                if ((v >> 32) & 1) std::swap(xx, yy);
                const bool diff = (v >> 33) & 1;
                int tbl = xx < 2 ? (int)((v >> 37) & 7) : (int)((v >> 34) & 7);
                int m = MODT[tbl][(v >> texel) & 1];
                if ((v >> (16 + texel)) & 1) m = -m;
                for (int k = 0; k < 3; k++) {
                    const int at = 56 - 8 * k;
                    int q8;
                    if (diff) {
                        int base = (int)((v >> (at + 3)) & 31), d = (int)((v >> at) & 7);
                        if (d >= 4) d -= 8;
                        int q5 = xx >= 2 ? base + d : base;
                        q8 = q5 << 3 | q5 >> 2;
                    } else {
                        q8 = (xx < 2 ? (int)((v >> (at + 4)) & 15) : (int)((v >> at) & 15)) * 17;
                    }
                    const int e = std::min(255, std::max(0, q8 + m)) - (int)((c >> (8 * k)) & 255);
                    if (flat) flat_max = std::max(flat_max, std::abs(e));
                    else se += e * e, samples++;
                }
            }
    }
    const double psnr = 10 * std::log10(255.0 * 255.0 * samples / se);
    const bool etc_ok = flat_max <= 3 && psnr >= 35.0 && !alpha_bad;
    log("selftest tex: CMPR as ETC1: flat blocks within %d, texture-like %.1f dB, %d alpha differences: %s", flat_max, psnr,
        alpha_bad, etc_ok ? "ok" : "WORSE THAN EXPECTED");
}

static void selftest_crc() {
    axref::Rng rnd{1616};
    const u32 base = 0x90200000;
    auto reference = [](u32 p, u32 n) {
        u32 c = 0;
        for (u32 i = 0; i < n; i++) {
            u32 b = load8(p + i);
            c = (c >> 4) ^ load16(GA___OSCRC16Table + ((c ^ b) & 15) * 2);
            c = (c >> 4) ^ load16(GA___OSCRC16Table + ((c ^ (b >> 4)) & 15) * 2);
        }
        return c & 0xFFFF;
    };
    int bad = 0, cases = 0;
    std::vector<u8> d;
    for (int t = 0; t < 400 && bad < 5; t++, cases++) {
        u32 n = t == 0 ? 0x77FF8 : t % 50 == 0 ? rnd() % 0x100000 : t % 5 == 0 ? rnd() % 8 : rnd() % 20000;
        u32 at = base + rnd() % 64;
        d.resize(n);
        for (u8& b : d)
            b = (u8)rnd();
        write_g(at, d.data(), n);
        u32 want = reference(at, n), got = g::OSCalcCRC16(at, n);
        if (want != got) {
            bad++;
            log("selftest crc: %u bytes at %08X: %04X, the loop %04X", n, at, got, want);
        }
    }
    log("selftest crc: %d blocks, %s", cases, bad ? "MISMATCH" : "ok");
}

void selftest_g3d();  // selftest_g3d.cpp
void selftest_g3drender();  // selftest_g3drender.cpp (the software backend; a package's views if there is one)
void selftest_walk();   // selftest_walk.cpp (needs the island's collision files: not in "all")
void selftest_vehicles();  // selftest_vehicles.cpp (needs the staged island package: not in "all")
void selftest_water_fx();  // selftest_water_fx.cpp (the water effects; the staged island's if there is one)
void selftest_water();     // selftest_water.cpp (the island's water entries and exits; needs its collision files: not in "all")
void selftest_input();  // selftest_walk.cpp
void selftest_kpad();   // selftest_walk.cpp

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
        selftest_ax(), selftest_axframe();
    else if (name == "cx")
        selftest_cx();
    else if (name == "fill")
        selftest_fill();
    else if (name == "crc")
        selftest_crc();
    else if (name == "stack")
        selftest_stack();
    else if (name == "efb")
        selftest_efb();
    else if (name == "tex")
        selftest_tex();
    else if (name == "g3d")
        selftest_g3d();
    else if (name == "g3drender")
        selftest_g3drender();
    else if (name == "walk")
        selftest_walk();
    else if (name == "vehicles")
        selftest_vehicles();
    else if (name == "waterfx")
        selftest_water_fx();
    else if (name == "water")
        selftest_water();
    else if (name == "input")
        selftest_input(), selftest_kpad();
    else if (name == "all") {
        selftest_input();  // (first: NOMA_INPUT is read once, at the first retrace, which a later test can reach)
        selftest_kpad();
        selftest_rfl();
        selftest_time();
        selftest_int64();
        selftest_aes();
        selftest_printf();
        selftest_math();
        selftest_strtol();
        selftest_mtx();
        selftest_ax();
        selftest_axframe();
        selftest_cx();
        selftest_fill();
        selftest_crc();
        selftest_stack();
        selftest_efb();
        selftest_tex();
        selftest_g3d();
        selftest_g3drender();
        selftest_water_fx();
    }
    else
        return false;
    return true;
}

}  // namespace guest
