#include "Core/Hardware/Ch341I2cAdapter.h"
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
// Function pointer types for CH341DLL.DLL
using PFN_CH341OpenDevice   = HANDLE(WINAPI*)(ULONG iIndex);
using PFN_CH341CloseDevice  = VOID(WINAPI*)(ULONG iIndex);
using PFN_CH341SetStream    = BOOL(WINAPI*)(ULONG iIndex, ULONG iMode);
using PFN_CH341StreamI2C    = BOOL(WINAPI*)(ULONG iIndex, ULONG iWriteLength, PVOID iWriteBuffer, ULONG iReadLength, PVOID oReadBuffer);
using PFN_CH341SetOutput    = BOOL(WINAPI*)(ULONG iIndex, ULONG iEnable, ULONG iSetDirOut, ULONG iSetDataOut);
#endif

class Ch341I2cAdapter::DriverBinding {
public:
    DriverBinding() {
#ifdef _WIN32
        h_dll_ = LoadLibraryA("CH341DLL.DLL");
        if (h_dll_) {
            fn_open_device_  = reinterpret_cast<PFN_CH341OpenDevice>(GetProcAddress(h_dll_, "CH341OpenDevice"));
            fn_close_device_ = reinterpret_cast<PFN_CH341CloseDevice>(GetProcAddress(h_dll_, "CH341CloseDevice"));
            fn_set_stream_   = reinterpret_cast<PFN_CH341SetStream>(GetProcAddress(h_dll_, "CH341SetStream"));
            fn_stream_i2c_   = reinterpret_cast<PFN_CH341StreamI2C>(GetProcAddress(h_dll_, "CH341StreamI2C"));
            fn_set_output_   = reinterpret_cast<PFN_CH341SetOutput>(GetProcAddress(h_dll_, "CH341SetOutput"));
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
    PFN_CH341OpenDevice  fn_open_device_{nullptr};
    PFN_CH341CloseDevice fn_close_device_{nullptr};
    PFN_CH341SetStream   fn_set_stream_{nullptr};
    PFN_CH341StreamI2C   fn_stream_i2c_{nullptr};
    PFN_CH341SetOutput   fn_set_output_{nullptr};
#endif
};

Ch341I2cAdapter::Ch341I2cAdapter()
    : driver_(std::make_unique<DriverBinding>()) {
}

Ch341I2cAdapter::~Ch341I2cAdapter() {
    Close();
}

bool Ch341I2cAdapter::IsDriverLoaded() const noexcept {
    return driver_->IsValid();
}

Common::Result<void> Ch341I2cAdapter::Open(uint32_t device_index) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (is_open_) {
        return Common::Result<void>::Ok();
    }

    if (!driver_->IsValid()) {
        return Common::Status::Error(Common::StatusCode::DeviceUnavailable,
            "CH341DLL.DLL driver is not installed or available on this system.");
    }

#ifdef _WIN32
    HANDLE h = driver_->fn_open_device_(device_index);
    if (h == INVALID_HANDLE_VALUE || h == nullptr) {
        return Common::Status::Error(Common::StatusCode::DeviceUnavailable,
            "Failed to open CH341 device index " + std::to_string(device_index));
    }

    device_index_ = device_index;
    is_open_ = true;

    // Apply default speed
    SetSpeed(speed_);
    return Common::Result<void>::Ok();
#else
    return Common::Status::Error(Common::StatusCode::NotImplemented, "CH341 driver only supported on Windows.");
#endif
}

void Ch341I2cAdapter::Close() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!is_open_) return;

#ifdef _WIN32
    if (driver_->IsValid() && driver_->fn_close_device_) {
        driver_->fn_close_device_(device_index_);
    }
#endif
    is_open_ = false;
}

bool Ch341I2cAdapter::IsOpen() const noexcept {
    return is_open_;
}

Common::Result<void> Ch341I2cAdapter::SetSpeed(I2cClockSpeed speed) {
    std::lock_guard<std::mutex> lock(mutex_);
    speed_ = speed;

    if (!is_open_) return Common::Result<void>::Ok();

#ifdef _WIN32
    if (driver_->IsValid() && driver_->fn_set_stream_) {
        // CH341 mode bits for I2C clock:
        // bit 1-0: 00=20KHz, 01=100KHz, 10=400KHz, 11=750KHz
        ULONG mode = 0x01; // default 100KHz
        switch (speed) {
            case I2cClockSpeed::Khz20:  mode = 0x00; break;
            case I2cClockSpeed::Khz100: mode = 0x01; break;
            case I2cClockSpeed::Khz400: mode = 0x02; break;
            case I2cClockSpeed::Khz750:
            case I2cClockSpeed::Khz1000: mode = 0x03; break;
        }
        driver_->fn_set_stream_(device_index_, mode);
    }
#endif
    return Common::Result<void>::Ok();
}

Common::Result<void> Ch341I2cAdapter::Write(uint8_t address7Bit, Common::ByteSpan data) {
    auto res = Transaction(address7Bit, data, 0);
    if (res.IsError()) {
        return res.Error();
    }
    return Common::Result<void>::Ok();
}

Common::Result<Common::ByteBuffer> Ch341I2cAdapter::Read(uint8_t address7Bit, size_t length) {
    return Transaction(address7Bit, {}, length);
}

Common::Result<Common::ByteBuffer> Ch341I2cAdapter::Transaction(uint8_t address7Bit,
                                                             Common::ByteSpan writeData,
                                                             size_t readLength) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!is_open_) {
        return Common::Status::Error(Common::StatusCode::DeviceUnavailable, "CH341 adapter is not open.");
    }

#ifdef _WIN32
    if (!driver_->IsValid()) {
        return Common::Status::Error(Common::StatusCode::DeviceUnavailable, "CH341 driver unavailable.");
    }

    // Prepare write payload (Address + writeData)
    Common::ByteBuffer write_buf;
    write_buf.reserve(1 + writeData.size());
    write_buf.push_back(Ch341Protocol::MakeAddressByte(address7Bit, false));
    write_buf.insert(write_buf.end(), writeData.begin(), writeData.end());

    Common::ByteBuffer read_buf(readLength, 0);

    // Call vendor driver StreamI2C
    ULONG write_len = static_cast<ULONG>(writeData.empty() && readLength > 0 ? 0 : write_buf.size());
    ULONG read_len = static_cast<ULONG>(readLength);

    // If it's a pure read or write+read transaction
    PVOID p_write = write_len > 0 ? write_buf.data() : nullptr;
    PVOID p_read = read_len > 0 ? read_buf.data() : nullptr;

    BOOL ok = driver_->fn_stream_i2c_(device_index_, write_len, p_write, read_len, p_read);
    if (!ok) {
        return Common::Status::Error(Common::StatusCode::HardwareError, "CH341StreamI2C hardware transaction failed.");
    }

    return read_buf;
#else
    return Common::Status::Error(Common::StatusCode::NotImplemented, "CH341 only supported on Windows.");
#endif
}

Common::Result<void> Ch341I2cAdapter::PulseReset(uint8_t pin_index,
                                               std::chrono::milliseconds low_duration,
                                               bool drive_high) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!is_open_) {
        return Common::Status::Error(Common::StatusCode::DeviceUnavailable, "CH341 adapter is not open.");
    }

#ifdef _WIN32
    if (!driver_->IsValid() || !driver_->fn_set_output_) {
        return Common::Result<void>::Ok();
    }

    ULONG pin_mask = 1UL << (pin_index & 0x07);
    // Drive pin LOW
    driver_->fn_set_output_(device_index_, 0x01, pin_mask, 0x00);
    std::this_thread::sleep_for(low_duration);

    // Drive pin HIGH or release
    ULONG out_data = drive_high ? pin_mask : 0x00;
    driver_->fn_set_output_(device_index_, 0x01, pin_mask, out_data);

    return Common::Result<void>::Ok();
#else
    (void)pin_index;
    (void)low_duration;
    (void)drive_high;
    return Common::Result<void>::Ok();
#endif
}

} // namespace CarPlayStation::Core::Hardware

