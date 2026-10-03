// IOS (the Wii's ARM-side OS) emulation: the requests the decompiled SDK sends over IPC.
#pragma once
#include <memory>
#include <string>
#include <vector>

#include "../hw.h"

namespace guest {

void ios_execute(u32 request_phys);
void ipc_reply(u32 request_phys);
void ios_init(const std::string& nand_root, const std::string& content_dir);

// IOS error codes
enum : s32 {
    IOS_OK = 0,
    IOS_EACCES = -1,
    IOS_EEXIST = -2,
    IOS_EINVAL = -4,
    IOS_ENOENT = -6,
    IOS_ENOMEM = -22,
    FS_EINVAL = -101,
    FS_EACCESS = -102,
    FS_ECORRUPT = -103,
    FS_EEXIST = -105,
    FS_ENOENT = -106,
    FS_ENFILE = -107,
    FS_EFBIG = -108,
    FS_EFDEXHAUSTED = -109,
    FS_ENAMELEN = -110,
    FS_EFDOPEN = -111,
    FS_EIO = -114,
    FS_ENOTEMPTY = -115,
    FS_EDIRDEPTH = -116,
    FS_EBUSY = -118,
    ES_EINVAL = -1017,
    ES_ENOENT = -1106,
};

static inline u32 phys_to_virt(u32 p) { return p ? (p | 0x80000000u) : 0; }

struct IoVec {
    u32 addr;  // virtual
    u32 size;
};

// One open handle (an IOS fd).
class IosHandle {
public:
    virtual ~IosHandle() {}
    virtual s32 close() { return IOS_OK; }
    virtual s32 read(u32 buf, u32 len) { return IOS_EINVAL; }
    virtual s32 write(u32 buf, u32 len) { return IOS_EINVAL; }
    virtual s32 seek(s32 off, u32 whence) { return IOS_EINVAL; }
    virtual s32 ioctl(u32 cmd, u32 in, u32 in_len, u32 out, u32 out_len);
    virtual s32 ioctlv(u32 cmd, const std::vector<IoVec>& in, const std::vector<IoVec>& io);
    // false: keep the request pending (event hooks); it is answered later with ios_complete()
    virtual bool reply_now() { return true; }
    std::string name;
    u32 cur_request = 0;  // physical address of the request being handled (for ios_complete)
};

// Answer a request that a device kept pending.
void ios_complete(u32 request_phys, s32 result);

// device factories (by path)
std::unique_ptr<IosHandle> fs_open_device();
std::unique_ptr<IosHandle> fs_open_file(const std::string& path, u32 mode, s32* err);
std::unique_ptr<IosHandle> es_open_device();
std::unique_ptr<IosHandle> di_open_device();
std::unique_ptr<IosHandle> stm_open_device(const std::string& path);
// press the Wii's power button (STM's event hook): false if the game isn't waiting for it
bool stm_power_event();
std::unique_ptr<IosHandle> net_open_device(const std::string& path, s32* err);
std::unique_ptr<IosHandle> usb_hid_open_device();

// host paths
std::string nand_host_path(const std::string& nand_path);
const std::string& content_dir();

}  // namespace guest
