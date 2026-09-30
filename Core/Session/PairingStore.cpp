#include "Core/Session/PairingStore.h"
#include "Core/Common/Logging.h"
#include <fstream>
#include <sstream>
#include <filesystem>
#include <iomanip>
#include <algorithm>

namespace CarPlayStation::Core::Session {

namespace {

// Converts a hex string without delimiters into a ByteBuffer
[[nodiscard]] Common::ByteBuffer HexToBytes(std::string_view hex) {
    Common::ByteBuffer bytes;
    bytes.reserve(hex.size() / 2);
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        char hi = hex[i];
        char lo = hex[i + 1];
        auto hex_val = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        int h = hex_val(hi);
        int l = hex_val(lo);
        if (h >= 0 && l >= 0) {
            bytes.push_back(static_cast<uint8_t>((h << 4) | l));
        }
    }
    return bytes;
}

// Converts bytes to compact hex string
[[nodiscard]] std::string BytesToCompactHex(Common::ByteSpan bytes) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (uint8_t b : bytes) {
        oss << std::setw(2) << static_cast<int>(b);
    }
    return oss.str();
}

// Parses standard colon-separated or dash-separated MAC address
[[nodiscard]] Common::MacAddress ParseMac(std::string_view str) {
    Common::MacAddress mac{};
    unsigned int b[6] = {0};
    if (sscanf_s(str.data(), "%02x:%02x:%02x:%02x:%02x:%02x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) == 6 ||
        sscanf_s(str.data(), "%02x-%02x-%02x-%02x-%02x-%02x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) == 6) {
        for (int i = 0; i < 6; ++i) {
            mac.bytes[i] = static_cast<uint8_t>(b[i]);
        }
    }
    return mac;
}

// Escapes string for JSON value
[[nodiscard]] std::string JsonEscape(std::string_view str) {
    std::string out;
    out.reserve(str.size() + 8);
    for (char c : str) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += c; break;
        }
    }
    return out;
}

// Helper to extract JSON string field value
[[nodiscard]] std::string ExtractJsonString(std::string_view object, std::string_view key) {
    std::string search = "\"" + std::string(key) + "\"";
    size_t pos = object.find(search);
    if (pos == std::string_view::npos) return {};

    size_t colon = object.find(':', pos + search.size());
    if (colon == std::string_view::npos) return {};

    size_t quote_start = object.find('"', colon + 1);
    if (quote_start == std::string_view::npos) return {};

    size_t quote_end = object.find('"', quote_start + 1);
    if (quote_end == std::string_view::npos) return {};

    return std::string(object.substr(quote_start + 1, quote_end - quote_start - 1));
}

// Helper to extract JSON integer/uint64 field value
[[nodiscard]] uint64_t ExtractJsonU64(std::string_view object, std::string_view key, uint64_t def = 0) {
    std::string search = "\"" + std::string(key) + "\"";
    size_t pos = object.find(search);
    if (pos == std::string_view::npos) return def;

    size_t colon = object.find(':', pos + search.size());
    if (colon == std::string_view::npos) return def;

    size_t num_start = object.find_first_of("0123456789", colon + 1);
    if (num_start == std::string_view::npos) return def;

    size_t num_end = object.find_first_not_of("0123456789", num_start);
    std::string_view num_str = object.substr(num_start, num_end == std::string_view::npos ? std::string_view::npos : num_end - num_start);

    try {
        return std::stoull(std::string(num_str));
    } catch (...) {
        return def;
    }
}

} // namespace

PairingStore::PairingStore(std::string storage_path)
    : storage_path_(std::move(storage_path)) {
}

Common::Result<void> PairingStore::SaveDevice(const PairedDevice& device) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!device.IsValid()) {
        return Common::Status::Error(Common::StatusCode::InvalidArgument, "Cannot save invalid PairedDevice (empty ID)");
    }

    if (!loaded_) {
        (void)LoadFromDiskLocked();
    }

    auto it = std::find_if(cache_.begin(), cache_.end(), [&](const PairedDevice& d) {
        return d.device_id == device.device_id;
    });

    if (it != cache_.end()) {
        *it = device;
    } else {
        cache_.push_back(device);
    }

    return SaveToDiskLocked();
}

Common::Result<PairedDevice> PairingStore::GetDevice(std::string_view device_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!loaded_) {
        auto* mutable_this = const_cast<PairingStore*>(this);
        (void)mutable_this->LoadFromDiskLocked();
    }

    auto it = std::find_if(cache_.begin(), cache_.end(), [&](const PairedDevice& d) {
        return d.device_id == device_id;
    });

    if (it != cache_.end()) {
        return *it;
    }
    return Common::Status::Error(Common::StatusCode::NotFound, "Paired device not found: " + std::string(device_id));
}

Common::Result<PairedDevice> PairingStore::FindByMac(const Common::MacAddress& mac) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!loaded_) {
        auto* mutable_this = const_cast<PairingStore*>(this);
        (void)mutable_this->LoadFromDiskLocked();
    }

    auto it = std::find_if(cache_.begin(), cache_.end(), [&](const PairedDevice& d) {
        return d.bluetooth_mac == mac || d.wifi_mac == mac;
    });

    if (it != cache_.end()) {
        return *it;
    }
    return Common::Status::Error(Common::StatusCode::NotFound, "No device found matching MAC " + mac.ToString());
}

std::vector<PairedDevice> PairingStore::ListDevices() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!loaded_) {
        auto* mutable_this = const_cast<PairingStore*>(this);
        (void)mutable_this->LoadFromDiskLocked();
    }

    std::vector<PairedDevice> sorted = cache_;
    std::sort(sorted.begin(), sorted.end(), [](const PairedDevice& a, const PairedDevice& b) {
        return a.last_connected_timestamp > b.last_connected_timestamp;
    });
    return sorted;
}

Common::Result<void> PairingStore::RemoveDevice(std::string_view device_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!loaded_) {
        (void)LoadFromDiskLocked();
    }

    auto it = std::remove_if(cache_.begin(), cache_.end(), [&](const PairedDevice& d) {
        return d.device_id == device_id;
    });

    if (it != cache_.end()) {
        cache_.erase(it, cache_.end());
        return SaveToDiskLocked();
    }
    return Common::Status::Error(Common::StatusCode::NotFound, "Device not found for removal: " + std::string(device_id));
}

void PairingStore::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    cache_.clear();
    (void)SaveToDiskLocked();
}

size_t PairingStore::Count() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!loaded_) {
        auto* mutable_this = const_cast<PairingStore*>(this);
        (void)mutable_this->LoadFromDiskLocked();
    }
    return cache_.size();
}

bool PairingStore::IsPaired(std::string_view device_id) const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!loaded_) {
        auto* mutable_this = const_cast<PairingStore*>(this);
        (void)mutable_this->LoadFromDiskLocked();
    }
    return std::any_of(cache_.begin(), cache_.end(), [&](const PairedDevice& d) {
        return d.device_id == device_id;
    });
}

Common::Result<void> PairingStore::Reload() {
    std::lock_guard<std::mutex> lock(mutex_);
    return LoadFromDiskLocked();
}

Common::Result<void> PairingStore::SaveToDiskLocked() {
    if (storage_path_.empty()) {
        return Common::Status::Error(Common::StatusCode::InvalidArgument, "PairingStore storage path is empty");
    }

    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path target_path(storage_path_);
    if (target_path.has_parent_path()) {
        fs::create_directories(target_path.parent_path(), ec);
    }

    fs::path temp_path = target_path;
    temp_path += ".tmp";

    std::ofstream out(temp_path, std::ios::trunc | std::ios::out);
    if (!out.is_open()) {
        return Common::Status::Error(Common::StatusCode::IoError, "Failed to open temporary file for writing: " + temp_path.string());
    }

    out << "{\n";
    out << "  \"version\": 1,\n";
    out << "  \"devices\": [\n";

    for (size_t i = 0; i < cache_.size(); ++i) {
        const auto& d = cache_[i];
        out << "    {\n";
        out << "      \"device_id\": \"" << JsonEscape(d.device_id) << "\",\n";
        out << "      \"device_name\": \"" << JsonEscape(d.device_name) << "\",\n";
        out << "      \"bluetooth_mac\": \"" << d.bluetooth_mac.ToString() << "\",\n";
        out << "      \"wifi_mac\": \"" << d.wifi_mac.ToString() << "\",\n";
        out << "      \"public_key\": \"" << BytesToCompactHex(d.public_key) << "\",\n";
        out << "      \"last_connected\": " << d.last_connected_timestamp << "\n";
        out << "    }" << (i + 1 < cache_.size() ? "," : "") << "\n";
    }

    out << "  ]\n";
    out << "}\n";
    out.close();

    // Atomic replace
    fs::rename(temp_path, target_path, ec);
    if (ec) {
        // Fallback: try copy + remove if rename failed across volumes
        fs::copy_file(temp_path, target_path, fs::copy_options::overwrite_existing, ec);
        fs::remove(temp_path, ec);
        if (ec) {
            return Common::Status::Error(Common::StatusCode::IoError, "Failed to atomic replace pairing store file: " + ec.message());
        }
    }

    return Common::Result<void>::Ok();
}

Common::Result<void> PairingStore::LoadFromDiskLocked() {
    loaded_ = true;
    cache_.clear();

    if (storage_path_.empty()) {
        return Common::Result<void>::Ok();
    }

    namespace fs = std::filesystem;
    if (!fs::exists(storage_path_)) {
        // File does not exist yet: healthy initial empty state
        return Common::Result<void>::Ok();
    }

    std::ifstream in(storage_path_, std::ios::in);
    if (!in.is_open()) {
        return Common::Status::Error(Common::StatusCode::IoError, "Could not open pairing store file for read: " + storage_path_);
    }

    std::stringstream ss;
    ss << in.rdbuf();
    std::string content = ss.str();

    // Parse array of devices inside "devices": [ ... ]
    size_t dev_array_start = content.find("\"devices\"");
    if (dev_array_start == std::string::npos) {
        return Common::Result<void>::Ok();
    }

    size_t open_bracket = content.find('[', dev_array_start);
    size_t close_bracket = content.rfind(']');
    if (open_bracket == std::string::npos || close_bracket == std::string::npos || close_bracket <= open_bracket) {
        return Common::Result<void>::Ok();
    }

    std::string_view array_content = std::string_view(content).substr(open_bracket + 1, close_bracket - open_bracket - 1);

    size_t obj_start = 0;
    while ((obj_start = array_content.find('{', obj_start)) != std::string_view::npos) {
        size_t obj_end = array_content.find('}', obj_start);
        if (obj_end == std::string_view::npos) break;

        std::string_view obj_slice = array_content.substr(obj_start, obj_end - obj_start + 1);

        PairedDevice device{};
        device.device_id = ExtractJsonString(obj_slice, "device_id");
        device.device_name = ExtractJsonString(obj_slice, "device_name");
        device.bluetooth_mac = ParseMac(ExtractJsonString(obj_slice, "bluetooth_mac"));
        device.wifi_mac = ParseMac(ExtractJsonString(obj_slice, "wifi_mac"));
        std::string pubkey_hex = ExtractJsonString(obj_slice, "public_key");
        if (!pubkey_hex.empty()) {
            device.public_key = HexToBytes(pubkey_hex);
        }
        device.last_connected_timestamp = ExtractJsonU64(obj_slice, "last_connected");

        if (device.IsValid()) {
            cache_.push_back(std::move(device));
        }

        obj_start = obj_end + 1;
    }

    return Common::Result<void>::Ok();
}

} // namespace CarPlayStation::Core::Session
