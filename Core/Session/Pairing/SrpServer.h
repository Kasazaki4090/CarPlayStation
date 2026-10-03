#pragma once
// SRP-6a accessory (server) side for CarPlay/HomeKit pair-setup — RFC 5054 3072-bit MODP
// group, generator 5, SHA-512. The iPhone (fast-srp-hap) requires the full RFC-5054 proof
// M1 = H( H(N) XOR H(g) | H(I) | s | PAD(A) | PAD(B) | K ) with every value zero-padded to
// the modulus length, except H(g) which is over the minimal generator byte [5] (verified
// against a live iPhone capture). Built on the local BigNum + CNG SHA-512.

#include "Core/Session/Pairing/BigNum.h"
#include "Core/Session/Pairing/Crypto.h"
#include <cstdint>
#include <cstring>
#include <vector>
#include <optional>
#include <random>

namespace CarPlayStation::Core::Session::Pairing {

// RFC 3526 / RFC 5054 3072-bit MODP prime (generator 5).
inline const std::vector<uint8_t>& SrpPrime() {
    static const std::vector<uint8_t> n = [] {
        const char* hex =
            "FFFFFFFFFFFFFFFFC90FDAA22168C234C4C6628B80DC1CD129024E088A67CC74"
            "020BBEA63B139B22514A08798E3404DDEF9519B3CD3A431B302B0A6DF25F1437"
            "4FE1356D6D51C245E485B576625E7EC6F44C42E9A637ED6B0BFF5CB6F406B7ED"
            "EE386BFB5A899FA5AE9F24117C4B1FE649286651ECE45B3DC2007CB8A163BF05"
            "98DA48361C55D39A69163FA8FD24CF5F83655D23DCA3AD961C62F356208552BB"
            "9ED529077096966D670C354E4ABC9804F1746C08CA18217C32905E462E36CE3B"
            "E39E772C180E86039B2783A2EC07A28FB5C55DF06F4C52C9DE2BCBF695581718"
            "3995497CEA956AE515D2261898FA051015728E5A8AAAC42DAD33170D04507A33"
            "A85521ABDF1CBA64ECFB850458DBEF0A8AEA71575D060C7DB3970F85A6E1E4C7"
            "ABF5AE8CDB0933D71E8C94E04A25619DCEE3D2261AD2EE6BF12FFA06D98A0864"
            "D87602733EC86A64521F2B18177B200CBBE117577A615D6C770988C0BAD946E2"
            "08E24FA074E5AB3143DB5BFCE0FD108E4B82D120A93AD2CAFFFFFFFFFFFFFFFF";
        auto hexVal = [](char c) -> uint8_t {
            if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
            if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(c - 'a' + 10);
            if (c >= 'A' && c <= 'F') return static_cast<uint8_t>(c - 'A' + 10);
            return 0;
        };
        std::vector<uint8_t> out;
        size_t len = strlen(hex);
        out.reserve(len / 2);
        for (size_t i = 0; i + 1 < len; i += 2) {
            out.push_back(static_cast<uint8_t>((hexVal(hex[i]) << 4) | hexVal(hex[i + 1])));
        }
        return out;
    }();
    return n;
}

// Left-pad a big-endian value to `len` (the modulus length) — RFC 5054 PAD().
inline std::vector<uint8_t> SrpPad(const BigNum& x, size_t len) {
    return x.ToBytesBe(len);
}

inline std::vector<uint8_t> SrpSha512(const std::vector<std::vector<uint8_t>>& parts) {
    return pairing::Sha512(parts);
}

// SRP-6a accessory/server.
class SrpServer {
public:
    // Create with a fixed server private `b` (32 random bytes in production).
    SrpServer(const std::vector<uint8_t>& username, const std::vector<uint8_t>& password,
              const std::vector<uint8_t>& salt, const std::vector<uint8_t>& b) {
        username_ = username;
        salt_ = salt;
        n_ = BigNum::FromBytesBe(SrpPrime());
        n_len_ = SrpPrime().size();
        g_ = BigNum::FromU64(5);
        // k = H(PAD(N) | PAD(g))
        k_big_ = BigNum::FromBytesBe(SrpSha512({SrpPad(n_, n_len_), SrpPad(g_, n_len_)}));
        // x = H(s | H(I | ":" | P))
        std::vector<uint8_t> ip;
        ip.insert(ip.end(), username.begin(), username.end());
        ip.push_back(':');
        ip.insert(ip.end(), password.begin(), password.end());
        x_ = BigNum::FromBytesBe(SrpSha512({salt, pairing::Sha512({ip})}));
        CPS_LOG_INFO("SrpServer", "Computing v_: g_limbs=", std::to_string(g_.limbs.size()),
                     ", g0=", std::to_string(g_.limbs.empty() ? 0 : g_.limbs[0]),
                     ", x_limbs=", std::to_string(x_.limbs.size()),
                     ", n_limbs=", std::to_string(n_.limbs.size()));
        v_ = BigNum::ModPow(g_, x_, n_);
        b_ = BigNum::FromBytesBe(b);

        CPS_LOG_INFO("SrpServer", "Computing gb: b_limbs=", std::to_string(b_.limbs.size()));
        auto gb = BigNum::ModPow(g_, b_, n_);
        auto kv = BigNum::Mul(k_big_, v_);
        auto sum = BigNum::Add(kv, gb);
        b_pub_ = BigNum::Mod(sum, n_);

        CPS_LOG_INFO("SrpServer", "SrpServer init: n_limbs=", std::to_string(n_.limbs.size()),
                     ", k_limbs=", std::to_string(k_big_.limbs.size()),
                     ", x_limbs=", std::to_string(x_.limbs.size()),
                     ", v_limbs=", std::to_string(v_.limbs.size()),
                     ", b_limbs=", std::to_string(b_.limbs.size()),
                     ", gb_limbs=", std::to_string(gb.limbs.size()),
                     ", kv_limbs=", std::to_string(kv.limbs.size()),
                     ", b_pub_limbs=", std::to_string(b_pub_.limbs.size()));
    }

    // Create with a fresh random server private `b`.
    static SrpServer New(const std::vector<uint8_t>& username, const std::vector<uint8_t>& password,
                         const std::vector<uint8_t>& salt) {
        std::vector<uint8_t> b(32);
        std::random_device rd;
        for (auto& x : b) x = static_cast<uint8_t>(rd() & 0xFF);
        return SrpServer(username, password, salt, b);
    }

    // The server public value B, padded to the modulus length (sent in M2).
    [[nodiscard]] std::vector<uint8_t> BPub() const { return SrpPad(b_pub_, n_len_); }
    [[nodiscard]] const std::vector<uint8_t>& Salt() const { return salt_; }

    // Verify the client's M3 (A, clientProof). On success returns the server proof M2 and
    // stores the session key K; on mismatch returns nullopt.
    [[nodiscard]] std::optional<std::vector<uint8_t>> Verify(const std::vector<uint8_t>& a_pub,
                                                              const std::vector<uint8_t>& client_proof) {
        a_ = BigNum::FromBytesBe(a_pub);
        if (BigNum::Mod(a_, n_).IsZero()) return std::nullopt;
        // u = H(PAD(A) | PAD(B))
        u_ = BigNum::FromBytesBe(SrpSha512({SrpPad(a_, n_len_), SrpPad(b_pub_, n_len_)}));
        // S = (A * v^u)^b mod N
        s_ = BigNum::ModPow(BigNum::Mod(BigNum::Mul(a_, BigNum::ModPow(v_, u_, n_)), n_), b_, n_);
        // K = H(PAD(S))
        session_key_ = SrpSha512({SrpPad(s_, n_len_)});
        // M1 = H( H(N) XOR H(g) | H(I) | s | PAD(A) | PAD(B) | K ).
        // H(g) is over the MINIMAL generator byte [5] — NOT padded.
        std::vector<uint8_t> hn = SrpSha512({SrpPad(n_, n_len_)});
        std::vector<uint8_t> hg = SrpSha512({{5}});
        std::vector<uint8_t> hng(hn.size());
        for (size_t i = 0; i < hn.size(); ++i) hng[i] = hn[i] ^ hg[i];
        std::vector<uint8_t> hi = SrpSha512({username_});
        std::vector<uint8_t> m1 = SrpSha512({hng, hi, salt_, SrpPad(a_, n_len_), SrpPad(b_pub_, n_len_), session_key_});
        if (m1 != client_proof) return std::nullopt;
        // M2 = H(PAD(A) | M1 | K)
        std::vector<uint8_t> m2 = SrpSha512({SrpPad(a_, n_len_), m1, session_key_});
        return m2;
    }

    // The SRP session key K (HKDF input keying material) — available after a successful verify.
    [[nodiscard]] const std::vector<uint8_t>& SessionKey() const { return session_key_; }

private:
    BigNum n_, g_, x_, v_, b_, b_pub_;
    BigNum k_big_;   // k = H(PAD(N)|PAD(g)) as a big integer
    size_t n_len_{0};
    std::vector<uint8_t> salt_, username_;
    BigNum a_, u_, s_;
    std::vector<uint8_t> session_key_;
};

} // namespace CarPlayStation::Core::Session::Pairing
