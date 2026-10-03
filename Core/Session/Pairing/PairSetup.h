#pragma once
// pair-setup (accessory/server) — first-time CarPlay/HomeKit pairing.
//
// Clean-room port of the carplayd `setup.rs` flow: SRP-6a (RFC 5054 3072-bit group,
// SHA-512) proves the setup code, then an Ed25519 long-term-key exchange
// (ChaCha20-Poly1305 encrypted) records the pairing so future sessions can pair-verify.
//
// Flow (driven by successive Handle() calls, one per inbound POST /pair-setup):
//   in  M1{State=1, Method}            -> out M2{State=2, Salt, PublicKey=B}
//   in  M3{State=3, PublicKey=A, Proof} -> out M4{State=4, Proof=M2}
//   in  M5{State=5, EncryptedData}     -> out M6{State=6, EncryptedData}
//
// Header-only. Built on the local SrpServer, Tlv8, Crypto (HKDF/ChaCha20-Poly1305) and
// the pure-C++ Ed25519.

#include "Core/Common/Logging.h"
#include "Core/Session/Pairing/SrpServer.h"
#include "Core/Session/Pairing/Tlv8.h"
#include "Core/Session/Pairing/Crypto.h"
#include "Core/Session/Pairing/Ed25519.h"
#include <cstdint>
#include <cstring>
#include <vector>
#include <optional>
#include <functional>
#include <random>

namespace CarPlayStation::Core::Session::Pairing {

// The fixed SRP username and the default CarPlay setup code.
inline const std::vector<uint8_t>& PairSetupUsername() {
    static const std::vector<uint8_t> u = {'P', 'a', 'i', 'r', '-', 'S', 'e', 't', 'u', 'p'};
    return u;
}
inline const std::vector<uint8_t>& PairSetupCode() {
    static const std::vector<uint8_t> c = {'3', '9', '3', '9'};
    return c;
}

// Records a newly-paired controller's Ed25519 long-term public key.
using PeerSaver = std::function<void(const uint8_t* id, size_t idLen, const uint8_t* ltpk)>;

// The accessory's persistent identity: its pairing identifier, Ed25519 long-term public
// key (32 bytes) and the matching 32-byte seed used to sign the M6 accessory proof.
struct AccessoryIdentity {
    std::vector<uint8_t> id;          // accessory pairing identifier
    std::array<uint8_t, 32> ltpk{};  // accessory Ed25519 long-term public key
    std::array<uint8_t, 32> ltpk_secret{}; // accessory Ed25519 seed
};

// One pair-setup exchange. Drive with successive Handle() calls (one per inbound
// POST /pair-setup request). Returns the TLV8 response bytes, or nullopt on a fatal
// error (malformed TLV, SRP failure, decrypt/auth failure) — the caller should then
// close the connection.
class PairSetupServer {
public:
    PairSetupServer(AccessoryIdentity identity, PeerSaver saver)
        : identity_(std::move(identity)), saver_(std::move(saver)) {}

    // True once M6 has been produced (pairing recorded).
    [[nodiscard]] bool IsDone() const noexcept { return done_; }

    // Process one inbound TLV8 message. Returns the TLV8 response, or nullopt on a
    // fatal error.
    [[nodiscard]] std::optional<std::vector<uint8_t>> Handle(const std::vector<uint8_t>& input) {
        TlvItems items;
        if (!TlvDecode(input, items)) {
            CPS_LOG_ERROR("PairSetup", "TlvDecode failed on input len=", std::to_string(input.size()));
            return std::nullopt;
        }

        auto state = TlvGet(items, TlvType::State);
        if (!state || state->empty()) {
            CPS_LOG_ERROR("PairSetup", "Missing State TLV item in input");
            return std::nullopt;
        }
        uint8_t st = state->front();
        CPS_LOG_INFO("PairSetup", "Handling pair-setup state M", std::to_string(st));
        switch (st) {
            case 1: return M1ToM2();
            case 3: return M3ToM4(items);
            case 5: return M5ToM6(items);
            default:
                CPS_LOG_ERROR("PairSetup", "Unknown pair-setup state: ", std::to_string(st));
                return std::nullopt;
        }
    }

private:
    std::optional<std::vector<uint8_t>> M1ToM2() {
        std::vector<uint8_t> salt(16);
        std::random_device rd;
        for (auto& x : salt) x = static_cast<uint8_t>(rd() & 0xFF);

        srp_ = SrpServer::New(PairSetupUsername(), PairSetupCode(), salt);
        auto b_pub = srp_->BPub();
        state_ = 2;

        auto hex_str = [](const std::vector<uint8_t>& data, size_t max_bytes = 16) {
            std::string s;
            for (size_t i = 0; i < (std::min)(data.size(), max_bytes); ++i) {
                char b[4];
                snprintf(b, sizeof(b), "%02x", data[i]);
                s += b;
            }
            return s;
        };

        std::string bpub_suffix;
        if (b_pub.size() >= 16) {
            for (size_t i = b_pub.size() - 16; i < b_pub.size(); ++i) {
                char b[4];
                snprintf(b, sizeof(b), "%02x", b_pub[i]);
                bpub_suffix += b;
            }
        }

        CPS_LOG_INFO("PairSetup", "Generated M2: salt=", hex_str(salt),
                     ", BPub len=", std::to_string(b_pub.size()),
                     ", BPub prefix=", hex_str(b_pub, 32),
                     ", BPub suffix=", bpub_suffix);

        auto out = TlvEncode({
            {TlvType::State, {2}},
            {TlvType::Salt, salt},
            {TlvType::PublicKey, b_pub},
        });
        CPS_LOG_INFO("PairSetup", "Encoded M2 TLV: len=", std::to_string(out.size()),
                     ", prefix=", hex_str(out, 16));
        return out;
    }

    std::optional<std::vector<uint8_t>> M3ToM4(const TlvItems& m3) {
        if (state_ != 2 || !srp_) {
            CPS_LOG_ERROR("PairSetup", "M3 received in invalid state: ", std::to_string(state_));
            return std::nullopt;
        }
        auto a_pub = TlvGet(m3, TlvType::PublicKey);
        auto client_proof = TlvGet(m3, TlvType::Proof);
        if (!a_pub || !client_proof) {
            CPS_LOG_ERROR("PairSetup", "M3 missing PublicKey or Proof TLV");
            return std::nullopt;
        }

        auto hex_str = [](const std::vector<uint8_t>& data, size_t max_bytes = 16) {
            std::string s;
            for (size_t i = 0; i < (std::min)(data.size(), max_bytes); ++i) {
                char b[4];
                snprintf(b, sizeof(b), "%02x", data[i]);
                s += b;
            }
            return s;
        };

        CPS_LOG_INFO("PairSetup", "Verifying M3 (APub len=", std::to_string(a_pub->size()),
                     " prefix=", hex_str(*a_pub, 16),
                     ", ClientProof len=", std::to_string(client_proof->size()),
                     " prefix=", hex_str(*client_proof, 16), ")");
        auto m2_proof = srp_->Verify(*a_pub, *client_proof);
        if (!m2_proof) {
            CPS_LOG_ERROR("PairSetup", "SRP verification failed (setup PIN or proof mismatch)");
            // C emits Error before State on the failure path.
            return TlvEncode({
                {TlvType::Error, {TlvError::Authentication}},
                {TlvType::State, {4}},
            });
        }
        srp_key_ = srp_->SessionKey();
        state_ = 4;
        CPS_LOG_INFO("PairSetup", "M3 verified successfully, emitting M4 proof (len=", std::to_string(m2_proof->size()), ")");
        return TlvEncode({
            {TlvType::State, {4}},
            {TlvType::Proof, *m2_proof},
        });
    }

    std::optional<std::vector<uint8_t>> M5ToM6(const TlvItems& m5) {
        if (state_ != 4) {
            CPS_LOG_ERROR("PairSetup", "M5 received in invalid state: ", std::to_string(state_));
            return std::nullopt;
        }
        auto enc = TlvGet(m5, TlvType::EncryptedData);
        if (!enc) {
            CPS_LOG_ERROR("PairSetup", "M5 missing EncryptedData TLV");
            return std::nullopt;
        }

        CPS_LOG_INFO("PairSetup", "M5 decrypting payload len=", std::to_string(enc->size()));
        auto key = pairing::HkdfSha512(srp_key_, "Pair-Setup-Encrypt-Salt",
                                       "Pair-Setup-Encrypt-Info", 32);
        auto pt = pairing::ChaCha20Poly1305Open(key.data(), "PS-Msg05",
                                                 enc->data(), enc->size());
        if (!pt) {
            CPS_LOG_ERROR("PairSetup", "M5 ChaCha20Poly1305Open failed (auth tag mismatch)");
            return std::nullopt;
        }

        CPS_LOG_INFO("PairSetup", "M5 decrypted plaintext len=", std::to_string(pt->size()));
        TlvItems sub;
        if (!TlvDecode(*pt, sub)) {
            CPS_LOG_ERROR("PairSetup", "M5 TlvDecode failed on decrypted plaintext");
            return std::nullopt;
        }
        auto ctrl_id = TlvGet(sub, TlvType::Identifier);
        auto ctrl_ltpk = TlvGet(sub, TlvType::PublicKey);
        auto ctrl_sig = TlvGet(sub, TlvType::Signature);
        if (!ctrl_id || !ctrl_ltpk || ctrl_ltpk->size() != 32 ||
            !ctrl_sig || ctrl_sig->size() != 64) {
            CPS_LOG_ERROR("PairSetup", "M5 decrypted sub-items invalid or missing");
            return std::nullopt;
        }

        std::string id_str(reinterpret_cast<const char*>(ctrl_id->data()), ctrl_id->size());
        CPS_LOG_INFO("PairSetup", "M5 controller id='", id_str, "', ltpk_len=", std::to_string(ctrl_ltpk->size()));

        // Verify the controller signed HKDF(K,Controller-Sign) || ctrlID || ctrlLTPK.
        auto dk = pairing::HkdfSha512(srp_key_, "Pair-Setup-Controller-Sign-Salt",
                                       "Pair-Setup-Controller-Sign-Info", 32);
        std::vector<uint8_t> signed_msg;
        signed_msg.insert(signed_msg.end(), dk.begin(), dk.end());
        signed_msg.insert(signed_msg.end(), ctrl_id->begin(), ctrl_id->end());
        signed_msg.insert(signed_msg.end(), ctrl_ltpk->begin(), ctrl_ltpk->end());

        bool ok = ed25519::Verify(ctrl_ltpk->data(), signed_msg.data(), signed_msg.size(),
                                   ctrl_sig->data());
        if (!ok) {
            CPS_LOG_ERROR("PairSetup", "M5 controller Ed25519 signature verify failed");
            return TlvEncode({
                {TlvType::Error, {TlvError::Authentication}},
                {TlvType::State, {6}},
            });
        }
        CPS_LOG_INFO("PairSetup", "M5 controller signature verified successfully!");
        if (saver_) saver_(ctrl_id->data(), ctrl_id->size(), ctrl_ltpk->data());

        // Accessory side: sign HKDF(K,Accessory-Sign) || accID || accLTPK.
        auto ak = pairing::HkdfSha512(srp_key_, "Pair-Setup-Accessory-Sign-Salt",
                                       "Pair-Setup-Accessory-Sign-Info", 32);
        std::vector<uint8_t> signed2;
        signed2.insert(signed2.end(), ak.begin(), ak.end());
        signed2.insert(signed2.end(), identity_.id.begin(), identity_.id.end());
        signed2.insert(signed2.end(), identity_.ltpk.begin(), identity_.ltpk.end());
        auto sig = ed25519::Sign(identity_.ltpk_secret.data(), signed2.data(), signed2.size());

        auto sub2 = TlvEncode({
            {TlvType::Identifier, identity_.id},
            {TlvType::PublicKey, std::vector<uint8_t>(identity_.ltpk.begin(), identity_.ltpk.end())},
            {TlvType::Signature, sig},
        });
        auto enc2 = pairing::ChaCha20Poly1305Seal(key.data(), "PS-Msg06",
                                                   sub2.data(), sub2.size());
        done_ = true;
        CPS_LOG_INFO("PairSetup", "M6 generated successfully, enc2_len=", std::to_string(enc2.size()));
        // C emits EncryptedData before State in M6.
        return TlvEncode({
            {TlvType::EncryptedData, enc2},
            {TlvType::State, {6}},
        });
    }

    AccessoryIdentity identity_;
    PeerSaver saver_;
    uint8_t state_{0};
    std::optional<SrpServer> srp_;
    std::vector<uint8_t> srp_key_;
    bool done_{false};
};

} // namespace CarPlayStation::Core::Session::Pairing
