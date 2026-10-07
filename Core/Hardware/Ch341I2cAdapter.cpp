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
// Function pointer types for CH341DLL.DLL / CH341DLLA64.DLL
using PFN_CH341OpenDevice   = HANDLE(WINAPI*)(ULONG iIndex);
using PFN_CH341CloseDevice  = VOID(WINAPI*)(ULONG iIndex);
using PFN_CH341SetStream    = BOOL(WINAPI*)(ULONG iIndex, ULONG iMode);
using PFN_CH341SetTimeout   = BOOL(WINAPI*)(ULONG iIndex, ULONG iWriteTimeout, ULONG iReadTimeout);
using PFN_CH341WriteData    = BOOL(WINAPI*)(ULONG iIndex, PVOID iBuffer, PULONG ioLength);
using PFN_CH341ReadData     = BOOL(WINAPI*)(ULONG iIndex, PVOID oBuffer, PULONG ioLength);
using PFN_CH341WriteRead    = BOOL(WINAPI*)(ULONG iIndex, ULONG iWriteLength, PVOID iWriteBuffer, ULONG iReadStep, ULONG iReadTimes, PULONG oReadLength, PVOID oReadBuffer);
using PFN_CH341StreamI2C    = BOOL(WINAPI*)(ULONG iIndex, ULONG iWriteLength, PVOID iWriteBuffer, ULONG iReadLength, PVOID oReadBuffer);
using PFN_CH341SetOutput    = BOOL(WINAPI*)(ULONG iIndex, ULONG iEnable, ULONG iSetDirOut, ULONG iSetDataOut);
using PFN_CH341GetInput     = BOOL(WINAPI*)(ULONG iIndex, PULONG iStatus);
#endif

// Bus idle gap held across the STOP/START boundary of a register read (xcertplay 5 ms gap).
static constexpr std::chrono::milliseconds kSelectToReadGap{5};

// Render a few bytes as hex so a silent bus can be told apart from a bus that
// answers with garbage.
static std::string ToHex(const void* data, size_t length) {
    static const char kDigits[] = "0123456789ABCDEF";
    const uint8_t* p = static_cast<const uint8_t*>(data);
    std::string out;
    out.reserve(length * 3);
    for (size_t i = 0; i < length; ++i) {
        if (i) out.push_back(' ');
        out.push_back(kDigits[p[i] >> 4]);
        out.push_back(kDigits[p[i] & 0x0F]);
    }
    return out;
}

// Build the ordered list of candidate CH341 DLL locations to try.
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
                fn_set_timeout_  = reinterpret_cast<PFN_CH341SetTimeout>(GetProcAddress(h_dll_, "CH341SetTimeout"));
                fn_write_data_   = reinterpret_cast<PFN_CH341WriteData>(GetProcAddress(h_dll_, "CH341WriteData"));
                fn_read_data_    = reinterpret_cast<PFN_CH341ReadData>(GetProcAddress(h_dll_, "CH341ReadData"));
                fn_write_read_   = reinterpret_cast<PFN_CH341WriteRead>(GetProcAddress(h_dll_, "CH341WriteRead"));
                fn_stream_i2c_   = reinterpret_cast<PFN_CH341StreamI2C>(GetProcAddress(h_dll_, "CH341StreamI2C"));
                fn_set_output_   = reinterpret_cast<PFN_CH341SetOutput>(GetProcAddress(h_dll_, "CH341SetOutput"));
                fn_get_input_    = reinterpret_cast<PFN_CH341GetInput>(GetProcAddress(h_dll_, "CH341GetInput"));
                if (fn_open_device_ && fn_close_device_ && ((fn_write_read_ && fn_write_data_) || fn_stream_i2c_)) {
                    loaded_path_ = candidate;
                    break;
                }
                FreeLibrary(h_dll_);
                h_dll_ = nullptr;
            }
        }
        if (IsValid()) {
            CPS_LOG_INFO("Ch341I2c", "CH341 driver loaded from: ", loaded_path_,
                         " (has_raw_stream=", (HasRawStream() ? "true" : "false"), ")");
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
        return h_dll_ && fn_open_device_ && fn_close_device_ && ((fn_write_read_ && fn_write_data_) || fn_stream_i2c_);
#else
        return false;
#endif
    }

    [[nodiscard]] bool HasRawStream() const noexcept {
#ifdef _WIN32
        return h_dll_ && fn_write_read_ && fn_write_data_;
#else
        return false;
#endif
    }

    [[nodiscard]] const std::string& LoadedPath() const noexcept {
        return loaded_path_;
    }

#ifdef _WIN32
    HMODULE h_dll_{nullptr};
    PFN_CH341OpenDevice   fn_open_device_{nullptr};
    PFN_CH341CloseDevice  fn_close_device_{nullptr};
    PFN_CH341SetStream    fn_set_stream_{nullptr};
    PFN_CH341SetTimeout   fn_set_timeout_{nullptr};
    PFN_CH341WriteData    fn_write_data_{nullptr};
    PFN_CH341ReadData     fn_read_data_{nullptr};
    PFN_CH341WriteRead    fn_write_read_{nullptr};
    PFN_CH341StreamI2C    fn_stream_i2c_{nullptr};
    PFN_CH341SetOutput    fn_set_output_{nullptr};
    PFN_CH341GetInput     fn_get_input_{nullptr};
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

    if (driver_->fn_set_timeout_) {
        driver_->fn_set_timeout_(device_index_, 500, 500);
    }

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

Common::Result<Common::ByteBuffer> Ch341I2cAdapter::ExecuteStreamLocked(
    uint8_t address7Bit,
    Common::ByteSpan writeData,
    size_t readLength) {
#ifdef _WIN32
    if (!driver_->IsValid()) {
        return Common::Status::Error(Common::StatusCode::DeviceUnavailable, "CH341 driver unavailable.");
    }

    if (driver_->HasRawStream()) {
        // Step 1: Send speed configuration stream packet [0xAA, speed_cmd, 0x00]
        uint8_t speed_cmd = Ch341Protocol::SPEED_100KHZ;
        switch (speed_) {
            case I2cClockSpeed::Khz20:  speed_cmd = Ch341Protocol::SPEED_20KHZ; break;
            case I2cClockSpeed::Khz100: speed_cmd = Ch341Protocol::SPEED_100KHZ; break;
            case I2cClockSpeed::Khz400: speed_cmd = Ch341Protocol::SPEED_400KHZ; break;
            case I2cClockSpeed::Khz750:
            case I2cClockSpeed::Khz1000: speed_cmd = Ch341Protocol::SPEED_750KHZ; break;
        }
        auto cfg = Ch341Protocol::EncodeSpeed(speed_cmd);
        ULONG cfg_len = static_cast<ULONG>(cfg.size());
        driver_->fn_write_data_(device_index_, cfg.data(), &cfg_len);

        // Step 2: Encode transaction stream
        auto stream = Ch341Protocol::EncodeTransaction(address7Bit, writeData, readLength);
        if (stream.empty()) {
            return Common::Status::Error(Common::StatusCode::InvalidArgument, "Failed to encode CH341 stream.");
        }

        // Step 3: Process each 32-byte segment in the stream
        Common::ByteBuffer response;
        response.reserve(readLength);

        size_t offset = 0;
        while (offset < stream.size()) {
            size_t end = std::min(offset + Ch341Protocol::MAX_STREAM_PACKET_BYTES, stream.size());
            Common::ByteSpan segment(stream.data() + offset, end - offset);
            auto lengths = Ch341Protocol::GetInstructionLengths(segment);

            if (lengths.written_bytes + lengths.data_bytes > 0) {
                uint8_t rx_buf[64] = {0};
                ULONG rx_len = 0;
                BOOL ok = driver_->fn_write_read_(
                    device_index_,
                    static_cast<ULONG>(segment.size()),
                    const_cast<uint8_t*>(segment.data()),
                    32,
                    1,
                    &rx_len,
                    rx_buf
                );
                if (!ok || rx_len < lengths.data_bytes) {
                    return Common::Status::Error(Common::StatusCode::DeviceUnavailable,
                        "CH341 stream packet answered " + std::to_string(rx_len) +
                        " bytes; expected " + std::to_string(lengths.data_bytes) + " data bytes");
                }

                // Check status bytes preceding data bytes
                size_t data_start = (rx_len >= lengths.data_bytes) ? (rx_len - lengths.data_bytes) : 0;
                size_t status_count = std::min(data_start, static_cast<size_t>(rx_len));
                for (size_t i = 0; i < status_count; ++i) {
                    if (rx_buf[i] & 0x80) {
                        return Common::Status::Error(Common::StatusCode::DeviceUnavailable,
                            "CH341 reported I2C NACK on byte " + std::to_string(i) +
                            " (status 0x" + ToHex(&rx_buf[i], 1) + ")");
                    }
                }

                if (lengths.data_bytes > 0) {
                    response.insert(response.end(), rx_buf + data_start, rx_buf + data_start + lengths.data_bytes);
                }
            } else {
                // Command carrying only batched writes or intermediate packet
                ULONG tx_len = static_cast<ULONG>(segment.size());
                BOOL ok = driver_->fn_write_data_(device_index_, const_cast<uint8_t*>(segment.data()), &tx_len);
                if (!ok) {
                    return Common::Status::Error(Common::StatusCode::HardwareError, "CH341 WriteData failed.");
                }
            }
            offset = end;
        }

        if (response.size() != readLength) {
            return Common::Status::Error(Common::StatusCode::ProtocolError,
                "CH341 transaction returned " + std::to_string(response.size()) +
                " bytes; expected " + std::to_string(readLength));
        }

        size_t shown = (readLength < 16) ? readLength : 16;
        if (readLength > 0) {
            CPS_LOG_DEBUG("Ch341I2c", "addr 0x", ToHex(&address7Bit, 1),
                          " read ", readLength, " bytes: ",
                          ToHex(response.data(), shown));
        }
        return response;
    }

    // Fallback if raw stream functions are missing: use CH341StreamI2C
    if (driver_->fn_stream_i2c_) {
        if (!writeData.empty() && readLength == 0) {
            Common::ByteBuffer write_buf;
            write_buf.reserve(1 + writeData.size());
            write_buf.push_back(Ch341Protocol::MakeAddressByte(address7Bit, false));
            write_buf.insert(write_buf.end(), writeData.begin(), writeData.end());

            ULONG write_len = static_cast<ULONG>(write_buf.size());
            BOOL ok = driver_->fn_stream_i2c_(device_index_, write_len, write_buf.data(), 0, nullptr);
            if (!ok) {
                return Common::Status::Error(Common::StatusCode::HardwareError, "CH341StreamI2C write failed.");
            }
            return Common::ByteBuffer{};
        }
        if (writeData.empty() && readLength > 0) {
            uint8_t addr_r = Ch341Protocol::MakeAddressByte(address7Bit, true);
            Common::ByteBuffer read_buf(readLength, 0);
            ULONG write_len = 1;
            ULONG read_len = static_cast<ULONG>(readLength);
            BOOL ok = driver_->fn_stream_i2c_(device_index_, write_len, &addr_r, read_len, read_buf.data());
            if (!ok) {
                return Common::Status::Error(Common::StatusCode::HardwareError, "CH341StreamI2C read failed.");
            }
            return read_buf;
        }
    }

    return Common::Status::Error(Common::StatusCode::DeviceUnavailable, "CH341 driver missing required transfer functions.");
#else
    return Common::Status::Error(Common::StatusCode::NotImplemented, "CH341 only supported on Windows.");
#endif
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

    ApplySpeedLocked();

    // Case 1: Pure write (readLength == 0)
    if (!writeData.empty() && readLength == 0) {
        return ExecuteStreamLocked(address7Bit, writeData, 0);
    }

    // Case 2: Pure read (writeData is empty, readLength > 0)
    if (writeData.empty() && readLength > 0) {
        // Hold the bus idle across the STOP/START boundary (xcertplay 5 ms gap)
        std::this_thread::sleep_for(kSelectToReadGap);
        return ExecuteStreamLocked(address7Bit, {}, readLength);
    }

    // Case 3: Combined Write-then-Read (both writeData and readLength > 0)
    // Follow LIVI / xcertplay contract: execute write with STOP, hold bus idle
    // for at least 5 ms, then execute pure read with STOP.
    if (!writeData.empty() && readLength > 0) {
        auto write_res = ExecuteStreamLocked(address7Bit, writeData, 0);
        if (write_res.IsError()) {
            return write_res.Error();
        }

        std::this_thread::sleep_for(kSelectToReadGap);

        return ExecuteStreamLocked(address7Bit, {}, readLength);
    }

    return Common::ByteBuffer{};
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

    if (driver_->fn_write_data_) {
        auto low_pkt = Ch341Protocol::EncodeResetLow(pin_index);
        ULONG low_len = static_cast<ULONG>(low_pkt.size());
        driver_->fn_write_data_(device_index_, low_pkt.data(), &low_len);
        std::this_thread::sleep_for(low_duration);

        auto high_pkt = Ch341Protocol::EncodeResetRelease(pin_index, drive_high);
        ULONG high_len = static_cast<ULONG>(high_pkt.size());
        driver_->fn_write_data_(device_index_, high_pkt.data(), &high_len);
        std::this_thread::sleep_for(std::chrono::milliseconds(11));
        return Common::Result<void>::Ok();
    }

    if (driver_->fn_set_output_) {
        ULONG pin_mask = 1UL << (pin_index & 0x07);
        driver_->fn_set_output_(device_index_, 0x01, pin_mask, 0x00);
        std::this_thread::sleep_for(low_duration);

        ULONG out_data = drive_high ? pin_mask : 0x00;
        driver_->fn_set_output_(device_index_, 0x01, pin_mask, out_data);
        std::this_thread::sleep_for(std::chrono::milliseconds(11));
        return Common::Result<void>::Ok();
    }

    return Common::Status::Error(Common::StatusCode::NotSupported,
        "Loaded CH341 driver exports neither CH341WriteData nor CH341SetOutput.");
#else
    (void)pin_index;
    (void)low_duration;
    (void)drive_high;
    return Common::Result<void>::Ok();
#endif
}

} // namespace CarPlayStation::Core::Hardware


