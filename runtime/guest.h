// Guest memory model for the decompiled Wii no Ma code.
//
// The decompiled code keeps the Wii's view of memory: pointers are 32-bit Wii addresses
// (gp<T>), memory is big-endian, and the hardware registers at 0xCC000000/0xCD000000 are
// reached through the same accessors (they land in the device emulation, see hw/).
// Every 1 MB page of the 4 GB address space maps to host memory or to MMIO (0). MEM1 and MEM2
// are each one block of host memory, so a run of bytes is contiguous up to the end of its bank.
//
//   gp<T>       pointer to a T in guest memory         (*p, p[i], p + n, p - q, casts)
//   gref<T>     a T stored big-endian at an address    (reads, writes, compound ops, &x)
//   GV(T, a)    a global variable of type T at a       (gref<T>)
//   GS(T, off)  a variable in this function's guest stack frame
//   gcall<R>()  call a function through its guest code address (vtables, callbacks)
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <utility>

typedef uint8_t u8;
typedef int8_t s8;
typedef uint16_t u16;
typedef int16_t s16;
typedef uint32_t u32;
typedef int32_t s32;
typedef uint64_t u64;
typedef int64_t s64;
typedef float f32;
typedef double f64;

#undef NULL
#define NULL 0

namespace guest {

// Each 1 MB page of the guest address space: the host address of its first byte minus its guest
// address, so that a guest address a is at g_page[a >> PAGE_BITS] + a (no mask); 0 = MMIO or
// unmapped. MEM1's cached view (0x80000000, all the program's globals) is always mapped.
static const u32 PAGE_BITS = 20;
static const u32 PAGE_SIZE = 1u << PAGE_BITS;
static const u32 MEM1_SIZE = 24u << 20;
extern uintptr_t g_page[1u << (32 - PAGE_BITS)];
// current guest stack pointer (r1) of the running guest thread
extern u32 g_sp;

u32 mmio_read(u32 addr, int size);
void mmio_write(u32 addr, u32 value, int size);
// the host function for a guest code address (aborts if unknown): every call through a pointer
// (vtables, callbacks) asks, so the answers stay in a direct-mapped cache in front of the search
struct FnSlot {
    u32 tag;  // the address | 1 (an empty slot's 0 never matches, not even address 0)
    void* fn;
};
extern FnSlot g_fn_cache[4096];
void* lookup_fn_slow(u32 guest_addr);
static inline void* lookup_fn(u32 a) {
    const FnSlot& s = g_fn_cache[(a >> 2) & 4095];
    if (__builtin_expect(s.tag == (a | 1), 1))
        return s.fn;
    return lookup_fn_slow(a);
}
[[noreturn]] void fatal(const char* fmt, ...);

// A constant address (a global, a hardware register) is resolved at compile time as far as it
// can be: in MEM1 no check for MMIO, at 0xCC/0xCD straight to the device emulation.
#define NOMA_CONST_MEM1(a) (__builtin_constant_p(a) && (u32)(a) - 0x80000000u < MEM1_SIZE)
#define NOMA_CONST_MMIO(a) (__builtin_constant_p(a) && ((u32)(a) >> 25) == (0xCC000000u >> 25))

__attribute__((always_inline)) static inline u8* host_ptr(u32 a) {
    u8* b = (u8*)g_page[a >> PAGE_BITS];
    return b ? b + a : nullptr;
}

// whether an access at a, whose page entry is b, goes to RAM (else to the device emulation)
#define NOMA_IS_RAM(a, b) (NOMA_CONST_MEM1(a) || (!NOMA_CONST_MMIO(a) && __builtin_expect((b) != 0, 1)))

__attribute__((always_inline)) static inline u8 load8(u32 a) {
    uintptr_t b = g_page[a >> PAGE_BITS];
    if (NOMA_IS_RAM(a, b))
        return *(u8*)(b + a);
    return (u8)mmio_read(a, 1);
}
__attribute__((always_inline)) static inline u16 load16(u32 a) {
    uintptr_t b = g_page[a >> PAGE_BITS];
    if (NOMA_IS_RAM(a, b)) {
        u16 v;
        ::memcpy(&v, (u8*)(b + a), 2);
        return __builtin_bswap16(v);
    }
    return (u16)mmio_read(a, 2);
}
__attribute__((always_inline)) static inline u32 load32(u32 a) {
    uintptr_t b = g_page[a >> PAGE_BITS];
    if (NOMA_IS_RAM(a, b)) {
        u32 v;
        ::memcpy(&v, (u8*)(b + a), 4);
        return __builtin_bswap32(v);
    }
    return mmio_read(a, 4);
}
__attribute__((always_inline)) static inline u64 load64(u32 a) {
    return ((u64)load32(a) << 32) | load32(a + 4);
}
#ifdef NOMA_TRACE_CALLS
// NOMA_WATCH=addr: report every store to that word (trace builds)
extern u32 g_watch_addr;
void noma_watch_hit(u32 a, u32 v, int size);
#define NOMA_WATCH(a, v, size)                                            \
    do {                                                                  \
        if (__builtin_expect(((a) & ~3u) == guest::g_watch_addr, 0))      \
            guest::noma_watch_hit((a), (u32)(v), (size));                 \
    } while (0)
#else
#define NOMA_WATCH(a, v, size) ((void)0)
#endif

// The write-gather pipe (0xCC008000), where every GX command and vertex goes: a store to it at a
// constant address appends to its 32-byte buffer inline when the value fits before the buffer's
// end, where the burst goes to the FIFO (hw/gx/fifo.cpp); a value across the end takes mmio_write.
extern u8 g_gather[32];
extern u32 g_gather_n;
void gx_gather_burst();
#define NOMA_CONST_WGPIPE(a) (NOMA_CONST_MMIO(a) && ((u32)(a) & 0x0FFFF000u) == 0x0C008000u)
template <u32 N>
__attribute__((always_inline)) static inline void gather_put(u32 v) {
    u32 n = g_gather_n;
    if (__builtin_expect(n <= 32 - N, 1)) {
        if (N == 4) {
            u32 w = __builtin_bswap32(v);
            ::memcpy(g_gather + n, &w, 4);
        } else if (N == 2) {
            u16 h = __builtin_bswap16((u16)v);
            ::memcpy(g_gather + n, &h, 2);
        } else {
            g_gather[n] = (u8)v;
        }
        g_gather_n = n += N;
        if (n == 32)
            gx_gather_burst();
    } else {
        mmio_write(0xCC008000u, v, (int)N);
    }
}

__attribute__((always_inline)) static inline void store8(u32 a, u8 v) {
    NOMA_WATCH(a, v, 1);
    if (NOMA_CONST_WGPIPE(a)) {
        gather_put<1>(v);
        return;
    }
    uintptr_t b = g_page[a >> PAGE_BITS];
    if (NOMA_IS_RAM(a, b))
        *(u8*)(b + a) = v;
    else
        mmio_write(a, v, 1);
}
__attribute__((always_inline)) static inline void store16(u32 a, u16 v) {
    NOMA_WATCH(a, v, 2);
    if (NOMA_CONST_WGPIPE(a)) {
        gather_put<2>(v);
        return;
    }
    uintptr_t b = g_page[a >> PAGE_BITS];
    if (NOMA_IS_RAM(a, b)) {
        v = __builtin_bswap16(v);
        ::memcpy((u8*)(b + a), &v, 2);
    } else {
        mmio_write(a, v, 2);
    }
}
__attribute__((always_inline)) static inline void store32(u32 a, u32 v) {
    NOMA_WATCH(a, v, 4);
    if (NOMA_CONST_WGPIPE(a)) {
        gather_put<4>(v);
        return;
    }
    uintptr_t b = g_page[a >> PAGE_BITS];
    if (NOMA_IS_RAM(a, b)) {
        v = __builtin_bswap32(v);
        ::memcpy((u8*)(b + a), &v, 4);
    } else {
        mmio_write(a, v, 4);
    }
}
__attribute__((always_inline)) static inline void store64(u32 a, u64 v) {
    store32(a, (u32)(v >> 32));
    store32(a + 4, (u32)v);
}

void memcpy_g(u32 dst, u32 src, u32 n);  // guest-to-guest, overlap-safe
void memset_g(u32 dst, u8 v, u32 n);

}  // namespace guest

namespace guest {
template <class T>
struct gp;
template <class T>
struct gref;

// At -O1 GCC calls the accessors below out of line (about 130,000 call sites in the decompiled
// code), and each call makes the caller keep its live values out of r0-r3/s0-s15.
// NOMA_ACCESSORS_INLINE=1 forces the float ones inline, =2 all of them. Measured on the 3DS
// build: all of them inline is +45% code (12.75 -> 18.55 MB), too much for the Old 3DS; see
// docs/optimisations.md for the floats-only measurement.
#ifndef NOMA_ACCESSORS_INLINE
#define NOMA_ACCESSORS_INLINE 0
#endif
#if NOMA_ACCESSORS_INLINE >= 2
#define NOMA_ACC __attribute__((always_inline))
#else
#define NOMA_ACC
#endif
#if NOMA_ACCESSORS_INLINE >= 1
#define NOMA_ACC_F __attribute__((always_inline))
#else
#define NOMA_ACC_F
#endif

// --------------------------------------------------------------------------------------
// storage of each type in guest memory
template <class T, class Enable = void>
struct gstore_traits;

template <class T>
struct gstore_traits<T, typename std::enable_if<std::is_integral<T>::value>::type> {
    static constexpr u32 size = sizeof(T);
    NOMA_ACC static T load(u32 a) {
        if (sizeof(T) == 1)
            return (T)guest::load8(a);
        if (sizeof(T) == 2)
            return (T)guest::load16(a);
        if (sizeof(T) == 4)
            return (T)guest::load32(a);
        return (T)guest::load64(a);
    }
    NOMA_ACC static void store(u32 a, T v) {
        if (sizeof(T) == 1)
            guest::store8(a, (u8)v);
        else if (sizeof(T) == 2)
            guest::store16(a, (u16)v);
        else if (sizeof(T) == 4)
            guest::store32(a, (u32)v);
        else
            guest::store64(a, (u64)v);
    }
};

template <>
struct gstore_traits<f32> {
    static constexpr u32 size = 4;
    NOMA_ACC_F static f32 load(u32 a) {
        u32 b = guest::load32(a);
        f32 f;
        ::memcpy(&f, &b, 4);
        return f;
    }
    NOMA_ACC_F static void store(u32 a, f32 v) {
        u32 b;
        ::memcpy(&b, &v, 4);
        guest::store32(a, b);
    }
};

template <>
struct gstore_traits<f64> {
    static constexpr u32 size = 8;
    NOMA_ACC_F static f64 load(u32 a) {
        u64 b = guest::load64(a);
        f64 f;
        ::memcpy(&f, &b, 8);
        return f;
    }
    NOMA_ACC_F static void store(u32 a, f64 v) {
        u64 b;
        ::memcpy(&b, &v, 8);
        guest::store64(a, b);
    }
};

template <class U>
struct gstore_traits<gp<U>> {
    static constexpr u32 size = 4;
    NOMA_ACC static gp<U> load(u32 a) { return gp<U>(guest::load32(a)); }
    NOMA_ACC static void store(u32 a, gp<U> v) { guest::store32(a, v.a); }
};

// element size for pointer arithmetic (void* arithmetic is byte-wise, as in GNU C)
template <class T>
struct gsize {
    static constexpr u32 value = gstore_traits<T>::size;
};
template <>
struct gsize<void> {
    static constexpr u32 value = 1;
};

// --------------------------------------------------------------------------------------
template <class T>
struct gref {
    u32 a;

    NOMA_ACC operator T() const { return gstore_traits<T>::load(a); }
    NOMA_ACC T get() const { return gstore_traits<T>::load(a); }
    NOMA_ACC const gref& operator=(T v) const {
        gstore_traits<T>::store(a, v);
        return *this;
    }
    const gref& operator=(const gref& o) const { return *this = o.get(); }
    gp<T> operator&() const { return gp<T>(a); }

#define GREF_COMPOUND(op)                                  \
    template <class V>                                     \
    NOMA_ACC const gref& operator op##=(V v) const {       \
        return *this = (T)(get() op v);                    \
    }
    GREF_COMPOUND(+)
    GREF_COMPOUND(-)
    GREF_COMPOUND(*)
    GREF_COMPOUND(/)
    GREF_COMPOUND(%)
    GREF_COMPOUND(&)
    GREF_COMPOUND(|)
    GREF_COMPOUND(^)
    GREF_COMPOUND(<<)
    GREF_COMPOUND(>>)
#undef GREF_COMPOUND
    const gref& operator++() const { return *this = (T)(get() + 1); }
    const gref& operator--() const { return *this = (T)(get() - 1); }
    T operator++(int) const {
        T old = get();
        *this = (T)(old + 1);
        return old;
    }
    T operator--(int) const {
        T old = get();
        *this = (T)(old - 1);
        return old;
    }
};

// pointers stored in memory: indexing and dereferencing go through the loaded pointer
template <class U>
struct gref<gp<U>> {
    u32 a;
    NOMA_ACC gp<U> get() const { return gp<U>(guest::load32(a)); }
    template <class V>
    NOMA_ACC operator gp<V>() const { return gp<V>(guest::load32(a)); }
    NOMA_ACC operator u32() const { return guest::load32(a); }
    NOMA_ACC const gref& operator=(gp<U> v) const {
        guest::store32(a, v.a);
        return *this;
    }
    const gref& operator=(const gref& o) const { return *this = o.get(); }
    template <class V>
    const gref& operator=(gp<V> v) const {
        guest::store32(a, v.a);
        return *this;
    }
    template <class I, class = typename std::enable_if<std::is_integral<I>::value>::type>
    const gref& operator=(I v) const {
        guest::store32(a, (u32)v);
        return *this;
    }
    const gref& operator=(std::nullptr_t) const {
        guest::store32(a, 0);
        return *this;
    }
    gp<gp<U>> operator&() const;
    gref<U> operator*() const { return *get(); }
    template <class I>
    gref<U> operator[](I i) const { return get()[i]; }
    explicit operator bool() const { return guest::load32(a) != 0; }
    template <class V>
    bool operator==(gp<V> y) const { return get().a == y.a; }
    template <class V>
    bool operator!=(gp<V> y) const { return get().a != y.a; }
    template <class I, class = typename std::enable_if<std::is_integral<I>::value>::type>
    bool operator==(I y) const { return get().a == (u32)y; }
    template <class I, class = typename std::enable_if<std::is_integral<I>::value>::type>
    bool operator!=(I y) const { return get().a != (u32)y; }
    template <class I, class = typename std::enable_if<std::is_integral<I>::value>::type>
    gp<U> operator+(I n) const { return get() + n; }
    template <class I, class = typename std::enable_if<std::is_integral<I>::value>::type>
    gp<U> operator-(I n) const { return get() - n; }
    template <class V>
    s32 operator-(gp<V> o) const { return get() - o; }
    template <class I>
    const gref& operator+=(I n) const { return *this = get() + n; }
    template <class I>
    const gref& operator-=(I n) const { return *this = get() - n; }
    const gref& operator++() const { return *this = get() + 1; }
    const gref& operator--() const { return *this = get() - 1; }
    gp<U> operator++(int) const {
        gp<U> o = get();
        *this = o + 1;
        return o;
    }
    gp<U> operator--(int) const {
        gp<U> o = get();
        *this = o - 1;
        return o;
    }
};

// --------------------------------------------------------------------------------------
template <class T>
struct gp {
    u32 a;

    constexpr gp() : a(0) {}
    constexpr gp(std::nullptr_t) : a(0) {}
    template <class I, class = typename std::enable_if<std::is_integral<I>::value>::type>
    constexpr gp(I x) : a((u32)x) {}
    template <class U>
    constexpr gp(gp<U> o) : a(o.a) {}
    template <class I, class = typename std::enable_if<std::is_integral<I>::value>::type>
    gp(const gref<I>& r) : a((u32)r.get()) {}

    operator u32() const { return a; }
    explicit operator bool() const { return a != 0; }

    // Arithmetic is in bytes, whatever T is: m2c writes `p + 8` for an addi on a pointer register
    // (it casts both sides to intptr silently). Only indexing (GIDX, from an array access with a
    // known element size) scales by the element size.
    gref<T> operator*() const { return gref<T>{a}; }
    template <class I>
    gref<T> operator[](I i) const { return gref<T>{a + (u32)(s32)i * gsize<T>::value}; }
    template <class I, class = typename std::enable_if<std::is_integral<I>::value>::type>
    gp operator+(I n) const { return gp(a + (u32)n); }
    template <class I, class = typename std::enable_if<std::is_integral<I>::value>::type>
    gp operator-(I n) const { return gp(a - (u32)n); }
    template <class U>
    s32 operator-(gp<U> o) const { return (s32)(a - o.a); }
    template <class I>
    gp& operator+=(I n) {
        a += (u32)n;
        return *this;
    }
    template <class I>
    gp& operator-=(I n) {
        a -= (u32)n;
        return *this;
    }
    gp& operator++() { return *this += 1; }
    gp& operator--() { return *this -= 1; }
    gp operator++(int) {
        gp o = *this;
        *this += 1;
        return o;
    }
    gp operator--(int) {
        gp o = *this;
        *this -= 1;
        return o;
    }
    template <class U>
    bool operator==(gp<U> y) const { return a == y.a; }
    template <class U>
    bool operator!=(gp<U> y) const { return a != y.a; }
    template <class U>
    bool operator<(gp<U> y) const { return a < y.a; }
    template <class U>
    bool operator>(gp<U> y) const { return a > y.a; }
    template <class U>
    bool operator<=(gp<U> y) const { return a <= y.a; }
    template <class U>
    bool operator>=(gp<U> y) const { return a >= y.a; }
    bool operator==(std::nullptr_t) const { return a == 0; }
    bool operator!=(std::nullptr_t) const { return a != 0; }
#define GP_CMP(op)                                                                    \
    template <class I, class = typename std::enable_if<std::is_integral<I>::value>::type> \
    bool operator op(I y) const {                                                     \
        return a op (u32)y;                                                           \
    }
    GP_CMP(==)
    GP_CMP(!=)
    GP_CMP(<)
    GP_CMP(>)
    GP_CMP(<=)
    GP_CMP(>=)
#undef GP_CMP
};

template <class U>
gp<gp<U>> gref<gp<U>>::operator&() const {
    return gp<gp<U>>(a);
}

// address of an expression that may be an integer, a pointer or a stored pointer
template <class I, class = typename std::enable_if<std::is_integral<I>::value>::type>
static inline u32 gaddr(I x) {
    return (u32)x;
}
template <class T>
static inline u32 gaddr(gp<T> p) {
    return p.a;
}
template <class U>
static inline u32 gaddr(const gref<gp<U>>& r) {
    return r.get().a;
}
template <class T, class = typename std::enable_if<std::is_integral<T>::value>::type>
static inline u32 gaddr(const gref<T>& r) {
    return (u32)r.get();
}
static inline u32 gaddr(std::nullptr_t) { return 0; }

}  // namespace guest
using guest::gp;
using guest::gref;
using guest::gaddr;
using guest::gstore_traits;
using guest::gsize;

// m2c's "unknown type": a 4-byte value
struct M2C_UNK {
    s32 v;
    M2C_UNK() = default;
    constexpr M2C_UNK(s32 x) : v(x) {}
    template <class T, class = typename std::enable_if<std::is_arithmetic<T>::value>::type>
    constexpr M2C_UNK(T x) : v((s32)x) {}
    template <class U>
    M2C_UNK(gp<U> p) : v((s32)p.a) {}
    operator s32() const { return v; }
};
namespace guest {
template <>
struct gstore_traits<M2C_UNK> {
    static constexpr u32 size = 4;
    NOMA_ACC static M2C_UNK load(u32 a) { return M2C_UNK((s32)load32(a)); }
    NOMA_ACC static void store(u32 a, M2C_UNK v) { store32(a, (u32)v.v); }
};
template <>
struct gref<M2C_UNK> {
    u32 a;
    NOMA_ACC s32 get() const { return (s32)load32(a); }
    NOMA_ACC operator s32() const { return get(); }
    operator M2C_UNK() const { return M2C_UNK(get()); }
    template <class U>
    operator gp<U>() const { return gp<U>((u32)get()); }
    template <class V>
    const gref& operator=(V v) const {
        store32(a, (u32)(s32)M2C_UNK(v));
        return *this;
    }
    const gref& operator=(const gref& o) const { return *this = o.get(); }
    gp<M2C_UNK> operator&() const;
    template <class V>
    const gref& operator+=(V v) const { return *this = get() + (s32)v; }
    template <class V>
    const gref& operator-=(V v) const { return *this = get() - (s32)v; }
    template <class V>
    const gref& operator|=(V v) const { return *this = get() | (s32)v; }
    template <class V>
    const gref& operator&=(V v) const { return *this = get() & (s32)v; }
};
}  // namespace guest

namespace guest {
inline gp<M2C_UNK> gref<M2C_UNK>::operator&() const { return gp<M2C_UNK>(a); }
}

// --------------------------------------------------------------------------------------
// guest stack frames: variables whose address the code takes live in guest memory
struct GuestFrame {
    u32 sp, csp;
    explicit GuestFrame(u32 n) : sp(0), csp(guest::g_sp) {
        sp = guest::g_sp = csp - ((n + 7) & ~7u);
    }
    // CodeWarrior's aligned frames (clrlwi/subfic/stwux): n is the frame plus `align` bytes of
    // slack; the locals' base is aligned, the caller's frame stays reachable through csp
    GuestFrame(u32 n, u32 align) : sp(0), csp(guest::g_sp) {
        sp = guest::g_sp = (csp - (n - align)) & ~(align - 1);
    }
    ~GuestFrame() { guest::g_sp = csp; }
};
#define GFRAME(n) GuestFrame _gframe(n)
#define GFRAME_ALIGN(n, a) GuestFrame _gframe(n, a)
#define GS(T, off) (*gp<T>(_gframe.sp + (u32)(off)))
#define GSARG(T, off) (*gp<T>(_gframe.csp + (u32)(off)))
// the incoming stack pointer (r1 on entry), as m2c names it
#define GSP (gp<void>(_gframe.csp))
// CodeWarrior's register save/restore helpers when m2c didn't fold them into the prologue
#define NOMA_REGHELPER(n)                          \
    template <class... A>                          \
    static inline u32 _savegpr_##n(A...) { return 0; } \
    template <class... A>                          \
    static inline u32 _restgpr_##n(A...) { return 0; } \
    template <class... A>                          \
    static inline u32 _savefpr_##n(A...) { return 0; } \
    template <class... A>                          \
    static inline u32 _restfpr_##n(A...) { return 0; }
NOMA_REGHELPER(14) NOMA_REGHELPER(15) NOMA_REGHELPER(16) NOMA_REGHELPER(17) NOMA_REGHELPER(18)
NOMA_REGHELPER(19) NOMA_REGHELPER(20) NOMA_REGHELPER(21) NOMA_REGHELPER(22) NOMA_REGHELPER(23)
NOMA_REGHELPER(24) NOMA_REGHELPER(25) NOMA_REGHELPER(26) NOMA_REGHELPER(27) NOMA_REGHELPER(28)
NOMA_REGHELPER(29) NOMA_REGHELPER(30) NOMA_REGHELPER(31)
#undef NOMA_REGHELPER

// the entry value of a callee-saved register (r31, lr...): has no meaning in C; such reads are
// audited in docs/decomp_notes.md
#define GSAVEDREG(r) (0u)

// globals and functions by guest address (GA_<name> constants come from gsyms.h)
#define GV(T, addr) (*gp<T>((u32)(addr)))
// a constant of the DOL's read-only .sdata2, by its bits (tools/decomp/decompile.py fold_ro_reads)
static constexpr inline f32 noma_kf32(u32 bits) { return __builtin_bit_cast(f32, bits); }
static constexpr inline f64 noma_kf64(u64 bits) { return __builtin_bit_cast(f64, bits); }
#define GARR(T, addr) (gp<T>((u32)(addr)))
#define GFN(f) (gp<void>(GA_##f))

// --------------------------------------------------------------------------------------
// m2c "valid syntax" macros
typedef s8 M2C_UNK8;
typedef s16 M2C_UNK16;
typedef s32 M2C_UNK32;
typedef s64 M2C_UNK64;
#define M2C_FIELD(expr, ptype, offset) (*(ptype)(gaddr(expr) + (u32)(offset)))
#define GIDX(T, ptr, idx) (gp<T>(gaddr(ptr))[(idx)])

template <class X>
static inline auto m2c_num(X x) {
    if constexpr (std::is_arithmetic<X>::value)
        return x;
    else
        return (u32)x;
}

// call through a guest code address with the uniform register ABI (u32 ints, f64 floats). A call
// site knows only the arguments it sets up, and the target may be defined with more (it reads
// registers this caller leaves alone). Every call therefore passes all eight integer argument
// registers: 32-bit ARM passes the 5th to 8th on the stack, and a callee that has them as
// parameters owns those stack words (GCC stores to them, and reuses them for tail calls), so
// they must exist in the caller's frame -- otherwise the callee overwrites the caller's spills.
namespace guest {
template <class A>
using greg_t = std::conditional_t<std::is_floating_point<A>::value, f64, u32>;
template <class R, class... A, size_t... I>
static inline R gcall_regs(void* f, std::index_sequence<I...>, A... args) {
    return ((R(*)(A..., decltype((void)I, 0u)...))f)(args..., ((void)I, 0u)...);
}
}  // namespace guest
template <class R, class... A>
static inline R gcall(u32 fn, A... args) {
    constexpr size_t ints = (0 + ... + (std::is_floating_point<A>::value ? 0 : 1));
    static_assert(ints <= 8, "more integer arguments than PowerPC passes in registers");
    return guest::gcall_regs<R>(guest::lookup_fn(fn), std::make_index_sequence<8 - ints>{},
                                (guest::greg_t<A>)m2c_num(args)...);
}
template <class To, class From>
static inline To m2c_bitcast(From f) {
    if constexpr (sizeof(To) == sizeof(From)) {
        To t;
        ::memcpy(&t, &f, sizeof(To));
        return t;
    } else {
        // only from prototype guesses that disagree about a register's width; keep the value
        auto v = m2c_num(f);
        if constexpr (std::is_arithmetic<To>::value)
            return (To)v;
        else
            return To((u32)v);
    }
}
template <class X>
static inline X gval(X x) {
    return x;
}
template <class T>
static inline T gval(const gref<T>& r) {
    return r.get();
}
#define M2C_BITWISE(type, expr) (m2c_bitcast<type>(gval(expr)))
// a 64-bit result in r3 (high word, kept in the low half here) and r4: register contents, so a
// float stored in a stack slot and loaded into r3/r4 (a two-float struct returned in registers)
// moves as bits
template <class X>
static inline u32 noma_reg_bits(X x) {
    auto v = gval(x);
    if constexpr (std::is_same<decltype(v), f32>::value) {
        u32 b;
        ::memcpy(&b, &v, 4);
        return b;
    } else if constexpr (std::is_floating_point<decltype(v)>::value) {
        f32 f = (f32)v;
        u32 b;
        ::memcpy(&b, &f, 4);
        return b;
    } else {
        return (u32)m2c_num(v);
    }
}
template <class H, class L>
static inline u64 NOMA_U64(H hi, L lo) {
    return ((u64)noma_reg_bits(lo) << 32) | (u64)noma_reg_bits(hi);
}
// Float to integer as the PowerPC converts. fctiwz: toward zero, saturating, NaN to INT_MIN;
// CodeWarrior's __cvt_fp2unsigned (the unsigned conversion): negative to 0, NaN and 2^32 or more
// to 0xFFFFFFFF. A C++ cast of a value out of range is undefined, and x86 and ARM disagree.
static inline s32 NOMA_FCTIWZ(f64 x) {
    if (!(x == x) || x <= -2147483648.0)
        return (s32)0x80000000u;
    if (x >= 2147483647.0)
        return 0x7FFFFFFF;
    return (s32)x;
}
static inline u32 NOMA_FP2U(f64 x) {
    if (x < 0.0)
        return 0;
    if (!(x < 4294967296.0))
        return 0xFFFFFFFFu;
    if (x < 2147483648.0)
        return (u32)(s32)x;
    return (u32)(s32)(x - 2147483648.0) + 0x80000000u;
}

// PowerPC shifts by a register use six bits of it: 32-63 shift everything out (slw/srw give 0,
// sraw the sign). CodeWarrior's inline 64-bit shifts rely on that; in C++ it's undefined.
template <class X, class N>
static inline u32 NOMA_SLW(X x, N n) {
    u32 a = (u32)m2c_num(gval(x)), s = (u32)m2c_num(gval(n));
    return (s & 32) ? 0u : a << (s & 31);
}
template <class X, class N>
static inline u32 NOMA_SRW(X x, N n) {
    u32 a = (u32)m2c_num(gval(x)), s = (u32)m2c_num(gval(n));
    return (s & 32) ? 0u : a >> (s & 31);
}
template <class X, class N>
static inline u32 NOMA_ROTL(X x, N n) {
    u32 a = (u32)m2c_num(gval(x)), s = (u32)m2c_num(gval(n)) & 31;
    return s ? (a << s) | (a >> (32 - s)) : a;
}
template <class X, class N>
static inline s32 NOMA_SRAW(X x, N n) {
    s32 a = (s32)(u32)m2c_num(gval(x));
    u32 s = (u32)m2c_num(gval(n));
    return a >> ((s & 32) ? 31 : (s & 31));
}
void noma_m2c_error(const char* where);
#define M2C_ERROR(desc) (noma_m2c_error(__func__), 0)
#define M2C_TRAP_IF(cond) ((cond) ? (noma_m2c_error(__func__), 0) : 0)
#define M2C_BREAK() (noma_m2c_error(__func__), 0)
#define M2C_SYNC() ((void)0)
#define M2C_DCACHE_CLEAN(addr) ((void)0)
#define M2C_DCACHE_INVALIDATE(addr) ((void)0)
#define M2C_DCACHE_CLEAN_INVALIDATE(addr) ((void)0)
#define M2C_DCACHE_BLOCK_SETZERO(addr) (guest::memset_g(gaddr(addr) & ~31u, 0, 32))
#define M2C_DCACHE_BLOCK_SETZERO_LOCKED(addr) (guest::memset_g(gaddr(addr) & ~31u, 0, 32))
#define M2C_ICACHE_INVALIDATE(addr) ((void)0)
#define M2C_PREFETCH(addr) ((void)0)
#define M2C_PREFETCH_STORE(addr) ((void)0)
#define MULT_HI(a, b) ((s32)(((s64)(s32)(a) * (s64)(s32)(b)) >> 32))
#define MULTU_HI(a, b) ((u32)(((u64)(u32)(a) * (u64)(u32)(b)) >> 32))
// divw/divwu by a register: the hardware never traps; by 0 (or INT_MIN / -1) the quotient is
// undefined and Dolphin's values are used (the remainder idiom, divw + mullw + subf, follows)
static inline s32 NOMA_DIVW(s32 a, s32 b) {
    if (b == 0 || (a == INT32_MIN && b == -1))
        return a < 0 ? -1 : 0;
    return a / b;
}
static inline u32 NOMA_DIVWU(u32 a, u32 b) { return b == 0 ? 0u : a / b; }
static inline s32 NOMA_MODW(s32 a, s32 b) { return (s32)((u32)a - (u32)NOMA_DIVW(a, b) * (u32)b); }
static inline u32 NOMA_MODWU(u32 a, u32 b) { return a - NOMA_DIVWU(a, b) * b; }
// (a function, not a macro: the argument is often a call, which must happen once)
static inline u32 noma_clz(u32 x) { return x ? (u32)__builtin_clz(x) : 32u; }
#define CLZ(x) noma_clz((u32)(x))
#define M2C_OVERFLOW(a) (0)
#define M2C_STRUCT_COPY(dst, src, n) (guest::memcpy_g(gaddr(dst), gaddr(src), (u32)(n)))
#define M2C_MEMCPY_ALIGNED(dst, src, n) (guest::memcpy_g(gaddr(dst), gaddr(src), (u32)(n)))
#define M2C_MEMCPY_UNALIGNED(dst, src, n) (guest::memcpy_g(gaddr(dst), gaddr(src), (u32)(n)))

// PowerPC intrinsics used by the decompiled code (runtime/cpu.cpp)
void __mtspr(u32 spr, u32 v);
u32 __mfspr(u32 spr);
u32 __mftb(void);
u32 __mftbu(void);
u32 __mfmsr(void);
void __mtmsr(u32 v);
f32 __psq_ld0(u32 ea, u32 qr);
f32 __psq_ld1(u32 ea, u32 w, u32 qr);
void __psq_st(u32 ea, f64 ps0, f64 ps1, u32 w, u32 qr);
static inline f64 __fres(f64 x) { return 1.0 / x; }
static inline f64 __frsqrte(f64 x) { return 1.0 / ::sqrt(x); }
// carry out of x + y + CA (adde/subfe and friends; subtractions pass ~rA)
static inline u32 __adde_ca(u32 x, u32 y, u32 ca) {
    return (u32)(((u64)x + (u64)y + (u64)(ca & 1)) >> 32);
}
static inline u32 __srawi_ca(u32 x, u32 sh) {
    return ((s32)x < 0 && (x & ((1u << sh) - 1)) != 0) ? 1u : 0u;
}
static inline u32 __sraw_ca(u32 x, u32 rb) {
    u32 sh = rb & 0x3F;
    if (sh >= 32)
        return (s32)x < 0 ? 1u : 0u;
    return ((s32)x < 0 && (x & ((1u << sh) - 1)) != 0) ? 1u : 0u;
}
static inline u32 __lwbrx(u32 ea) { return __builtin_bswap32(guest::load32(ea)); }
static inline u32 __lhbrx(u32 ea) { return __builtin_bswap16(guest::load16(ea)); }
static inline void __stwbrx(u32 ea, u32 v) { guest::store32(ea, __builtin_bswap32(v)); }
static inline void __sthbrx(u32 ea, u32 v) { guest::store16(ea, __builtin_bswap16((u16)v)); }
static inline u32 __lwarx(u32 ea) { return guest::load32(ea); }
static inline void __stwcx(u32 ea, u32 v) { guest::store32(ea, v); }
// A 64-bit result travels in r3 (its high word) and r4 (its low word). On the host the pair is
// packed with r3 in the LOW half, so that a caller that reads only r3 -- a 32-bit call through
// a function pointer (gcall<u32>) to a function that happens to return 64 bits -- gets r3, as
// on the console. Generated code only ever takes such values apart with these macros. r4 after
// a call whose result is 32 bits (or a float) holds nothing the callee set for its caller: 0
// then (widening a signed result would give 0xFFFFFFFF, a pointer that faults).

#define NOMA_HI32(x) ((u32)(u64)(x))
template <class T>
static inline u32 NOMA_LO32(T x) {
    if constexpr (std::is_integral<T>::value && sizeof(T) == 8)
        return (u32)((u64)x >> 32);
    else
        return 0;
}

// mfcr: one CR bit (a condition) placed in a word
#define NOMA_CRBIT(cond, shift) ((u32)(bool)(cond) << (shift))
// an unconditional trap instruction (a debugger breakpoint)
void noma_trap(const char* where);
#define NOMA_TRAP() noma_trap(__func__)

// loop poll points (tools/decomp/decompile.py add_loop_polls): interrupts can be taken in any
// loop, and an endless empty loop is reported
namespace guest {
extern s32 g_loop_budget;
void loop_poll();
void spin_poll(const char* fn, u32 addr);
}  // namespace guest
#define NOMA_LOOP()                                                \
    do {                                                           \
        if (__builtin_expect(--guest::g_loop_budget < 0, 0))       \
            guest::loop_poll();                                    \
    } while (0)
#define NOMA_SPIN(name, addr) guest::spin_poll(name, addr)

// The sampling profiler (runtime/profile.cpp, builds with -DNOMA_PROFILE_CALLS): every guest
// thread keeps the stack of functions it is in, which a sampler thread reads. NOMA_PROF_SCOPE
// marks a function, or a part of the runtime as "(name)", for as long as its scope lasts.
#ifdef NOMA_PROFILE_CALLS
namespace guest {
static const u32 PROF_DEPTH = 128;
struct ProfStack {
    volatile u32 depth = 0;
    const char* volatile names[PROF_DEPTH];
    u32 thread = 0;  // the guest thread's OSContext (0: the main thread)
};
extern ProfStack* volatile g_prof;   // the running guest thread's
extern thread_local ProfStack* t_prof;  // a host thread's own stack (the GPU's thread), else null
static inline ProfStack* prof_stack() {
    ProfStack* s = t_prof;
    return s ? s : g_prof;
}
struct ProfScope {
    ProfStack* s;
    u32 d;
    explicit ProfScope(const char* name) : ProfScope(prof_stack(), name) {}
    // guest code, which only ever runs on the thread of the guest threads: no thread-local lookup
    ProfScope(ProfStack* stack, const char* name) : s(stack), d(s->depth) {
        if (d < PROF_DEPTH)
            s->names[d] = name;
        s->depth = d + 1;
    }
    ~ProfScope() { s->depth = d; }  // also unwinds what a longjmp skipped
};
// where a longjmp may come back to: unwind() drops the scopes it skipped
struct ProfMark {
    ProfStack* s = prof_stack();
    u32 d = s->depth;
    void unwind() const { s->depth = d; }
};
}  // namespace guest
#define NOMA_PROF_CAT2(a, b) a##b
#define NOMA_PROF_CAT(a, b) NOMA_PROF_CAT2(a, b)
#define NOMA_PROF_SCOPE(name) guest::ProfScope NOMA_PROF_CAT(noma_prof_scope_, __LINE__)(name)
#else
namespace guest {
struct ProfMark {
    void unwind() const {}
};
}  // namespace guest
#define NOMA_PROF_SCOPE(name) ((void)0)
#endif

// Every decompiled function starts with NOMA_TRACE. Function entries count toward the device
// poll budget like loop iterations do: hardware interrupts (the 3 ms audio DMA, VI) get taken
// throughout the code, not only where the guest loops or re-enables interrupts.
// (A build with -DNOMA_TRACE_CALLS also logs function entries; see runtime/trace.cpp.)
#ifdef NOMA_TRACE_CALLS
void noma_trace(const char* name, u32 addr);
#define NOMA_TRACE(name, addr) \
    do {                       \
        noma_trace(name, addr); \
        NOMA_LOOP();           \
    } while (0)
#elif defined(NOMA_PROFILE_CALLS)
#define NOMA_TRACE(name, addr)                                          \
    guest::ProfScope NOMA_PROF_CAT(noma_prof_scope_, __LINE__)(guest::g_prof, name); \
    NOMA_LOOP()
#else
#define NOMA_TRACE(name, addr) NOMA_LOOP()
#endif

// an argument register the caller never set (only ever passed through to unused parameters)
#define M2C_UNSET(desc) (0)
