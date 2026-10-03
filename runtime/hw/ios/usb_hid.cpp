// /dev/usb/hid: IOS's USB HID module (interface version 4). The SDK's KBD library (USB keyboards,
// text entry in the shopping room) talks to it; on a 3DS no USB device is ever attached, so this
// is the module with an empty bus. GetDeviceChange answers its first request with the (empty)
// device list at once and keeps the next one pending, as IOS does until a device comes or goes.
#include "ios.h"

namespace guest {

class HidDevice : public IosHandle {
    enum : u32 {
        GET_DEVICE_CHANGE = 0,
        GET_VERSION = 6,
        SHUTDOWN = 7,
        CANCEL_INTERRUPT = 8,
    };
    static const u32 VERSION = 0x40001;
    static const u32 DEVICE_LIST_SIZE = 0x600;

    bool pending_ = false;
    bool listed_ = false;     // the first GetDeviceChange was answered
    u32 hook_ = 0, hook_out_ = 0;  // the pending GetDeviceChange (physical request, its list)

public:
    s32 ioctl(u32 cmd, u32 in, u32 in_len, u32 out, u32 out_len) override {
        pending_ = false;
        switch (cmd) {
        case GET_VERSION:
            return VERSION;
        case GET_DEVICE_CHANGE:
            if (!out || out_len != DEVICE_LIST_SIZE)
                return IOS_EINVAL;
            if (!listed_) {
                listed_ = true;
                store32(out, 0xFFFFFFFF);  // the end of the list: no devices
                return IOS_OK;
            }
            pending_ = true;
            hook_ = cur_request, hook_out_ = out;
            return IOS_OK;
        case SHUTDOWN:
            // the pending GetDeviceChange ends with an empty list and -1
            if (hook_) {
                store32(hook_out_, 0xFFFFFFFF);
                ios_complete(hook_, -1);
                hook_ = 0;
            }
            return IOS_OK;
        case CANCEL_INTERRUPT:
            return IOS_OK;
        default:
            // the transfers name a device, and there are none
            log("ios: /dev/usb/hid: ioctl %u with no device attached", cmd);
            return IOS_EINVAL;
        }
    }

    bool reply_now() override { return !pending_; }
};

std::unique_ptr<IosHandle> usb_hid_open_device() { return std::unique_ptr<IosHandle>(new HidDevice()); }

}  // namespace guest
