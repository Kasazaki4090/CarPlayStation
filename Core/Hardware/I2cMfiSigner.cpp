#include "Core/Hardware/I2cMfiSigner.h"
#include "Core/Common/Logging.h"
#include <thread>
#include <vector>
#include <algorithm>

namespace CarPlayStation::Core::Hardware {

namespace {
constexpr uint8_t REG_PROTOCOL_MAJOR = 0x02;
constexpr uint8_t REG_ERROR_CODE     = 0x05;
constexpr uint8_t REG_AUTH_STATUS    = 0x10;
constexpr uint8_t REG_SIG_LEN        = 0x11;
constexpr uint8_t REG_SIG_DATA       = 0x12;
constexpr uint8_t REG_CHALLENGE_LEN  = 0x20;
constexpr uint8_t REG_CHALLENGE_DATA = 0x21;
constexpr uint8_t REG_CERT_LEN       = 0x30;
constexpr uint8_t REG_CERT_DATA      = 0x31;

constexpr uint8_t AUTH_CMD_START     = 0x01;
constexpr uint8_t AUTH_STATUS_OK     = 0x10;

// The coprocessor exposes its certificate and signature through sequential
// 128-byte register windows: register 0x31 + w is the window starting at
// offset w * 128 of the blob. A single over-long read past the first window
// returns garbage, so blobs are read window by window.
constexpr size_t kRegisterWindowBytes = 128;
} // namespace

I2cMfiSigner::I2cMfiSigner(std::shared_ptr<II2cAdapter> adapter,
                           uint8_t slave_address7Bit,
                           Options options)
    : adapter_(std::move(adapter))
    , slave_address_(slave_address7Bit)
    , options_(options) {
    // NOTE: The I2C adapter is opened lazily on first use (see EnsureOpen),
    // NOT here. Opening in the constructor would call the vendor driver's
    // CH341OpenDevice(), which blocks when the physical USB bridge is not
    // plugged in. That would hang the entire application startup (the MFi
    // signer is built during StartCarPlay, before the Bluetooth transport and
    // the UI window are brought up).
}

void I2cMfiSigner::EnsureOpen() const {
    // Caller must hold mutex_.
    if (!adapter_ || adapter_->IsOpen() || is_shutdown_) {
        return;
    }
    auto res = adapter_->Open(0);
    if (res.IsError()) {
        CPS_LOG_WARN("I2cMfiSigner", "Failed to open I2C adapter: ", res.Error().Message());
    }
}

I2cMfiSigner::~I2cMfiSigner() {
    Shutdown();
}

void I2cMfiSigner::Shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (is_shutdown_) return;
    is_shutdown_ = true;
    if (adapter_) {
        adapter_->Close();
    }
}

Common::Result<uint8_t> I2cMfiSigner::ReadByte(uint8_t reg) {
    auto res = ReadRegister(reg, 1);
    if (res.IsError()) return res.Error();
    if (res.Value().empty()) {
        return Common::Status::Error(Common::StatusCode::HardwareError, "Empty response from register read");
    }
    return res.Value()[0];
}

Common::Result<uint16_t> I2cMfiSigner::ReadBigEndianShort(uint8_t reg) {
    auto res = ReadRegister(reg, 2);
    if (res.IsError()) return res.Error();
    const auto& bytes = res.Value();
    if (bytes.size() < 2) {
        return Common::Status::Error(Common::StatusCode::HardwareError, "Short read from 16-bit register");
    }
    uint16_t value = static_cast<uint16_t>((static_cast<uint16_t>(bytes[0]) << 8) | bytes[1]);
    return value;
}

Common::Result<void> I2cMfiSigner::WriteBigEndianShort(uint8_t reg, uint16_t value) {
    uint8_t data[2] = {
        static_cast<uint8_t>((value >> 8) & 0xFF),
        static_cast<uint8_t>(value & 0xFF)
    };
    return WriteRegister(reg, Common::ByteSpan(data, 2));
}

Common::Result<void> I2cMfiSigner::SelectRegister(uint8_t reg) {
    if (!adapter_ || !adapter_->IsOpen()) {
        return Common::Status::Error(Common::StatusCode::DeviceUnavailable, "I2C adapter is not open");
    }

    uint8_t reg_addr = reg;
    return adapter_->Write(slave_address_, Common::ByteSpan(&reg_addr, 1));
}

Common::Result<Common::ByteBuffer> I2cMfiSigner::ReadRegister(uint8_t reg, size_t length) {
    if (!adapter_ || !adapter_->IsOpen()) {
        return Common::Status::Error(Common::StatusCode::DeviceUnavailable, "I2C adapter is not open");
    }

    auto deadline = std::chrono::steady_clock::now() + options_.io_retry_timeout;
    Common::Result<Common::ByteBuffer> last_res = Common::Status::Error(Common::StatusCode::Timeout, "I2C read timed out");

    while (std::chrono::steady_clock::now() < deadline) {
        // The coprocessor can be asleep when the register-select write arrives,
        // and the USB bridges do not surface that I2C NACK separately. Retry the
        // complete select/read pair: retrying only the read half can otherwise
        // keep reading whatever register was selected before.
        auto sel = SelectRegister(reg);
        if (sel.IsError()) {
            last_res = sel.Error();
            std::this_thread::sleep_for(options_.io_retry_delay);
            continue;
        }

        auto res = adapter_->Read(slave_address_, length);
        if (res.IsOk() && res.Value().size() == length) {
            return res;
        }
        last_res = res;
        std::this_thread::sleep_for(options_.io_retry_delay);
    }

    return last_res;
}

// Reads a blob exposed as sequential register windows (certificate 0x31,
// signature 0x12). Each window is 128 bytes and starts at base_reg + w.
Common::Result<Common::ByteBuffer> I2cMfiSigner::ReadRegisterWindows(uint8_t base_reg, size_t length) {
    Common::ByteBuffer blob;
    blob.reserve(length);

    size_t offset = 0;
    uint16_t reg = base_reg;
    while (offset < length) {
        size_t count = std::min(kRegisterWindowBytes, length - offset);
        auto res = ReadRegister(static_cast<uint8_t>(reg), count);
        if (res.IsError()) {
            return res.Error();
        }
        blob.insert(blob.end(), res.Value().begin(), res.Value().end());
        offset += count;
        reg += 1;
    }

    return blob;
}

Common::Result<void> I2cMfiSigner::WriteRegister(uint8_t reg, Common::ByteSpan data) {
    if (!adapter_ || !adapter_->IsOpen()) {
        return Common::Status::Error(Common::StatusCode::DeviceUnavailable, "I2C adapter is not open");
    }

    Common::ByteBuffer payload;
    payload.reserve(1 + data.size());
    payload.push_back(reg);
    payload.insert(payload.end(), data.begin(), data.end());

    auto deadline = std::chrono::steady_clock::now() + options_.io_retry_timeout;
    Common::Result<void> last_res = Common::Status::Error(Common::StatusCode::Timeout, "I2C write timed out");

    while (std::chrono::steady_clock::now() < deadline) {
        auto res = adapter_->Write(slave_address_, payload);
        if (res.IsOk()) {
            return Common::Result<void>::Ok();
        }
        last_res = res;
        std::this_thread::sleep_for(options_.io_retry_delay);
    }

    return last_res;
}

Common::Result<Common::ByteBuffer> I2cMfiSigner::FetchCertificate() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (is_shutdown_) {
        return Common::Status::Error(Common::StatusCode::DeviceUnavailable, "Signer is shut down");
    }
    EnsureOpen();

    // 1. Read certificate length from register 0x30
    auto len_res = ReadBigEndianShort(REG_CERT_LEN);
    if (len_res.IsError()) {
        return len_res.Error();
    }

    uint16_t cert_len = len_res.Value();
    if (cert_len == 0 || cert_len > 65525) {
        return Common::Status::Error(Common::StatusCode::AuthenticationFailed,
            "Invalid MFi certificate length: " + std::to_string(cert_len));
    }

    // The certificate body is exposed as sequential 128-byte register windows
    // starting at 0x31. One long read past the first window returns garbage.
    return ReadRegisterWindows(REG_CERT_DATA, cert_len);
}

Common::Result<Common::ByteBuffer> I2cMfiSigner::SignChallenge(Common::ByteSpan challenge) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (is_shutdown_) {
        return Common::Status::Error(Common::StatusCode::DeviceUnavailable, "Signer is shut down");
    }
    EnsureOpen();

    if (challenge.empty() || challenge.size() > 128) {
        return Common::Status::Error(Common::StatusCode::InvalidArgument, "Invalid challenge length");
    }

    // 1. Write challenge length to 0x20
    auto w_len = WriteBigEndianShort(REG_CHALLENGE_LEN, static_cast<uint16_t>(challenge.size()));
    if (w_len.IsError()) return w_len.Error();

    // 2. Write challenge payload to 0x21
    auto w_data = WriteRegister(REG_CHALLENGE_DATA, challenge);
    if (w_data.IsError()) return w_data.Error();

    // 3. Trigger signature generation by writing 0x01 to 0x10
    uint8_t start_cmd = AUTH_CMD_START;
    auto w_start = WriteRegister(REG_AUTH_STATUS, Common::ByteSpan(&start_cmd, 1));
    if (w_start.IsError()) return w_start.Error();

    // 4. Poll 0x10 until AUTH_STATUS_OK
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    auto deadline = std::chrono::steady_clock::now() + options_.auth_timeout;
    bool completed = false;

    while (std::chrono::steady_clock::now() < deadline) {
        auto status_res = ReadByte(REG_AUTH_STATUS);
        if (status_res.IsOk() && status_res.Value() == AUTH_STATUS_OK) {
            completed = true;
            break;
        }
        std::this_thread::sleep_for(options_.auth_poll_interval);
    }

    if (!completed) {
        auto err_byte = ReadByte(REG_ERROR_CODE);
        uint8_t code = err_byte.IsOk() ? err_byte.Value() : 0xFF;
        return Common::Status::Error(Common::StatusCode::AuthenticationFailed,
            "MFi chip failed to compute signature. Chip error code: 0x" + std::to_string(code));
    }

    // 5. Read response signature length from 0x11
    auto sig_len_res = ReadBigEndianShort(REG_SIG_LEN);
    if (sig_len_res.IsError()) return sig_len_res.Error();

    uint16_t sig_len = sig_len_res.Value();
    if (sig_len == 0 || sig_len > 512) {
        return Common::Status::Error(Common::StatusCode::AuthenticationFailed,
            "Invalid MFi signature response length: " + std::to_string(sig_len));
    }

    // 6. Read signature payload from 0x12
    return ReadRegister(REG_SIG_DATA, sig_len);
}

MFiChipInfo I2cMfiSigner::GetChipInfo() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    MFiChipInfo info{};
    info.present = false;

    if (!adapter_ || is_shutdown_) {
        return info;
    }
    EnsureOpen();
    if (!adapter_->IsOpen()) {
        return info;
    }

    // Read major protocol version from 0x02
    uint8_t reg = REG_PROTOCOL_MAJOR;
    auto res = adapter_->Transaction(slave_address_, Common::ByteSpan(&reg, 1), 1);
    if (res.IsOk() && !res.Value().empty()) {
        uint8_t major = res.Value()[0];
        info.present = true;
        info.chip_id = 0x00020000 | major;
        info.firmware_version = "MFi-CP-v" + std::to_string(major);
    }

    return info;
}

} // namespace CarPlayStation::Core::Hardware

