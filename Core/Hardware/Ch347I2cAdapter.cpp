#include "Core/Hardware/Ch347I2cAdapter.h"
#include "Core/Hardware/Ch341Protocol.h"
#include "Core/Hardware/DriverDllLookup.h"
#include "Core/Common/Logging.h"
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace CarPlayStation::Core::Hardware {

#ifdef _WIN32
using PFN_CH347OpenDevice   = HANDLE(WINAPI*)(ULONG iIndex);
using PFN_CH347CloseDevice  = BOOL(WINAPI*)(ULONG iIndex);
using PFN_CH347I2C_Init     = BOOL(WINAPI*)(ULONG iIndex, ULONG iClockFreq);
using PFN_CH347StreamI2C    = BOOL(WINAPI*)(ULONG iIndex, ULONG iWriteLength, PVOID iWriteBuffer, ULONG iReadLength, PVOID oReadBuffer);
using PFN_CH347GPIO_Set     = BOOL(WINAPI*)(ULONG iIndex, UCHAR iEnable, UCHAR iSetDirOut, UCHAR iSetDataOut);
#endif

// Bus idle gap held before a pure read. See the comment in Transaction().
static constexpr std::chrono::milliseconds kSelectToReadGap{5};

// The WCH CH347 driver ships in DriverStore\FileRepository\ch341wdm.inf_* (the
// same INF covers the CH347), which is not on the default DLL search path, and
// the 32-bit CH347DLL.DLL cannot be loaded into a 64-bit process. Probe the
// exe directory, System32 and the DriverStore, preferring the A64 build on x64.
static std::vector<std::string> BuildCh347DllCandidates() {
#ifdef _WIN64
    const std::vector<std::string> names = {"CH347DLLA64.DLL", "CH347DLL.DLL"};
#else
    const std::vector<std::string> names = {"CH347DLL.DLL", "CH347DLLA64.DLL"};
#endif
    return BuildDriverDllCandidates(names, {"ch341wdm.inf", "ch347wdm.inf"});
}

class Ch347I2cAdapter::DriverBinding {
public:
    DriverBinding() {
#ifdef _WIN32
        for (const auto& candidate : BuildCh347DllCandidates()) {
            h_dll_ = LoadLibraryA(candidate.c_str());
            if (!h_dll_) continue;
            fn_open_device_  = reinterpret_cast<PFN_CH347OpenDevice>(GetProcAddress(h_dll_, "CH347OpenDevice"));
            fn_close_device_ = reinterpret_cast<PFN_CH347CloseDevice>(GetProcAddress(h_dll_, "CH347CloseDevice"));
            fn_i2c_init_     = reinterpret_cast<PFN_CH347I2C_Init>(GetProcAddress(h_dll_, "CH347I2C_Init"));
            fn_stream_i2c_   = reinterpret_cast<PFN_CH347StreamI2C>(GetProcAddress(h_dll_, "CH347StreamI2C"));
            fn_gpio_set_     = reinterpret_cast<PFN_CH347GPIO_Set>(GetProcAddress(h_dll_, "CH347GPIO_Set"));
            if (fn_open_device_ && fn_close_device_ && fn_stream_i2c_) {
                loaded_path_ = candidate;
                break;
            }
            FreeLibrary(h_dll_);
            h_dll_ = nullptr;
        }
        if (IsValid()) {
            CPS_LOG_INFO("Ch347I2c", "CH347 driver loaded from: ", loaded_path_);
        } else {
            CPS_LOG_WARN("Ch347I2c", "CH347 driver DLL not found in any candidate location "
                "(exe dir, System32, DriverStore ch341wdm.inf_*). Install the WCH CH347 driver "
                "or place CH347DLLA64.DLL next to the executable.");
        }
#endif
    }

    ~DriverBinding() {
#ifdef _WIN32
        if (h_dll_) {
            FreeLibrary(h_dll_);
            h_dll_ = nullptr;
        }
#endif
    }

    [[nodiscard]] bool IsValid() const noexcept {
#ifdef _WIN32
        return h_dll_ && fn_open_device_ && fn_close_device_ && fn_stream_i2c_;
#else
        return false;
#endif
    }

#ifdef _WIN32
    HMODULE h_dll_{nullptr};
    std::string loaded_path_;
    PFN_CH347OpenDevice  fn_open_device_{nullptr};
    PFN_CH347CloseDevice fn_close_device_{nullptr};
    PFN_CH347I2C_Init    fn_i2c_init_{nullptr};
    PFN_CH347StreamI2C   fn_stream_i2c_{nullptr};
    PFN_CH347GPIO_Set    fn_gpio_set_{nullptr};
#endif
};

Ch347I2cAdapter::Ch347I2cAdapter()
    : driver_(std::make_unique<DriverBinding>()) {
}

Ch347I2cAdapter::~Ch347I2cAdapter() {
    Close();
}

bool Ch347I2cAdapter::IsDriverLoaded() const noexcept {
    return driver_->IsValid();
}

Common::Result<void> Ch347I2cAdapter::Open(uint32_t device_index) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (is_open_) {
        return Common::Result<void>::Ok();
    }

    if (!driver_->IsValid()) {
        return Common::Status::Error(Common::StatusCode::DeviceUnavailable,
            "CH347DLL.DLL driver is not installed or available on this system.");
    }

#ifdef _WIN32
    HANDLE h = driver_->fn_open_device_(device_index);
    if (h == INVALID_HANDLE_VALUE || h == nullptr) {
        return Common::Status::Error(Common::StatusCode::DeviceUnavailable,
            "Failed to open CH347 device index " + std::to_string(device_index));
    }

    device_index_ = device_index;
    is_open_ = true;

    ApplySpeedLocked();
    return Common::Result<void>::Ok();
#else
    return Common::Status::Error(Common::StatusCode::NotImplemented, "CH347 driver only supported on Windows.");
#endif
}

void Ch347I2cAdapter::Close() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!is_open_) return;

#ifdef _WIN32
    if (driver_->IsValid() && driver_->fn_close_device_) {
        driver_->fn_close_device_(device_index_);
    }
#endif
    is_open_ = false;
}

bool Ch347I2cAdapter::IsOpen() const noexcept {
    return is_open_;
}

Common::Result<void> Ch347I2cAdapter::SetSpeed(I2cClockSpeed speed) {
    std::lock_guard<std::mutex> lock(mutex_);
    speed_ = speed;
    ApplySpeedLocked();
    return Common::Result<void>::Ok();
}

// Open() already holds mutex_, so the hardware call lives in a separate
// unlocked helper to avoid re-locking the non-recursive mutex.
void Ch347I2cAdapter::ApplySpeedLocked() {
    if (!is_open_) return;

#ifdef _WIN32
    if (driver_->IsValid() && driver_->fn_i2c_init_) {
        // CH347 iClockFreq: 0=20KHz, 1=100KHz, 2=400KHz, 3=750KHz, 4=1MHz
        ULONG freq = 1;
        switch (speed_) {
            case I2cClockSpeed::Khz20:   freq = 0; break;
            case I2cClockSpeed::Khz100:  freq = 1; break;
            case I2cClockSpeed::Khz400:  freq = 2; break;
            case I2cClockSpeed::Khz750:  freq = 3; break;
            case I2cClockSpeed::Khz1000: freq = 4; break;
        }
        driver_->fn_i2c_init_(device_index_, freq);
    }
#endif
}

Common::Result<void> Ch347I2cAdapter::Write(uint8_t address7Bit, Common::ByteSpan data) {
    auto res = Transaction(address7Bit, data, 0);
    if (res.IsError()) {
        return res.Error();
    }
    return Common::Result<void>::Ok();
}

Common::Result<Common::ByteBuffer> Ch347I2cAdapter::Read(uint8_t address7Bit, size_t length) {
    return Transaction(address7Bit, {}, length);
}

Common::Result<Common::ByteBuffer> Ch347I2cAdapter::Transaction(uint8_t address7Bit,
                                                             Common::ByteSpan writeData,
                                                             size_t readLength) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!is_open_) {
        return Common::Status::Error(Common::StatusCode::DeviceUnavailable, "CH347 adapter is not open.");
    }

#ifdef _WIN32
    if (!driver_->IsValid()) {
        return Common::Status::Error(Common::StatusCode::DeviceUnavailable, "CH347 driver unavailable.");
    }

    // See the comment in Ch341I2cAdapter::Transaction(): a pure read still
    // needs an address byte on the wire, so send the read address byte when
    // there is no register write to precede it.
    Common::ByteBuffer write_buf;
    if (writeData.empty() && readLength > 0) {
        write_buf.reserve(1);
        write_buf.push_back(Ch341Protocol::MakeAddressByte(address7Bit, true));
    } else {
        write_buf.reserve(1 + writeData.size());
        write_buf.push_back(Ch341Protocol::MakeAddressByte(address7Bit, false));
        write_buf.insert(write_buf.end(), writeData.begin(), writeData.end());
    }

    Common::ByteBuffer read_buf(readLength, 0);

    // Hold the bus idle across the STOP/START boundary of a register read so
    // the coprocessor has time to prepare for the read address.
    if (writeData.empty() && readLength > 0) {
        std::this_thread::sleep_for(kSelectToReadGap);
    }

    ULONG write_len = static_cast<ULONG>(write_buf.size());
    ULONG read_len = static_cast<ULONG>(readLength);

    PVOID p_write = write_len > 0 ? write_buf.data() : nullptr;
    PVOID p_read = read_len > 0 ? read_buf.data() : nullptr;

    BOOL ok = driver_->fn_stream_i2c_(device_index_, write_len, p_write, read_len, p_read);
    if (!ok) {
        return Common::Status::Error(Common::StatusCode::HardwareError, "CH347StreamI2C hardware transaction failed.");
    }

    return read_buf;
#else
    return Common::Status::Error(Common::StatusCode::NotImplemented, "CH347 only supported on Windows.");
#endif
}

Common::Result<void> Ch347I2cAdapter::PulseReset(uint8_t pin_index,
                                               std::chrono::milliseconds low_duration,
                                               bool drive_high) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!is_open_) {
        return Common::Status::Error(Common::StatusCode::DeviceUnavailable, "CH347 adapter is not open.");
    }

#ifdef _WIN32
    if (!driver_->IsValid() || !driver_->fn_gpio_set_) {
        return Common::Result<void>::Ok();
    }

    UCHAR pin_mask = static_cast<UCHAR>(1 << (pin_index & 0x07));
    // Set pin LOW
    driver_->fn_gpio_set_(device_index_, pin_mask, pin_mask, 0x00);
    std::this_thread::sleep_for(low_duration);

    // Set pin HIGH or release
    UCHAR out_data = drive_high ? pin_mask : 0x00;
    driver_->fn_gpio_set_(device_index_, pin_mask, pin_mask, out_data);

    return Common::Result<void>::Ok();
#else
    (void)pin_index;
    (void)low_duration;
    (void)drive_high;
    return Common::Result<void>::Ok();
#endif
}

} // namespace CarPlayStation::Core::Hardware

