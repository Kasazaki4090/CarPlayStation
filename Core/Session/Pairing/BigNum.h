#pragma once
// Minimal arbitrary-precision unsigned integer (little-endian 64-bit limbs) for the
// AirPlay pairing subsystem: 3072-bit SRP-6a and the 255-bit Ed25519/X25519 field.
// Self-contained, no external dependencies.

#include <cstdint>
#include <vector>
#include <string>
#include <algorithm>
#include "Core/Common/Logging.h"
#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace CarPlayStation::Core::Session::Pairing {

class BigNum {
public:
    // limbs[0] is the least-significant 64-bit limb.
    std::vector<uint64_t> limbs;

    BigNum() = default;
    explicit BigNum(uint64_t v) { if (v) limbs = {v}; }

    static BigNum FromU64(uint64_t v) { BigNum r; if (v) r.limbs = {v}; return r; }

    [[nodiscard]] bool IsZero() const noexcept { return limbs.empty(); }

    // Big-endian byte conversion.
    static BigNum FromBytesBe(const uint8_t* data, size_t len) {
        BigNum r;
        r.limbs.assign((len + 7) / 8, 0);
        for (size_t i = 0; i < len; ++i) {
            size_t byte = len - 1 - i;
            r.limbs[i / 8] |= static_cast<uint64_t>(data[byte]) << ((i % 8) * 8);
        }
        Normalize(r);
        return r;
    }
    static BigNum FromBytesBe(const std::vector<uint8_t>& v) {
        return FromBytesBe(v.data(), v.size());
    }

    // Little-endian byte conversion.
    static BigNum FromBytesLe(const uint8_t* data, size_t len) {
        BigNum r;
        r.limbs.assign((len + 7) / 8, 0);
        for (size_t i = 0; i < len; ++i) {
            r.limbs[i / 8] |= static_cast<uint64_t>(data[i]) << ((i % 8) * 8);
        }
        Normalize(r);
        return r;
    }
    static BigNum FromBytesLe(const std::vector<uint8_t>& v) {
        return FromBytesLe(v.data(), v.size());
    }

    // Big-endian bytes, zero-padded to `len` (the modulus length). If the value is longer,
    // the full value is returned (matches the reference PAD() semantics).
    std::vector<uint8_t> ToBytesBe(size_t len) const {
        std::vector<uint8_t> out;
        if (limbs.empty()) {
            out.assign(len, 0);
            return out;
        }
        size_t nbytes = (limbs.size() * 8);
        std::vector<uint8_t> be(nbytes, 0);
        for (size_t i = 0; i < limbs.size(); ++i) {
            uint64_t v = limbs[i];
            size_t out_idx = (limbs.size() - 1 - i) * 8;
            for (int b = 0; b < 8; ++b) {
                be[out_idx + b] = static_cast<uint8_t>((v >> (56 - b * 8)) & 0xFF);
            }
        }
        if (nbytes >= len) {
            return be;
        }
        out.assign(len - nbytes, 0);
        out.insert(out.end(), be.begin(), be.end());
        return out;
    }

    static void Normalize(BigNum& r) {
        while (!r.limbs.empty() && r.limbs.back() == 0) r.limbs.pop_back();
    }

    // a - b (assumes a >= b).
    static BigNum Sub(const BigNum& a, const BigNum& b) {
        BigNum r;
        r.limbs.resize(std::max(a.limbs.size(), b.limbs.size()));
        int64_t borrow = 0;
        for (size_t i = 0; i < r.limbs.size(); ++i) {
            uint64_t av = i < a.limbs.size() ? a.limbs[i] : 0;
            uint64_t bv = i < b.limbs.size() ? b.limbs[i] : 0;
            uint64_t res = av - static_cast<uint64_t>(borrow) - bv;
            borrow = (av < bv + static_cast<uint64_t>(borrow)) ? 1 : 0;
            r.limbs[i] = res;
        }
        Normalize(r);
        return r;
    }

    static int Cmp(const BigNum& a, const BigNum& b) {
        if (a.limbs.size() != b.limbs.size())
            return a.limbs.size() < b.limbs.size() ? -1 : 1;
        for (size_t i = a.limbs.size(); i-- > 0;) {
            if (a.limbs[i] != b.limbs[i])
                return a.limbs[i] < b.limbs[i] ? -1 : 1;
        }
        return 0;
    }

    bool operator==(const BigNum& o) const { return Cmp(*this, o) == 0; }
    bool operator!=(const BigNum& o) const { return Cmp(*this, o) != 0; }

    // a + b
    static BigNum Add(const BigNum& a, const BigNum& b) {
        BigNum r;
        r.limbs.resize(std::max(a.limbs.size(), b.limbs.size()) + 1);
        uint64_t carry = 0;
        for (size_t i = 0; i < r.limbs.size(); ++i) {
            uint64_t av = i < a.limbs.size() ? a.limbs[i] : 0;
            uint64_t bv = i < b.limbs.size() ? b.limbs[i] : 0;
            uint64_t s1 = av + bv;
            uint64_t c1 = (s1 < av) ? 1 : 0;
            uint64_t s2 = s1 + carry;
            uint64_t c2 = (s2 < s1) ? 1 : 0;
            r.limbs[i] = s2;
            carry = c1 + c2;
        }
        Normalize(r);
        return r;
    }

    // 64x64 -> 128-bit multiply, portable across MSVC / GCC / Clang.
    static void Mul64x64(uint64_t a, uint64_t b, uint64_t& hi, uint64_t& lo) {
#if defined(_MSC_VER)
        lo = _umul128(a, b, &hi);
#else
        __uint128_t r = static_cast<__uint128_t>(a) * b;
        lo = static_cast<uint64_t>(r);
        hi = static_cast<uint64_t>(r >> 64);
#endif
    }

    // a * b (schoolbook).
    static BigNum Mul(const BigNum& a, const BigNum& b) {
        if (a.IsZero() || b.IsZero()) return BigNum();
        BigNum r;
        r.limbs.assign(a.limbs.size() + b.limbs.size(), 0);
        for (size_t i = 0; i < a.limbs.size(); ++i) {
            uint64_t carry = 0;
            for (size_t j = 0; j < b.limbs.size(); ++j) {
                uint64_t hi, lo;
                Mul64x64(a.limbs[i], b.limbs[j], hi, lo);
                uint64_t s1 = r.limbs[i + j] + lo;
                uint64_t c1 = (s1 < lo) ? 1 : 0;
                uint64_t s2 = s1 + carry;
                uint64_t c2 = (s2 < s1) ? 1 : 0;
                r.limbs[i + j] = s2;
                carry = hi + c1 + c2;
            }
            size_t k = i + b.limbs.size();
            while (carry) {
                if (k < r.limbs.size()) {
                    uint64_t s = r.limbs[k] + carry;
                    uint64_t c = (s < carry) ? 1 : 0;
                    r.limbs[k] = s;
                    carry = c;
                    ++k;
                } else {
                    r.limbs.push_back(carry);
                    break;
                }
            }
        }
        Normalize(r);
        return r;
    }

    static unsigned char SubBorrow64(unsigned char borrow, uint64_t a, uint64_t b, uint64_t* out) {
        uint64_t diff1 = a - b;
        unsigned char b1 = (a < b) ? 1 : 0;
        uint64_t diff2 = diff1 - borrow;
        unsigned char b2 = (diff1 < borrow) ? 1 : 0;
        *out = diff2;
        return b1 + b2;
    }

    // Long division: a = q*m + rem. Returns quotient; remainder via outRem.
    static BigNum DivMod(const BigNum& a, const BigNum& m, BigNum* outRem) {
        if (m.IsZero()) {
            if (outRem) *outRem = BigNum();
            return BigNum();
        }

        auto CmpRaw = [](const uint64_t* x, size_t x_len, const uint64_t* y, size_t y_len) -> int {
            while (x_len > 0 && x[x_len - 1] == 0) x_len--;
            while (y_len > 0 && y[y_len - 1] == 0) y_len--;
            if (x_len != y_len) return x_len < y_len ? -1 : 1;
            for (size_t i = x_len; i-- > 0;) {
                if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
            }
            return 0;
        };

        if (CmpRaw(a.limbs.data(), a.limbs.size(), m.limbs.data(), m.limbs.size()) < 0) {
            if (outRem) *outRem = a;
            return BigNum();
        }

        size_t m_len = m.limbs.size();
        while (m_len > 0 && m.limbs[m_len - 1] == 0) m_len--;

        size_t a_len = a.limbs.size();
        while (a_len > 0 && a.limbs[a_len - 1] == 0) a_len--;

        int abits = static_cast<int>(a_len * 64);

        constexpr size_t kMaxRem = 256;
        uint64_t rem[kMaxRem] = {0};
        size_t rem_len = 0;

        BigNum q;
        q.limbs.assign((abits + 63) / 64, 0);

        for (int bit = abits - 1; bit >= 0; --bit) {
            uint64_t carry = 0;
            for (size_t i = 0; i < rem_len; ++i) {
                uint64_t next = rem[i] >> 63;
                rem[i] = (rem[i] << 1) | carry;
                carry = next;
            }
            if (carry && rem_len < kMaxRem) {
                rem[rem_len++] = carry;
            }
            uint64_t abit = (a.limbs[bit / 64] >> (bit % 64)) & 1;
            if (rem_len == 0 && abit) {
                rem[0] = 1;
                rem_len = 1;
            } else if (rem_len > 0) {
                rem[0] |= abit;
            }

            if (CmpRaw(rem, rem_len, m.limbs.data(), m_len) >= 0) {
                unsigned char borrow = 0;
                for (size_t i = 0; i < rem_len; ++i) {
                    uint64_t mv = i < m_len ? m.limbs[i] : 0;
                    borrow = SubBorrow64(borrow, rem[i], mv, &rem[i]);
                }
                while (rem_len > 0 && rem[rem_len - 1] == 0) rem_len--;

                q.limbs[bit / 64] |= (1ull << (bit % 64));
            }
        }

        Normalize(q);
        if (outRem) {
            while (rem_len > 0 && rem[rem_len - 1] == 0) rem_len--;
            outRem->limbs.assign(rem, rem + rem_len);
        }
        return q;
    }

    static BigNum Mod(const BigNum& a, const BigNum& m) {
        BigNum rem;
        DivMod(a, m, &rem);
        return rem;
    }

    // base^exp mod m
    static BigNum ModPow(const BigNum& base, const BigNum& exp, const BigNum& m) {
        BigNum result = FromU64(1);
        BigNum b = Mod(base, m);
        if (b.IsZero()) {
            CPS_LOG_ERROR("BigNum", "ModPow: initial b = Mod(base, m) is ZERO! base_limbs=",
                          std::to_string(base.limbs.size()), ", m_limbs=", std::to_string(m.limbs.size()));
            return result;
        }
        BigNum e = exp;
        int step = 0;
        while (!e.IsZero()) {
            if (e.limbs[0] & 1) {
                result = Mod(Mul(result, b), m);
            }
            b = Mod(Mul(b, b), m);
            ShiftRight1(e);
            step++;
            if (b.IsZero()) {
                CPS_LOG_ERROR("BigNum", "ModPow: b became zero at step ", std::to_string(step));
                break;
            }
            if (result.IsZero()) {
                CPS_LOG_ERROR("BigNum", "ModPow: result became zero at step ", std::to_string(step));
                break;
            }
        }
        return result;
    }

    static void ShiftLeft1(BigNum& r) {
        uint64_t carry = 0;
        for (size_t i = 0; i < r.limbs.size(); ++i) {
            uint64_t next = r.limbs[i] >> 63;
            r.limbs[i] = (r.limbs[i] << 1) | carry;
            carry = next;
        }
        if (carry) r.limbs.push_back(carry);
    }

    static void ShiftRight1(BigNum& r) {
        uint64_t carry = 0;
        for (size_t i = r.limbs.size(); i-- > 0;) {
            uint64_t next = r.limbs[i] & 1;
            r.limbs[i] = (r.limbs[i] >> 1) | (carry << 63);
            carry = next;
        }
        Normalize(r);
    }

    // a mod m, where m is a 255-bit prime (used for Ed25519/X25519 field reduction).
    static BigNum Mod255(const BigNum& a, const BigNum& p) {
        return Mod(a, p);
    }

    // ---- Instance-method wrappers (used by Ed25519.h) ----
    // const so they can be called on const references (e.g. P()).
    // Note: FromBytesBe/FromU64 are static-only (a non-static overload with the
    // same signature is illegal in C++), so Ed25519.h uses the static form for those.
    BigNum Sub(const BigNum& o) const { return Sub(*this, o); }
    BigNum Add(const BigNum& o) const { return Add(*this, o); }
    BigNum Mul(const BigNum& o) const { return Mul(*this, o); }
    BigNum Mod(const BigNum& m) const { return Mod(*this, m); }
    BigNum ModPow(const BigNum& e, const BigNum& m) const { return ModPow(*this, e, m); }
    void Div(const BigNum& a, const BigNum& b) { *this = DivMod(a, b, nullptr); }
    BigNum ShiftLeft(int n) const { BigNum r = *this; for (int i = 0; i < n; ++i) ShiftLeft1(r); return r; }
    BigNum ShiftRight(int n) const { BigNum r = *this; for (int i = 0; i < n; ++i) ShiftRight1(r); return r; }
};

} // namespace CarPlayStation::Core::Session::Pairing
