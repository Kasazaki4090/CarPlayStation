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
using PFN_CH347OpenDevice       = HANDLE(WINAPI*)(ULONG iIndex);
using PFN_CH347CloseDevice      = BOOL(WINAPI*)(ULONG iIndex);
using PFN_CH347SetTimeout       = BOOL(WINAPI*)(ULONG iIndex, ULONG iWriteTimeout, ULONG iReadTimeout);
using PFN_CH347I2C_Set          = BOOL(WINAPI*)(ULONG iIndex, ULONG iMode);
using PFN_CH347I2C_SetStretch   = BOOL(WINAPI*)(ULONG iIndex, BOOL iEnable);
using PFN_CH347StreamI2C        = BOOL(WINAPI*)(ULONG iIndex, ULONG iWriteLength, PVOID iWriteBuffer, ULONG iReadLength, PVOID oReadBuffer);
using PFN_CH347StreamI2C_RetACK = BOOL(WINAPI*)(ULONG iIndex, ULONG iWriteLength, PVOID iWriteBuffer, ULONG iReadLength, PVOID oReadBuffer, PULONG rAckCount);
using PFN_CH347GPIO_Set         = BOOL(WINAPI*)(ULONG iIndex, UCHAR iEnable, UCHAR iSetDirOut, UCHAR iSetDataOut);
#endif

// Bus idle gap held before a pure read (matching xcertplay / MFi coprocessor requirement).
static constexpr std::chrono::milliseconds kSelectToReadGap{5};

static std::string ToHex(const uint8_t* data, size_t length) {
    std::string out;
    out.reserve(length * 3);
    for (size_t i = 0; i < length; ++i) {
        char buf[4];
        std::snprintf(buf, sizeof(buf), "%02X ", data[i]);
        out += buf;
    }
    if (!out.empty() && out.back() == ' ') {
        out.pop_back();
    }
    return out;
}

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
            fn_open_device_         = reinterpret_cast<PFN_CH347OpenDevice>(GetProcAddress(h_dll_, "CH347OpenDevice"));
            fn_close_device_        = reinterpret_cast<PFN_CH347CloseDevice>(GetProcAddress(h_dll_, "CH347CloseDevice"));
            fn_set_timeout_         = reinterpret_cast<PFN_CH347SetTimeout>(GetProcAddress(h_dll_, "CH347SetTimeout"));
            fn_i2c_set_             = reinterpret_cast<PFN_CH347I2C_Set>(GetProcAddress(h_dll_, "CH347I2C_Set"));
            fn_i2c_set_stretch_     = reinterpret_cast<PFN_CH347I2C_SetStretch>(GetProcAddress(h_dll_, "CH347I2C_SetStretch"));
            fn_stream_i2c_          = reinterpret_cast<PFN_CH347StreamI2C>(GetProcAddress(h_dll_, "CH347StreamI2C"));
            fn_stream_i2c_ret_ack_  = reinterpret_cast<PFN_CH347StreamI2C_RetACK>(GetProcAddress(h_dll_, "CH347StreamI2C_RetACK"));
            fn_gpio_set_            = reinterpret_cast<PFN_CH347GPIO_Set>(GetProcAddress(h_dll_, "CH347GPIO_Set"));
            if (fn_open_device_ && fn_close_device_ && (fn_stream_i2c_ || fn_stream_i2c_ret_ack_)) {
                loaded_path_ = candidate;
                break;
            }
            FreeLibrary(h_dll_);
            h_dll_ = nullptr;
        }
        if (IsValid()) {
            CPS_LOG_INFO("Ch347I2c", "CH347 driver loaded from: ", loaded_path_,
                         " (has_ret_ack=", (fn_stream_i2c_ret_ack_ != nullptr), ")");
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
        return h_dll_ && fn_open_device_ && fn_close_device_ && (fn_stream_i2c_ || fn_stream_i2c_ret_ack_);
#else
        return false;
#endif
    }

#ifdef _WIN32
    HMODULE h_dll_{nullptr};
    std::string loaded_path_;
    PFN_CH347OpenDevice       fn_open_device_{nullptr};
    PFN_CH347CloseDevice      fn_close_device_{nullptr};
    PFN_CH347SetTimeout       fn_set_timeout_{nullptr};
    PFN_CH347I2C_Set          fn_i2c_set_{nullptr};
    PFN_CH347I2C_SetStretch   fn_i2c_set_stretch_{nullptr};
    PFN_CH347StreamI2C        fn_stream_i2c_{nullptr};
    PFN_CH347StreamI2C_RetACK fn_stream_i2c_ret_ack_{nullptr};
    PFN_CH347GPIO_Set         fn_gpio_set_{nullptr};
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

    if (driver_->fn_set_timeout_) {
        driver_->fn_set_timeout_(device_index_, 500, 500);
    }

    if (driver_->fn_i2c_set_stretch_) {
        driver_->fn_i2c_set_stretch_(device_index_, TRUE);
    }

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
    if (driver_->IsValid() && driver_->fn_i2c_set_) {
        // CH347 iMode: lower 2 bits: 00=20KHz, 01=100KHz, 10=400KHz, 11=750KHz
        ULONG mode = 1;
        switch (speed_) {
            case I2cClockSpeed::Khz20:   mode = 0; break;
            case I2cClockSpeed::Khz100:  mode = 1; break;
            case I2cClockSpeed::Khz400:  mode = 2; break;
            case I2cClockSpeed::Khz750:  mode = 3; break;
            case I2cClockSpeed::Khz1000: mode = 3; break;
        }
        driver_->fn_i2c_set_(device_index_, mode);
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

    ApplySpeedLocked();

    auto execute_stream = [this](ULONG write_len, PVOID p_write, ULONG read_len, PVOID p_read, ULONG expected_ack_min) -> Common::Result<void> {
        if (driver_->fn_stream_i2c_ret_ack_) {
            ULONG ack_count = 0;
            BOOL ok = driver_->fn_stream_i2c_ret_ack_(device_index_, write_len, p_write, read_len, p_read, &ack_count);
            if (!ok) {
                return Common::Status::Error(Common::StatusCode::HardwareError, "CH347StreamI2C_RetACK call failed.");
            }
            if (expected_ack_min > 0 && ack_count < expected_ack_min) {
                return Common::Status::Error(Common::StatusCode::DeviceUnavailable,
                    "CH347 reported I2C NACK (expected at least " + std::to_string(expected_ack_min) +
                    " ACKs, received " + std::to_string(ack_count) + ")");
            }
            return Common::Result<void>::Ok();
        } else if (driver_->fn_stream_i2c_) {
            BOOL ok = driver_->fn_stream_i2c_(device_index_, write_len, p_write, read_len, p_read);
            if (!ok) {
                return Common::Status::Error(Common::StatusCode::HardwareError, "CH347StreamI2C call failed.");
            }
            return Common::Result<void>::Ok();
        }
        return Common::Status::Error(Common::StatusCode::DeviceUnavailable, "No CH347 stream transfer function available.");
    };

    // Case 1: Pure write (readLength == 0)
    if (!writeData.empty() && readLength == 0) {
        Common::ByteBuffer write_buf;
        write_buf.reserve(1 + writeData.size());
        write_buf.push_back(Ch341Protocol::MakeAddressByte(address7Bit, false));
        write_buf.insert(write_buf.end(), writeData.begin(), writeData.end());

        auto res = execute_stream(static_cast<ULONG>(write_buf.size()), write_buf.data(), 0, nullptr, static_cast<ULONG>(write_buf.size()));
        if (res.IsError()) {
            return res.Error();
        }
        return Common::ByteBuffer{};
    }

    // Case 2: Pure read (writeData is empty, readLength > 0)
    if (writeData.empty() && readLength > 0) {
        std::this_thread::sleep_for(kSelectToReadGap);

        uint8_t addr_r = Ch341Protocol::MakeAddressByte(address7Bit, true);
        Common::ByteBuffer read_buf(readLength, 0);

        auto res = execute_stream(1, &addr_r, static_cast<ULONG>(readLength), read_buf.data(), 1);
        if (res.IsError()) {
            return res.Error();
        }

        size_t shown = (readLength < 16) ? readLength : 16;
        CPS_LOG_DEBUG("Ch347I2c", "addr 0x", ToHex(&address7Bit, 1),
                      " read ", readLength, " bytes: ",
                      ToHex(read_buf.data(), shown));
        return read_buf;
    }

    // Case 3: Combined Write-then-Read (both writeData and readLength > 0)
    // Follow LIVI / xcertplay contract: execute write with STOP, hold bus idle
    // for at least 5 ms, then execute pure read with STOP.
    if (!writeData.empty() && readLength > 0) {
        Common::ByteBuffer write_buf;
        write_buf.reserve(1 + writeData.size());
        write_buf.push_back(Ch341Protocol::MakeAddressByte(address7Bit, false));
        write_buf.insert(write_buf.end(), writeData.begin(), writeData.end());

        auto write_res = execute_stream(static_cast<ULONG>(write_buf.size()), write_buf.data(), 0, nullptr, static_cast<ULONG>(write_buf.size()));
        if (write_res.IsError()) {
            return write_res.Error();
        }

        std::this_thread::sleep_for(kSelectToReadGap);

        uint8_t addr_r = Ch341Protocol::MakeAddressByte(address7Bit, true);
        Common::ByteBuffer read_buf(readLength, 0);

        auto read_res = execute_stream(1, &addr_r, static_cast<ULONG>(readLength), read_buf.data(), 1);
        if (read_res.IsError()) {
            return read_res.Error();
        }

        size_t shown = (readLength < 16) ? readLength : 16;
        CPS_LOG_DEBUG("Ch347I2c", "addr 0x", ToHex(&address7Bit, 1),
                      " read ", readLength, " bytes: ",
                      ToHex(read_buf.data(), shown));
        return read_buf;
    }

    return Common::ByteBuffer{};
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
    if (!driver_ || !driver_->IsValid()) {
        return Common::Status::Error(Common::StatusCode::DeviceUnavailable, "CH347 driver not loaded.");
    }
    if (!driver_->fn_gpio_set_) {
        return Common::Status::Error(Common::StatusCode::NotSupported,
            "Loaded CH347 driver exports no GPIO set function; cannot drive a reset GPIO.");
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

