#include "Core/Hardware/FtdiI2cAdapter.h"
#include "Core/Hardware/DriverDllLookup.h"
#include "Core/Common/Logging.h"
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace CarPlayStation::Core::Hardware {

#ifdef _WIN32
using FT_HANDLE = PVOID;
using FT_STATUS = ULONG;

struct ChannelConfig {
    ULONG ClockRate;
    UCHAR LatencyTimer;
    ULONG Options;
};

// libMPSSE I2C function types
using PFN_I2C_OpenChannel  = FT_STATUS(*)(ULONG index, FT_HANDLE* handle);
using PFN_I2C_CloseChannel = FT_STATUS(*)(FT_HANDLE handle);
using PFN_I2C_InitChannel  = FT_STATUS(*)(FT_HANDLE handle, ChannelConfig* config);
using PFN_I2C_DeviceRead   = FT_STATUS(*)(FT_HANDLE handle, ULONG deviceAddress, ULONG sizeToTransfer, UCHAR* buffer, ULONG* sizeTransfered, ULONG options);
using PFN_I2C_DeviceWrite  = FT_STATUS(*)(FT_HANDLE handle, ULONG deviceAddress, ULONG sizeToTransfer, UCHAR* buffer, ULONG* sizeTransfered, ULONG options);
using PFN_FT_WriteGPIO     = FT_STATUS(*)(FT_HANDLE handle, UCHAR dir, UCHAR value);
using PFN_FT_ReadGPIO      = FT_STATUS(*)(FT_HANDLE handle, UCHAR* value);

// libMPSSE Transfer options
constexpr ULONG I2C_TRANSFER_OPTIONS_START_BIT       = 0x00000001;
constexpr ULONG I2C_TRANSFER_OPTIONS_STOP_BIT        = 0x00000002;
constexpr ULONG I2C_TRANSFER_OPTIONS_BREAK_ON_NACK   = 0x00000004;
constexpr ULONG I2C_TRANSFER_OPTIONS_NACK_LAST_BYTE  = 0x00000008;
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

// libMPSSE ships as libmpsse.dll in the official 1.0.7 Windows package; the
// older FTDI samples named it libMPSSE-I2C.dll. libmpsse.dll itself links
// against ftd2xx.dll, so both are copied next to the executable by the build
// (see Core/CMakeLists.txt) and found through the default search path.
static std::vector<std::string> BuildFtdiDllCandidates() {
    const std::vector<std::string> names = {"libmpsse.dll", "libMPSSE-I2C.dll", "libMPSSE.dll"};
    return BuildDriverDllCandidates(names, {"ftdigp22.inf", "ftd2xx.inf"});
}

class FtdiI2cAdapter::DriverBinding {
public:
    DriverBinding() {
#ifdef _WIN32
        for (const auto& candidate : BuildFtdiDllCandidates()) {
            h_dll_ = LoadLibraryA(candidate.c_str());
            if (!h_dll_) continue;
            fn_open_       = reinterpret_cast<PFN_I2C_OpenChannel>(GetProcAddress(h_dll_, "I2C_OpenChannel"));
            fn_close_      = reinterpret_cast<PFN_I2C_CloseChannel>(GetProcAddress(h_dll_, "I2C_CloseChannel"));
            fn_init_       = reinterpret_cast<PFN_I2C_InitChannel>(GetProcAddress(h_dll_, "I2C_InitChannel"));
            fn_read_       = reinterpret_cast<PFN_I2C_DeviceRead>(GetProcAddress(h_dll_, "I2C_DeviceRead"));
            fn_write_      = reinterpret_cast<PFN_I2C_DeviceWrite>(GetProcAddress(h_dll_, "I2C_DeviceWrite"));
            fn_write_gpio_ = reinterpret_cast<PFN_FT_WriteGPIO>(GetProcAddress(h_dll_, "FT_WriteGPIO"));
            fn_read_gpio_  = reinterpret_cast<PFN_FT_ReadGPIO>(GetProcAddress(h_dll_, "FT_ReadGPIO"));
            if (fn_open_ && fn_close_ && fn_read_ && fn_write_) {
                loaded_path_ = candidate;
                break;
            }
            FreeLibrary(h_dll_);
            h_dll_ = nullptr;
        }
        if (IsValid()) {
            CPS_LOG_INFO("FtdiI2c", "libMPSSE loaded from: ", loaded_path_,
                         " (has_gpio=", (fn_write_gpio_ != nullptr), ")");
        } else {
            CPS_LOG_WARN("FtdiI2c", "libMPSSE not found in any candidate location "
                "(exe dir, System32, DriverStore). Copy Lib/ftdi-d2xx/<arch>/libmpsse.dll "
                "and ftd2xx.dll next to the executable, or install the FTDI D2XX driver.");
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
        return h_dll_ && fn_open_ && fn_close_ && fn_read_ && fn_write_;
#else
        return false;
#endif
    }

#ifdef _WIN32
    HMODULE h_dll_{nullptr};
    std::string loaded_path_;
    PFN_I2C_OpenChannel  fn_open_{nullptr};
    PFN_I2C_CloseChannel fn_close_{nullptr};
    PFN_I2C_InitChannel  fn_init_{nullptr};
    PFN_I2C_DeviceRead   fn_read_{nullptr};
    PFN_I2C_DeviceWrite  fn_write_{nullptr};
    PFN_FT_WriteGPIO     fn_write_gpio_{nullptr};
    PFN_FT_ReadGPIO      fn_read_gpio_{nullptr};
    FT_HANDLE handle_{nullptr};
#endif
};

FtdiI2cAdapter::FtdiI2cAdapter()
    : driver_(std::make_unique<DriverBinding>()) {
}

FtdiI2cAdapter::~FtdiI2cAdapter() {
    Close();
}

bool FtdiI2cAdapter::IsDriverLoaded() const noexcept {
    return driver_->IsValid();
}

Common::Result<void> FtdiI2cAdapter::Open(uint32_t device_index) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (is_open_) return Common::Result<void>::Ok();

    if (!driver_->IsValid()) {
        return Common::Status::Error(Common::StatusCode::DeviceUnavailable,
            "FTDI libMPSSE/D2XX driver not found on this system.");
    }

#ifdef _WIN32
    FT_STATUS st = driver_->fn_open_(device_index, &driver_->handle_);
    if (st != 0 || driver_->handle_ == nullptr) {
        return Common::Status::Error(Common::StatusCode::DeviceUnavailable,
            "Failed to open FTDI MPSSE channel " + std::to_string(device_index));
    }

    device_index_ = device_index;
    is_open_ = true;

    // Initialize channel with Clock stretching enabled (BIT2 = 0x04)
    ChannelConfig cfg{};
    cfg.ClockRate = static_cast<ULONG>(speed_) * 1000;
    cfg.LatencyTimer = 16;
    cfg.Options = 0x00000004; // Enable clock stretching for MFi coprocessor
    if (driver_->fn_init_) {
        driver_->fn_init_(driver_->handle_, &cfg);
    }

    return Common::Result<void>::Ok();
#else
    return Common::Status::Error(Common::StatusCode::NotImplemented, "FTDI driver only supported on Windows.");
#endif
}

void FtdiI2cAdapter::Close() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!is_open_) return;

#ifdef _WIN32
    if (driver_->IsValid() && driver_->handle_) {
        driver_->fn_close_(driver_->handle_);
        driver_->handle_ = nullptr;
    }
#endif
    is_open_ = false;
}

bool FtdiI2cAdapter::IsOpen() const noexcept {
    return is_open_;
}

Common::Result<void> FtdiI2cAdapter::SetSpeed(I2cClockSpeed speed) {
    std::lock_guard<std::mutex> lock(mutex_);
    speed_ = speed;

    if (!is_open_) return Common::Result<void>::Ok();

#ifdef _WIN32
    if (driver_->IsValid() && driver_->handle_ && driver_->fn_init_) {
        ChannelConfig cfg{};
        cfg.ClockRate = static_cast<ULONG>(speed_) * 1000;
        cfg.LatencyTimer = 16;
        cfg.Options = 0x00000004; // Enable clock stretching
        driver_->fn_init_(driver_->handle_, &cfg);
    }
#endif
    return Common::Result<void>::Ok();
}

Common::Result<void> FtdiI2cAdapter::Write(uint8_t address7Bit, Common::ByteSpan data) {
    auto res = Transaction(address7Bit, data, 0);
    if (res.IsError()) return res.Error();
    return Common::Result<void>::Ok();
}

Common::Result<Common::ByteBuffer> FtdiI2cAdapter::Read(uint8_t address7Bit, size_t length) {
    return Transaction(address7Bit, {}, length);
}

Common::Result<Common::ByteBuffer> FtdiI2cAdapter::Transaction(uint8_t address7Bit,
                                                            Common::ByteSpan writeData,
                                                            size_t readLength) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!is_open_) {
        return Common::Status::Error(Common::StatusCode::DeviceUnavailable, "FTDI adapter is not open.");
    }

#ifdef _WIN32
    if (!driver_->IsValid() || !driver_->handle_) {
        return Common::Status::Error(Common::StatusCode::DeviceUnavailable, "FTDI driver unavailable.");
    }

    auto execute_write = [this](uint8_t addr, Common::ByteSpan data) -> Common::Result<void> {
        ULONG transferred = 0;
        ULONG options = I2C_TRANSFER_OPTIONS_START_BIT | I2C_TRANSFER_OPTIONS_STOP_BIT | I2C_TRANSFER_OPTIONS_BREAK_ON_NACK;
        std::vector<UCHAR> w_copy(data.begin(), data.end());
        FT_STATUS st = driver_->fn_write_(driver_->handle_, addr,
                                          static_cast<ULONG>(w_copy.size()),
                                          w_copy.data(), &transferred, options);
        if (st != 0 || transferred != w_copy.size()) {
            return Common::Status::Error(Common::StatusCode::HardwareError,
                "FTDI I2C write failed or NACK received (transferred " + std::to_string(transferred) +
                " of " + std::to_string(w_copy.size()) + ")");
        }
        return Common::Result<void>::Ok();
    };

    auto execute_read = [this](uint8_t addr, size_t length) -> Common::Result<Common::ByteBuffer> {
        Common::ByteBuffer read_buf(length, 0);
        ULONG transferred = 0;
        ULONG options = I2C_TRANSFER_OPTIONS_START_BIT | I2C_TRANSFER_OPTIONS_STOP_BIT | I2C_TRANSFER_OPTIONS_NACK_LAST_BYTE;

        FT_STATUS st = driver_->fn_read_(driver_->handle_, addr,
                                         static_cast<ULONG>(length),
                                         read_buf.data(), &transferred, options);
        if (st != 0 || transferred != length) {
            return Common::Status::Error(Common::StatusCode::HardwareError,
                "FTDI I2C read failed or NACK received (transferred " + std::to_string(transferred) +
                " of " + std::to_string(length) + ")");
        }

        size_t shown = (length < 16) ? length : 16;
        CPS_LOG_DEBUG("FtdiI2c", "addr 0x", ToHex(&addr, 1),
                      " read ", length, " bytes: ",
                      ToHex(read_buf.data(), shown));
        return read_buf;
    };

    // Case 1: Pure write (readLength == 0)
    if (!writeData.empty() && readLength == 0) {
        auto res = execute_write(address7Bit, writeData);
        if (res.IsError()) {
            return res.Error();
        }
        return Common::ByteBuffer{};
    }

    // Case 2: Pure read (writeData is empty, readLength > 0)
    if (writeData.empty() && readLength > 0) {
        std::this_thread::sleep_for(kSelectToReadGap);
        return execute_read(address7Bit, readLength);
    }

    // Case 3: Combined Write-then-Read (both writeData and readLength > 0)
    // Follow LIVI / xcertplay contract: execute write with STOP, hold bus idle
    // for at least 5 ms, then execute pure read with STOP.
    if (!writeData.empty() && readLength > 0) {
        auto write_res = execute_write(address7Bit, writeData);
        if (write_res.IsError()) {
            return write_res.Error();
        }

        std::this_thread::sleep_for(kSelectToReadGap);

        return execute_read(address7Bit, readLength);
    }

    return Common::ByteBuffer{};
#else
    return Common::Status::Error(Common::StatusCode::NotImplemented, "FTDI only supported on Windows.");
#endif
}

Common::Result<void> FtdiI2cAdapter::PulseReset(uint8_t pin_index,
                                               std::chrono::milliseconds low_duration,
                                               bool drive_high) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!is_open_) {
        return Common::Status::Error(Common::StatusCode::DeviceUnavailable, "FTDI adapter is not open.");
    }

#ifdef _WIN32
    if (!driver_ || !driver_->IsValid() || !driver_->handle_) {
        return Common::Status::Error(Common::StatusCode::DeviceUnavailable, "FTDI driver not loaded.");
    }
    if (!driver_->fn_write_gpio_) {
        return Common::Status::Error(Common::StatusCode::NotSupported,
            "Loaded libMPSSE exports no FT_WriteGPIO function; cannot drive reset GPIO.");
    }

    // GPIO lines on FTDI MPSSE (FT_WriteGPIO controls 8 lines on high byte of channel):
    // dir: 1 for out, 0 for in
    // value: 1 for high, 0 for low
    UCHAR pin_mask = static_cast<UCHAR>(1 << (pin_index & 0x07));

    // Drive LOW
    driver_->fn_write_gpio_(driver_->handle_, pin_mask, 0x00);
    std::this_thread::sleep_for(low_duration);

    // Drive HIGH or release to input
    if (drive_high) {
        driver_->fn_write_gpio_(driver_->handle_, pin_mask, pin_mask);
    } else {
        driver_->fn_write_gpio_(driver_->handle_, 0x00, 0x00);
    }

    return Common::Result<void>::Ok();
#else
    (void)pin_index;
    (void)low_duration;
    (void)drive_high;
    return Common::Result<void>::Ok();
#endif
}

} // namespace CarPlayStation::Core::Hardware


