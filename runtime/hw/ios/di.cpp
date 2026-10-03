// /dev/di: the disc drive. Wii no Ma is a channel and never reads a disc, but the SDK's DVD
// library starts in OSInit and asks the drive about itself, so this is a drive with no disc in
// it. Each ioctl's input is a 0x20-byte command block (the command again in its first byte);
// the result is a DI status (1 success, 2 drive error), not an IOS error.
#include "ios.h"

namespace guest {

enum : s32 { DI_SUCCESS = 1, DI_DRIVE_ERROR = 2 };
static const u32 DI_ERROR_NO_DISC = 0x01023A00;  // cover open, medium not present

class DiDevice : public IosHandle {
    bool pending_ = false;

    static void out32(u32 out, u32 out_len, u32 v) {
        if (out && out_len >= 4)
            store32(out, v);
    }

public:
    s32 ioctl(u32 cmd, u32 in, u32 in_len, u32 out, u32 out_len) override {
        pending_ = false;
        switch (cmd) {
        case 0x12:  // Inquiry: revision 0, device code 2, release date (a retail drive's answer)
            out32(out, out_len, 0x00000002);
            if (out_len >= 8)
                store32(out + 4, 0x20060526);
            return DI_SUCCESS;
        case 0x79:  // WaitForCoverClose: no disc is coming; stays pending
            pending_ = true;
            return 0;
        case 0x7A:  // GetCoverRegister: DICVR with the cover-open bit
            out32(out, out_len, 1);
            return DI_SUCCESS;
        case 0x88:  // GetCoverStatus: 1 = no disc
            out32(out, out_len, 1);
            return DI_SUCCESS;
        case 0xE0:  // RequestError
            out32(out, out_len, DI_ERROR_NO_DISC);
            return DI_SUCCESS;
        case 0x84:  // GetImmBuf
        case 0x95:  // GetStatusRegister
        case 0x96:  // GetControlRegister
            out32(out, out_len, 0);
            return DI_SUCCESS;
        case 0x7E:  // NotifyReset
        case 0x7F:  // SetSpinupFlag
        case 0x85:  // MaskCoverInterrupt
        case 0x86:  // ClearCoverInterrupt
        case 0x87:  // UnmaskStatusInterrupts
        case 0x89:  // UnmaskCoverInterrupt
        case 0x8A:  // Reset
        case 0x8C:  // ClosePartition
        case 0x8E:  // EnableDvdVideo
        case 0xD2:  // StopLaser
        case 0xE3:  // StopMotor
        case 0xE4:  // AudioBufferConfig
            return DI_SUCCESS;
        case 0x70:  // ReadDiskID
        case 0x71:  // Read
        case 0x83:  // GetLength
        case 0x8D:  // UnencryptedRead
        case 0xAB:  // Seek
        case 0xD0:  // ReadDvd
        case 0xDA:  // ReadDiskBca
        case 0xDB:  // RequestDiscStatus
            return DI_DRIVE_ERROR;
        default:
            log("ios: /dev/di: unhandled ioctl 0x%02X", cmd);
            return DI_DRIVE_ERROR;
        }
    }

    s32 ioctlv(u32 cmd, const std::vector<IoVec>& in, const std::vector<IoVec>& io) override {
        pending_ = false;
        if (cmd != 0x8B)  // OpenPartition (no disc, no partition)
            log("ios: /dev/di: unhandled ioctlv 0x%02X", cmd);
        return DI_DRIVE_ERROR;
    }

    bool reply_now() override { return !pending_; }
};

std::unique_ptr<IosHandle> di_open_device() { return std::unique_ptr<IosHandle>(new DiDevice()); }

}  // namespace guest
