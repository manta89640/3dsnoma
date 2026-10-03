// EXI (external interface): three serial channels. On a Wii only channel 0 device 1 is wired:
// the "MX" chip with the ROM (fonts), the RTC, 64 bytes of battery-backed SRAM and a debug UART.
//
// Registers, per channel at n*0x14:
//   0x00 CSR    0 EXIINTMSK 1 EXIINT 2 TCINTMSK 3 TCINT 4-6 clock 7-9 chip select (dev 0/1/2)
//               10 EXTINTMSK 11 EXTINT 12 EXT (device present) 13 ROMDIS
//   0x04 MAR    DMA address   0x08 LENGTH   0x10 DATA (immediate, MSB first)
//   0x0C CR     0 TSTART 1 DMA 2-3 RW (0 read, 1 write, 2 read/write) 4-5 immediate length-1
// Transfers complete at once; TCINT then raises the EXI interrupt when TCINTMSK is set.
//
// MX chip: after chip select, the first 4 bytes are a command, bit 31 = write and
// (command & 0x7FFFFFFF) >> 6 = the address; following bytes read/write from there on.
#include <cstring>
#include <ctime>
#include <string>

#include "hw.h"

namespace guest {

// local time in seconds since 2000-01-01, like the console's clock
s64 rtc_seconds() {
    time_t t = time(nullptr);
#ifdef __3DS__
    return (s64)t - 946684800;  // the 3DS clock is local time already
#else
    struct tm lt;
    localtime_r(&t, &lt);
    return (s64)t + lt.tm_gmtoff - 946684800;
#endif
}

enum : u32 {
    CSR_EXIINTMSK = 1u << 0,
    CSR_EXIINT = 1u << 1,
    CSR_TCINTMSK = 1u << 2,
    CSR_TCINT = 1u << 3,
    CSR_CS_SHIFT = 7,
    CSR_CS_MASK = 7u << 7,
    CSR_EXTINTMSK = 1u << 10,
    CSR_EXTINT = 1u << 11,
    CSR_EXT = 1u << 12,
    CSR_ROMDIS = 1u << 13,
    CR_TSTART = 1u << 0,
    CR_DMA = 1u << 1,
};

// ---- the MX chip (channel 0, device 1)
class MxChip {
public:
    void select() {
        cmd_bytes_ = 0;
        cmd_ = 0;
        cursor_ = 0;
    }
    void deselect() { flush_uart(); }

    u8 transfer(u8 in) {
        if (cmd_bytes_ < 4) {
            cmd_ = (cmd_ << 8) | in;
            cmd_bytes_++;
            return 0;
        }
        bool write = (cmd_ >> 31) != 0;
        u32 addr = (cmd_ & 0x7FFFFFFF) >> 6;
        u32 at = cursor_++;
        if (addr < ROM_SIZE) {
            return write ? 0 : rom_byte(addr + at);
        }
        if (addr == RTC_ADDR) {  // 4 bytes, big-endian seconds since 2000-01-01
            if (write) {
                rtc_set_[at & 3] = in;
                if ((at & 3) == 3) {
                    u32 v = (rtc_set_[0] << 24) | (rtc_set_[1] << 16) | (rtc_set_[2] << 8) | rtc_set_[3];
                    rtc_offset_ = (s64)v - (s64)rtc_now();
                }
                return 0;
            }
            u32 v = (u32)(rtc_now() + rtc_offset_);
            return (u8)(v >> (24 - 8 * (at & 3)));
        }
        if (addr >= SRAM_ADDR && addr < SRAM_ADDR + sizeof(sram_)) {
            // the SDK sends RTC_SRAM_ADDR + (byte offset << 6): address = SRAM_ADDR + offset
            u32 k = (addr - SRAM_ADDR) + at;
            if (k >= sizeof(sram_))
                return 0;
            if (write) {
                sram_[k] = in;
                return 0;
            }
            return sram_[k];
        }
        if (addr == UART_ADDR) {
            if (write)
                uart_ += (char)in;
            return 0;
        }
        static u32 s_logged;
        if (s_logged != addr) {
            s_logged = addr;
            log("exi: MX %s at unknown address %06X", write ? "write" : "read", addr);
        }
        return 0;
    }

    void init() {
        memset(sram_, 0, sizeof(sram_));
        sram_[0x13] = 0x2C;  // flags: stereo, ...
        fix_sram_checksum();
    }

private:
    static const u32 ROM_SIZE = 0x200000, RTC_ADDR = 0x800000, SRAM_ADDR = 0x800004,
                     UART_ADDR = 0x800400;
    u32 cmd_ = 0, cmd_bytes_ = 0, cursor_ = 0;
    u8 sram_[64];
    u8 rtc_set_[4] = {};
    s64 rtc_offset_ = 0;
    std::string uart_;

    static s64 rtc_now() { return rtc_seconds(); }

    // The fonts in the ROM aren't ours to ship; OSFatal's text is all that uses them.
    static u8 rom_byte(u32 a) { return 0; }

    void fix_sram_checksum() {
        u16 sum = 0, inv = 0;
        for (int o = 0x0C; o < 0x14; o += 2) {
            u16 w = (u16)((sram_[o] << 8) | sram_[o + 1]);
            sum += w;
            inv += (u16)~w;
        }
        sram_[0] = sum >> 8;
        sram_[1] = (u8)sum;
        sram_[2] = inv >> 8;
        sram_[3] = (u8)inv;
    }

    void flush_uart() {
        size_t p;
        while ((p = uart_.find('\n')) != std::string::npos) {
            log("uart: %s", uart_.substr(0, p).c_str());
            uart_.erase(0, p + 1);
        }
    }
};

static MxChip s_mx;

struct Channel {
    u32 csr = 0, mar = 0, len = 0, cr = 0, data = 0;
    int selected = -1;  // device number, or -1
};
static Channel s_ch[3];

static bool present(int ch, int dev) { return ch == 0 && dev == 1; }

static u8 transfer_byte(int ch, u8 in) {
    if (s_ch[ch].selected == 1 && ch == 0)
        return s_mx.transfer(in);
    return 0xFF;  // nothing there
}

static void update_irq() {
    bool any = false;
    for (auto& c : s_ch) {
        if ((c.csr & CSR_EXIINT) && (c.csr & CSR_EXIINTMSK))
            any = true;
        if ((c.csr & CSR_TCINT) && (c.csr & CSR_TCINTMSK))
            any = true;
        if ((c.csr & CSR_EXTINT) && (c.csr & CSR_EXTINTMSK))
            any = true;
    }
    if (any)
        pi_raise(PI_INT_EXI);
    else
        pi_clear(PI_INT_EXI);
}

static void select_device(int ch, u32 cs_bits) {
    int dev = cs_bits & 1 ? 0 : cs_bits & 2 ? 1 : cs_bits & 4 ? 2 : -1;
    Channel& c = s_ch[ch];
    if (dev == c.selected)
        return;
    if (c.selected == 1 && ch == 0)
        s_mx.deselect();
    c.selected = dev;
    if (dev == 1 && ch == 0)
        s_mx.select();
}

static void start_transfer(int ch) {
    Channel& c = s_ch[ch];
    u32 rw = (c.cr >> 2) & 3;
    if (c.cr & CR_DMA) {
        u32 addr = c.mar & 0x1FFFFFE0, n = c.len & ~31u;
        for (u32 i = 0; i < n; i++) {
            u32 va = (addr + i) | 0x80000000;
            if (rw == 1) {
                transfer_byte(ch, load8(va));
            } else {
                u8 b = transfer_byte(ch, rw == 2 ? load8(va) : 0);
                store8(va, b);
            }
        }
    } else {
        u32 n = ((c.cr >> 4) & 3) + 1;
        u32 out = 0;
        for (u32 i = 0; i < n; i++) {
            u8 send = (u8)(c.data >> (24 - 8 * i));
            u8 got = transfer_byte(ch, rw == 0 ? 0 : send);
            out |= (u32)got << (24 - 8 * i);
        }
        if (rw != 1)
            c.data = out;
    }
    c.cr &= ~CR_TSTART;
    c.csr |= CSR_TCINT;
    update_irq();
}

void exi_init() {
    for (auto& c : s_ch)
        c = Channel();
    s_mx.init();
}

u32 exi_read(u32 off, int size) {
    u32 ch = off / 0x14, reg = off % 0x14;
    if (ch >= 3 || size != 4) {
        mmio_log_unhandled("EXI", off, 0, size, false);
        return 0;
    }
    Channel& c = s_ch[ch];
    switch (reg) {
    case 0x00: {
        u32 v = c.csr & ~CSR_EXT;
        // EXT: a device is attached to the channel (memory card slots; nothing on a Wii)
        return v;
    }
    case 0x04: return c.mar;
    case 0x08: return c.len;
    case 0x0C: return c.cr;
    case 0x10: return c.data;
    }
    return 0;
}

void exi_write(u32 off, u32 v, int size) {
    u32 ch = off / 0x14, reg = off % 0x14;
    if (ch >= 3 || size != 4) {
        mmio_log_unhandled("EXI", off, v, size, true);
        return;
    }
    Channel& c = s_ch[ch];
    switch (reg) {
    case 0x00: {
        u32 ack = v & (CSR_EXIINT | CSR_TCINT | CSR_EXTINT);  // write 1 to clear
        u32 keep = c.csr & (CSR_EXIINT | CSR_TCINT | CSR_EXTINT) & ~ack;
        u32 romdis = (c.csr | v) & CSR_ROMDIS;  // stays set once set
        c.csr = (v & ~(CSR_EXIINT | CSR_TCINT | CSR_EXTINT | CSR_ROMDIS | CSR_EXT)) | keep | romdis;
        select_device((int)ch, (c.csr & CSR_CS_MASK) >> CSR_CS_SHIFT);
        update_irq();
        break;
    }
    case 0x04: c.mar = v; break;
    case 0x08: c.len = v; break;
    case 0x0C:
        c.cr = v;
        if (v & CR_TSTART)
            start_transfer((int)ch);
        break;
    case 0x10: c.data = v; break;
    }
}

}  // namespace guest
