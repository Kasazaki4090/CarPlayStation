#pragma once
// pair-verify (accessory/server) — per-session handshake for paired controllers.
//
// Clean-room port of carplayd `verify.rs` flow:
// - Curve25519 ECDH for an ephemeral shared secret
// - Ed25519 signatures for mutual authentication
// - HKDF-SHA512 key derivation
// - ChaCha20-Poly1305 for M2/M3 sub-TLVs
//
// Flow:
//   in  M1{State=1, PublicKey=ctrl ephemeral} -> out M2{State=2, PublicKey=our ephemeral, EncryptedData=enc(Identifier || Signature)}
//   in  M3{State=3, EncryptedData=enc(Identifier || Signature)} -> out M4{State=4} (or {State=4, Error=Auth})

#include "Core/Common/Logging.h"
#include "Core/Session/Pairing/Tlv8.h"
#include "Core/Session/Pairing/Crypto.h"
#include "Core/Session/Pairing/PairSetup.h" // For AccessoryIdentity
#include <cstdint>
#include <vector>
#include <array>
#include <optional>
#include <functional>

namespace CarPlayStation::Core::Session::Pairing {

using PeerFinder = std::function<std::optional<std::array<uint8_t, 32>>(const uint8_t* id, size_t idLen)>;

class PairVerifyServer {
public:
    PairVerifyServer(AccessoryIdentity identity, PeerFinder finder)
        : identity_(std::move(identity)), finder_(std::move(finder)) {}

    [[nodiscard]] bool IsDone() const noexcept { return is_done_; }
    [[nodiscard]] const std::array<uint8_t, 32>& GetSharedSecret() const noexcept { return shared_secret_; }

    std::optional<std::vector<uint8_t>> Handle(const std::vector<uint8_t>& requestBody) {
        TlvItems items;
        if (!TlvDecode(requestBody, items)) {
            CPS_LOG_ERROR("PairVerify", "Empty or malformed TLV8 body");
            return std::nullopt;
        }

        auto stateItem = TlvGet(items, TlvType::State);
        if (!stateItem || stateItem->empty()) {
            CPS_LOG_ERROR("PairVerify", "Missing State TLV");
            return std::nullopt;
        }

        uint8_t state = stateItem->front();
        CPS_LOG_INFO("PairVerify", "Processing pair-verify inbound state=", std::to_string(state));

        switch (state) {
            case 1:
                return HandleM1(items);
            case 3:
                return HandleM3(items);
            default:
                CPS_LOG_ERROR("PairVerify", "Unexpected pair-verify state=", std::to_string(state));
                return std::nullopt;
        }
    }

private:
    std::optional<std::vector<uint8_t>> HandleM1(const TlvItems& items) {
        auto pkItem = TlvGet(items, TlvType::PublicKey);
        if (!pkItem || pkItem->size() != 32) {
            CPS_LOG_ERROR("PairVerify", "M1 missing or invalid PublicKey (expected 32 bytes)");
            return std::nullopt;
        }
        memcpy(peer_curve_pk_.data(), pkItem->data(), 32);

        // Generate 32-byte random seed for ephemeral key
        uint8_t rnd[32];
        if (BCryptGenRandom(nullptr, rnd, sizeof(rnd), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
            CPS_LOG_ERROR("PairVerify", "BCryptGenRandom failed");
            return std::nullopt;
        }

        const char* ecdhSalt = "Pair-Verify-ECDH-Salt";
        const char* ecdhInfo = "Pair-Verify-ECDH-Info";
        auto curve_sk = pairing::HkdfSha512(rnd, 32,
                                             reinterpret_cast<const uint8_t*>(ecdhSalt), strlen(ecdhSalt),
                                             reinterpret_cast<const uint8_t*>(ecdhInfo), strlen(ecdhInfo),
                                             32);

        pairing::Curve25519BaseMult(our_curve_pk_.data(), curve_sk.data());
        pairing::Curve25519Mult(shared_secret_.data(), curve_sk.data(), peer_curve_pk_.data());

        const char* encSalt = "Pair-Verify-Encrypt-Salt";
        const char* encInfo = "Pair-Verify-Encrypt-Info";
        auto encKey = pairing::HkdfSha512(shared_secret_.data(), 32,
                                           reinterpret_cast<const uint8_t*>(encSalt), strlen(encSalt),
                                           reinterpret_cast<const uint8_t*>(encInfo), strlen(encInfo),
                                           32);
        memcpy(encrypt_key_.data(), encKey.data(), 32);

        // Sign: our_curve_pk || identity.id || peer_curve_pk
        std::vector<uint8_t> to_sign;
        to_sign.insert(to_sign.end(), our_curve_pk_.begin(), our_curve_pk_.end());
        to_sign.insert(to_sign.end(), identity_.id.begin(), identity_.id.end());
        to_sign.insert(to_sign.end(), peer_curve_pk_.begin(), peer_curve_pk_.end());

        auto sig = pairing::Ed25519Sign(identity_.ltpk_secret.data(), to_sign.data(), to_sign.size());
        if (!sig) {
            CPS_LOG_ERROR("PairVerify", "Failed to sign M2 payload");
            return std::nullopt;
        }

        // Sub-TLV: Identifier || Signature
        auto subBytes = TlvEncode({
            {TlvType::Identifier, identity_.id},
            {TlvType::Signature, std::vector<uint8_t>(sig->begin(), sig->end())},
        });

        // Seal with ChaCha20-Poly1305 nonce "PV-Msg02"
        auto enc = pairing::ChaCha20Poly1305Seal(encrypt_key_.data(), "PV-Msg02", subBytes.data(), subBytes.size());

        state_ = 2;
        CPS_LOG_INFO("PairVerify", "M2 response generated (", std::to_string(enc.size()), " enc bytes, our pubkey len=32)");
        // Emit M2: EncryptedData, State=2, PublicKey
        return TlvEncode({
            {TlvType::EncryptedData, enc},
            {TlvType::State, {2}},
            {TlvType::PublicKey, std::vector<uint8_t>(our_curve_pk_.begin(), our_curve_pk_.end())},
        });
    }

    std::optional<std::vector<uint8_t>> HandleM3(const TlvItems& items) {
        if (state_ != 2) {
            CPS_LOG_ERROR("PairVerify", "M3 received in invalid state=", std::to_string(state_));
            return std::nullopt;
        }

        auto encItem = TlvGet(items, TlvType::EncryptedData);
        if (!encItem || encItem->empty()) {
            CPS_LOG_ERROR("PairVerify", "M3 missing EncryptedData");
            return std::nullopt;
        }

        auto pt = pairing::ChaCha20Poly1305Open(encrypt_key_.data(), "PV-Msg03", encItem->data(), encItem->size());
        if (!pt) {
            CPS_LOG_ERROR("PairVerify", "M3 ChaCha20-Poly1305 decrypt/auth failed");
            return MakeErrorM4();
        }

        TlvItems subItems;
        if (!TlvDecode(*pt, subItems)) {
            CPS_LOG_ERROR("PairVerify", "M3 failed to decode sub-TLV");
            return MakeErrorM4();
        }

        auto idItem = TlvGet(subItems, TlvType::Identifier);
        auto sigItem = TlvGet(subItems, TlvType::Signature);
        if (!idItem || !sigItem || sigItem->size() != 64) {
            CPS_LOG_ERROR("PairVerify", "M3 sub-TLV missing Identifier or 64-byte Signature");
            return MakeErrorM4();
        }

        std::string peer_id_str(reinterpret_cast<const char*>(idItem->data()), idItem->size());
        CPS_LOG_INFO("PairVerify", "M3 controller ID: ", peer_id_str);

        if (!finder_) {
            CPS_LOG_ERROR("PairVerify", "No PeerFinder configured to look up controller");
            return MakeErrorM4();
        }

        auto peer_ltpk = finder_(idItem->data(), idItem->size());
        if (!peer_ltpk) {
            CPS_LOG_ERROR("PairVerify", "Controller ", peer_id_str, " not found in pairing store");
            return MakeErrorM4();
        }

        // Verify signature over: peer_curve_pk || peer_id || our_curve_pk
        std::vector<uint8_t> to_verify;
        to_verify.insert(to_verify.end(), peer_curve_pk_.begin(), peer_curve_pk_.end());
        to_verify.insert(to_verify.end(), idItem->begin(), idItem->end());
        to_verify.insert(to_verify.end(), our_curve_pk_.begin(), our_curve_pk_.end());

        bool ok = pairing::Ed25519Verify(peer_ltpk->data(), to_verify.data(), to_verify.size(), sigItem->data());
        if (!ok) {
            CPS_LOG_ERROR("PairVerify", "Controller signature verification FAILED");
            return MakeErrorM4();
        }

        CPS_LOG_INFO("PairVerify", "Controller signature verified successfully! Pair-verify handshake complete.");
        is_done_ = true;
        state_ = 4;

        return TlvEncode({
            {TlvType::State, {4}},
        });
    }

    std::vector<uint8_t> MakeErrorM4() {
        state_ = 4;
        return TlvEncode({
            {TlvType::Error, {TlvError::Authentication}},
            {TlvType::State, {4}},
        });
    }

    AccessoryIdentity identity_;
    PeerFinder finder_;
    int state_{0};
    bool is_done_{false};
    std::array<uint8_t, 32> our_curve_pk_{};
    std::array<uint8_t, 32> peer_curve_pk_{};
    std::array<uint8_t, 32> shared_secret_{};
    std::array<uint8_t, 32> encrypt_key_{};
};

} // namespace CarPlayStation::Core::Session::Pairing
