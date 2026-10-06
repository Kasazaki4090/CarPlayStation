#include "Core/Session/AccessoryIdentityStore.h"
#include "Core/Session/Pairing/Ed25519.h"
#include "Core/Common/Logging.h"
#include <fstream>
#include <sstream>
#include <filesystem>
#include <iomanip>

namespace CarPlayStation::Core::Session {

namespace {

constexpr const char* kLogTag = "AccessoryID";

[[nodiscard]] std::string ToHex(Common::ByteSpan bytes) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (uint8_t b : bytes) {
        oss << std::setw(2) << static_cast<int>(b);
    }
    return oss.str();
}

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

// Decodes a hex string into exactly out.size() bytes. Returns false on any
// malformed or wrong-length input so a corrupt file falls back to a new key.
bool FromHex(std::string_view hex, Common::MutableByteSpan out) {
    if (hex.size() != out.size() * 2) return false;
    auto nibble = [](char c, uint8_t& v) -> bool {
        if (c >= '0' && c <= '9') v = static_cast<uint8_t>(c - '0');
        else if (c >= 'a' && c <= 'f') v = static_cast<uint8_t>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v = static_cast<uint8_t>(c - 'A' + 10);
        else return false;
        return true;
    };
    for (size_t i = 0; i < out.size(); ++i) {
        uint8_t hi = 0;
        uint8_t lo = 0;
        if (!nibble(hex[i * 2], hi) || !nibble(hex[i * 2 + 1], lo)) return false;
        out[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return true;
}

}  // namespace

Pairing::AccessoryIdentity StoredAccessoryIdentity::ToAccessoryIdentity() const {
    Pairing::AccessoryIdentity identity;
    identity.ltpk = public_key;
    identity.ltpk_secret = secret_key;
    return identity;
}

AccessoryIdentityStore::AccessoryIdentityStore(std::string path) : path_(std::move(path)) {
    if (Load()) {
        CPS_LOG_INFO(kLogTag, "Loaded accessory identity from: ", path_);
        return;
    }

    auto kp = Pairing::ed25519::GenerateKeyPair();
    identity_.public_key = kp.pubKey;
    identity_.secret_key = kp.secret;
    generated_ = true;
    if (Save()) {
        CPS_LOG_INFO(kLogTag, "Generated new accessory identity: ", path_);
    } else {
        CPS_LOG_WARN(kLogTag, "Could not save accessory identity to: ", path_);
    }
}

bool AccessoryIdentityStore::Load() {
    std::ifstream file(path_);
    if (!file.is_open()) return false;

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();

    std::string pub_hex = ExtractJsonString(content, "publicKey");
    std::string secret_hex = ExtractJsonString(content, "secretKey");
    if (!FromHex(pub_hex, identity_.public_key) ||
        !FromHex(secret_hex, identity_.secret_key)) {
        CPS_LOG_WARN(kLogTag, "Accessory identity file is malformed: ", path_);
        return false;
    }
    return true;
}

bool AccessoryIdentityStore::Save() const {
    std::filesystem::path target(path_);
    if (target.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(target.parent_path(), ec);
    }

    std::ostringstream oss;
    oss << "{\n  \"version\": 1,\n"
        << "  \"publicKey\": \"" << ToHex(identity_.public_key) << "\",\n"
        << "  \"secretKey\": \"" << ToHex(identity_.secret_key) << "\"\n}\n";

    // Write to a temp file then rename, so a crash cannot leave a half-written key.
    std::filesystem::path tmp = target;
    tmp += ".tmp";
    std::ofstream out(tmp, std::ios::binary);
    if (!out.is_open()) return false;
    out << oss.str();
    out.flush();
    if (!out.good()) {
        std::filesystem::remove(tmp);
        return false;
    }
    out.close();

    std::error_code ec;
    std::filesystem::rename(tmp, target, ec);
    if (ec) {
        std::filesystem::remove(tmp);
        return false;
    }
    return true;
}

}  // namespace CarPlayStation::Core::Session
