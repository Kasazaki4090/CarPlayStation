#include "Core/Hardware/TouchscreenCapabilities.h"

#include <windows.h>

#include <string>
#include <vector>

namespace CarPlayStation::Core::Hardware {

namespace {

std::string ToUtf8(const wchar_t* text) {
    if (!text || text[0] == L'\0') {
        return std::string();
    }
    const int len = ::WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1) {
        return std::string();
    }
    std::string out(static_cast<size_t>(len - 1), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), len, nullptr, nullptr);
    return out;
}

} // namespace

TouchscreenCapabilities DetectTouchscreenCapabilities() {
    TouchscreenCapabilities caps;

    UINT32 deviceCount = 0;
    if (!::GetPointerDevices(&deviceCount, nullptr) || deviceCount == 0) {
        return caps;
    }

    std::vector<POINTER_DEVICE_INFO> devices(deviceCount);
    if (!::GetPointerDevices(&deviceCount, devices.data())) {
        return caps;
    }

    for (const auto& device : devices) {
        // Touchpads and pens are skipped on purpose: the UI pointer router only
        // forwards PT_TOUCH, so those devices could never fill a second contact.
        if (device.pointerDeviceType != POINTER_DEVICE_TYPE_TOUCH) {
            continue;
        }
        // Some drivers report 0 when they do not publish the limit; the system
        // metric is the documented fallback in that case. SM_MAXIMUM_TOUCHPOINTS
        // is not declared by every SDK, so the documented value is used inline.
        int contacts = static_cast<int>(device.maxActiveContacts);
        if (contacts < 1) {
            contacts = ::GetSystemMetrics(95 /* SM_MAXIMUM_TOUCHPOINTS */);
        }
        if (contacts < 1) {
            continue;
        }
        if (!caps.has_touch_screen || contacts > caps.max_contacts) {
            caps.has_touch_screen = true;
            caps.max_contacts = contacts;
            caps.device_description = ToUtf8(device.productString);
        }
    }

    return caps;
}

int DetectMaxTouchContacts() {
    const TouchscreenCapabilities caps = DetectTouchscreenCapabilities();
    return caps.max_contacts >= 2 ? 2 : 1;
}

} // namespace CarPlayStation::Core::Hardware
