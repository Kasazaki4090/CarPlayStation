#include "Core/Hardware/Ch341I2cAdapter.h"
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
// Function pointer types for CH341DLL.DLL
using PFN_CH341OpenDevice   = HANDLE(WINAPI*)(ULONG iIndex);
using PFN_CH341CloseDevice  = VOID(WINAPI*)(ULONG iIndex);
using PFN_CH341SetStream    = BOOL(WINAPI*)(ULONG iIndex, ULONG iMode);
using PFN_CH341StreamI2C    = BOOL(WINAPI*)(ULONG iIndex, ULONG iWriteLength, PVOID iWriteBuffer, ULONG iReadLength, PVOID oReadBuffer);
using PFN_CH341SetOutput    = BOOL(WINAPI*)(ULONG iIndex, ULONG iEnable, ULONG iSetDirOut, ULONG iSetDataOut);
#endif

// Bus idle gap held before a pure read. See the comment in Transaction().
static constexpr std::chrono::milliseconds kSelectToReadGap{5};

// Build the ordered list of candidate CH341 DLL locations to try.
// The WCH driver installer registers the device but leaves the DLL in
// DriverStore\FileRepository\ch341wdm.inf_*, which is NOT in the default
// DLL search path, so a bare LoadLibraryA("CH341DLL.DLL") fails. On x64 we
// prefer the A64 build (CH341DLLA64.DLL) since a 32-bit DLL cannot be loaded
// into a 64-bit process.
static std::vector<std::string> BuildCh341DllCandidates() {
#ifdef _WIN64
    const std::vector<std::string> names = {"CH341DLLA64.DLL", "CH341DLL.DLL"};
#else
    const std::vector<std::string> names = {"CH341DLL.DLL", "CH341DLLA64.DLL"};
#endif
    return BuildDriverDllCandidates(names, {"ch341wdm.inf"});
}

class Ch341I2cAdapter::DriverBinding {
public:
    DriverBinding() {
#ifdef _WIN32
        for (const auto& candidate : BuildCh341DllCandidates()) {
            h_dll_ = LoadLibraryA(candidate.c_str());
            if (h_dll_) {
                fn_open_device_  = reinterpret_cast<PFN_CH341OpenDevice>(GetProcAddress(h_dll_, "CH341OpenDevice"));
                fn_close_device_ = reinterpret_cast<PFN_CH341CloseDevice>(GetProcAddress(h_dll_, "CH341CloseDevice"));
                fn_set_stream_   = reinterpret_cast<PFN_CH341SetStream>(GetProcAddress(h_dll_, "CH341SetStream"));
                fn_stream_i2c_   = reinterpret_cast<PFN_CH341StreamI2C>(GetProcAddress(h_dll_, "CH341StreamI2C"));
                fn_set_output_   = reinterpret_cast<PFN_CH341SetOutput>(GetProcAddress(h_dll_, "CH341SetOutput"));
                if (fn_open_device_ && fn_close_device_ && fn_stream_i2c_) {
                    loaded_path_ = candidate;
                    break;
                }
                FreeLibrary(h_dll_);
                h_dll_ = nullptr;
            }
        }
        if (IsValid()) {
            CPS_LOG_INFO("Ch341I2c", "CH341 driver loaded from: ", loaded_path_);
        } else {
            CPS_LOG_WARN("Ch341I2c", "CH341 driver DLL not found in any candidate location "
                "(exe dir, System32, DriverStore ch341wdm.inf_*). Install the WCH CH341 driver "
                "or place CH341DLLA64.DLL next to the executable.");
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

    [[nodiscard]] const std::string& LoadedPath() const noexcept {
        return loaded_path_;
    }

#ifdef _WIN32
    HMODULE h_dll_{nullptr};
    PFN_CH341OpenDevice  fn_open_device_{nullptr};
    PFN_CH341CloseDevice  fn_close_device_{nullptr};
    PFN_CH341SetStream    fn_set_stream_{nullptr};
    PFN_CH341StreamI2C    fn_stream_i2c_{nullptr};
    PFN_CH341SetOutput    fn_set_output_{nullptr};
    std::string loaded_path_;
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

// Apply the I2C clock mode to an already-open device WITHOUT taking mutex_
// (the caller must already hold it). This avoids the reentrant self-deadlock
// that occurred when Open() called SetSpeed() while holding the same
// non-recursive mutex_.
void Ch341I2cAdapter::ApplySpeedLocked() {
#ifdef _WIN32
    if (!driver_->IsValid() || !driver_->fn_set_stream_) {
        return;
    }
    // CH341 mode bits for I2C clock:
    // bit 1-0: 00=20KHz, 01=100KHz, 10=400KHz, 11=750KHz
    ULONG mode = 0x01; // default 100KHz
    switch (speed_) {
        case I2cClockSpeed::Khz20:  mode = 0x00; break;
        case I2cClockSpeed::Khz100: mode = 0x01; break;
        case I2cClockSpeed::Khz400: mode = 0x02; break;
        case I2cClockSpeed::Khz750:
        case I2cClockSpeed::Khz1000: mode = 0x03; break;
    }
    driver_->fn_set_stream_(device_index_, mode);
#endif
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

    // Apply default speed WITHOUT re-locking mutex_ (would self-deadlock).
    ApplySpeedLocked();
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

    // Apply the new clock mode (caller already holds mutex_).
    ApplySpeedLocked();
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

    // Prepare write payload (Address + writeData).
    //
    // A pure read still needs an address byte on the wire: CH341StreamI2C
    // emits START + the bytes we hand it, so passing no write data at all
    // produces a START with no address and the slave never answers (the
    // driver returns 0xFF padding). The reference driver sends the read
    // address byte, (addr << 1) | 1, as a one byte write for that case.
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

    // The 2.0C revision of the CH341A NACKs the read address when a START
    // follows a STOP sooner than about 3 ms, and then answers the read with
    // 0xFF padding. Hold the bus idle across the STOP/START boundary of a
    // register read (same 5 ms gap the reference transports use).
    if (writeData.empty() && readLength > 0) {
        std::this_thread::sleep_for(kSelectToReadGap);
    }

    // Call vendor driver StreamI2C
    ULONG write_len = static_cast<ULONG>(write_buf.size());
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
    if (!driver_->IsValid()) {
        return Common::Status::Error(Common::StatusCode::DeviceUnavailable, "CH341 driver unavailable.");
    }
    // Reporting success here would let the caller log a reset pulse that never
    // happened, which is exactly the wrong thing to conclude from when the chip
    // is silent.
    if (!driver_->fn_set_output_) {
        return Common::Status::Error(Common::StatusCode::NotSupported,
            "Loaded CH341 driver exports no CH341SetOutput; cannot drive a reset GPIO.");
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

