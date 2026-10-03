// The game's debug output (OSReport, printf, OSPanic), written to our log. On a retail Wii
// OSReport prints nowhere; here it is the best view into what the program is doing.
//
// guest_format() is printf for guest arguments: varargs calls pass integers in r3-r10 and
// floating-point values in f1-f8 (more go on the caller's stack, 8 bytes from its r1), and a
// CodeWarrior va_list is {u8 gpr, u8 fpr, u16, u32 overflow_area, u32 reg_save_area} with
// r3-r10 then f1-f8 (as doubles) in the save area.
#include <cstdio>
#include <cstring>
#include <string>

#include "noma_decomp.h"
#include "runtime.h"

using namespace guest;

namespace {

struct GuestArgs {
    // register file of the call (or of the va_list's save area)
    u32 gpr[8];
    f64 fpr[8];
    int gi, fi;
    u32 overflow;  // next stack argument

    u32 next_u32() {
        if (gi < 8)
            return gpr[gi++];
        u32 v = load32(overflow);
        overflow += 4;
        return v;
    }
    u64 next_u64() {
        gi += gi & 1;  // 64-bit values take an aligned register pair (r3:r4, r5:r6, ...)
        if (gi < 7) {
            u64 v = ((u64)gpr[gi] << 32) | gpr[gi + 1];
            gi += 2;
            return v;
        }
        gi = 8;
        overflow = (overflow + 7) & ~7u;
        u64 v = ((u64)load32(overflow) << 32) | load32(overflow + 4);
        overflow += 8;
        return v;
    }
    f64 next_f64() {
        if (fi < 8)
            return fpr[fi++];
        overflow = (overflow + 7) & ~7u;
        u64 bits = ((u64)load32(overflow) << 32) | load32(overflow + 4);
        overflow += 8;
        f64 d;
        memcpy(&d, &bits, 8);
        return d;
    }
};

std::string guest_format(u32 fmt, GuestArgs& a) {
    std::string out;
    std::string f = read_string(fmt, 1024);
    for (size_t i = 0; i < f.size(); i++) {
        char c = f[i];
        if (c != '%') {
            out += c;
            continue;
        }
        size_t start = i++;
        std::string spec = "%";
        while (i < f.size() && strchr("-+ #0", f[i]))
            spec += f[i++];
        if (i < f.size() && f[i] == '*') {
            spec += std::to_string((s32)a.next_u32());
            i++;
        } else {
            while (i < f.size() && isdigit((unsigned char)f[i]))
                spec += f[i++];
        }
        if (i < f.size() && f[i] == '.') {
            spec += f[i++];
            if (i < f.size() && f[i] == '*') {
                spec += std::to_string((s32)a.next_u32());
                i++;
            } else {
                while (i < f.size() && isdigit((unsigned char)f[i]))
                    spec += f[i++];
            }
        }
        int longs = 0;
        bool shorts = false;
        while (i < f.size() && strchr("hlLqjzt", f[i])) {
            if (f[i] == 'l' || f[i] == 'q' || f[i] == 'L' || f[i] == 'j')
                longs++;
            if (f[i] == 'h')
                shorts = true;
            i++;
        }
        if (i >= f.size())
            break;
        char conv = f[i];
        char buf[512];
        switch (conv) {
        case 'd':
        case 'i':
            if (longs >= 2)
                snprintf(buf, sizeof(buf), (spec + "lld").c_str(), (long long)(s64)a.next_u64());
            else
                snprintf(buf, sizeof(buf), (spec + "d").c_str(),
                         shorts ? (int)(s16)a.next_u32() : (int)(s32)a.next_u32());
            out += buf;
            break;
        case 'u':
        case 'x':
        case 'X':
        case 'o':
            if (longs >= 2)
                snprintf(buf, sizeof(buf), (spec + "ll" + conv).c_str(), (unsigned long long)a.next_u64());
            else
                snprintf(buf, sizeof(buf), (spec + conv).c_str(),
                         shorts ? (unsigned)(u16)a.next_u32() : (unsigned)a.next_u32());
            out += buf;
            break;
        case 'c':
            snprintf(buf, sizeof(buf), (spec + "c").c_str(), (int)(u8)a.next_u32());
            out += buf;
            break;
        case 'p':
            snprintf(buf, sizeof(buf), "%08X", a.next_u32());
            out += buf;
            break;
        case 's': {
            u32 p = a.next_u32();
            std::string s = p ? read_string(p, 512) : "(null)";
            snprintf(buf, sizeof(buf), (spec + "s").c_str(), s.c_str());
            out += buf;
            break;
        }
        case 'f':
        case 'F':
        case 'e':
        case 'E':
        case 'g':
        case 'G':
        case 'a':
        case 'A':
            snprintf(buf, sizeof(buf), (spec + conv).c_str(), a.next_f64());
            out += buf;
            break;
        case 'n':
            a.next_u32();
            break;
        case '%':
            out += '%';
            break;
        default:
            out += f.substr(start, i - start + 1);
            break;
        }
    }
    return out;
}

GuestArgs call_args(u32 r3, u32 r4, u32 r5, u32 r6, u32 r7, u32 r8, u32 r9, u32 r10, f64 f1, f64 f2,
                    f64 f3, f64 f4, f64 f5, f64 f6, f64 f7, f64 f8, int named) {
    GuestArgs a{{r3, r4, r5, r6, r7, r8, r9, r10}, {f1, f2, f3, f4, f5, f6, f7, f8}, named, 0,
                g_sp + 8};
    return a;
}

std::string s_line;

void emit(const std::string& text, const char* who) {
    s_line += text;
    size_t p;
    while ((p = s_line.find('\n')) != std::string::npos) {
        log("%s: %s", who, s_line.substr(0, p).c_str());
        s_line.erase(0, p + 1);
    }
    if (s_line.size() > 400) {
        log("%s: %s", who, s_line.c_str());
        s_line.clear();
    }
}

}  // namespace

// A CodeWarrior va_list in guest memory, for vprintf-style natives.
std::string guest_vformat(u32 fmt, u32 va) {
    GuestArgs a{};
    u8 gi = load8(va), fi = load8(va + 1);
    a.overflow = load32(va + 4);
    u32 save = load32(va + 8);
    for (int i = 0; i < 8; i++) {
        a.gpr[i] = load32(save + 4 * i);
        u64 bits = ((u64)load32(save + 32 + 8 * i) << 32) | load32(save + 36 + 8 * i);
        memcpy(&a.fpr[i], &bits, 8);
    }
    a.gi = gi;
    a.fi = fi;
    return guest_format(fmt, a);
}

namespace g {

void OSReport(u32 r3, u32 r4, u32 r5, u32 r6, u32 r7, u32 r8, u32 r9, u32 r10, f64 f1, f64 f2, f64 f3,
              f64 f4, f64 f5, f64 f6, f64 f7, f64 f8) {
    GuestArgs a = call_args(r3, r4, r5, r6, r7, r8, r9, r10, f1, f2, f3, f4, f5, f6, f7, f8, 1);
    emit(guest_format(r3, a), "OSReport");
}

u32 printf(u32 r3, u32 r4, u32 r5, u32 r6, u32 r7, u32 r8, u32 r9, u32 r10, f64 f1, f64 f2, f64 f3,
           f64 f4, f64 f5, f64 f6, f64 f7, f64 f8) {
    GuestArgs a = call_args(r3, r4, r5, r6, r7, r8, r9, r10, f1, f2, f3, f4, f5, f6, f7, f8, 1);
    std::string s = guest_format(r3, a);
    emit(s, "printf");
    return (u32)s.size();
}

// OSPanic(file, line, fmt, ...): the SDK's assertion failure
void OSPanic(u32 r3, u32 r4, u32 r5, u32 r6, u32 r7, u32 r8, u32 r9, u32 r10, f64 f1, f64 f2, f64 f3,
             f64 f4, f64 f5, f64 f6, f64 f7, f64 f8) {
    GuestArgs a = call_args(r3, r4, r5, r6, r7, r8, r9, r10, f1, f2, f3, f4, f5, f6, f7, f8, 3);
    std::string msg = guest_format(r5, a);
    fatal("OSPanic in %s:%u: %s", read_string(r3, 256).c_str(), r4, msg.c_str());
}

}  // namespace g
