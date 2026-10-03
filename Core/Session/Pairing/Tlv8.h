#pragma once
// TLV8 — the Apple/HomeKit type-length-value codec used by pair-setup, pair-verify and
// MFi-SAP. A stream of [type:u8][len:u8][value] items; a value longer than 255 bytes is
// split into consecutive fragments of the same type (every fragment but the last being
// exactly 255 bytes), and decoders coalesce them (cf. AccessorySDK TLV8CopyCoalesced).
// Zero-dependency, header-only.

#include <cstdint>
#include <vector>
#include <optional>
#include <string>

namespace CarPlayStation::Core::Session::Pairing {

// Maximum value bytes in a single TLV8 fragment (the length field is one byte).
constexpr size_t kMaxFragment = 255;

// TLV item types (HomeKit/AirPlay pairing). Values from AccessorySDK PairingUtils.c.
namespace TlvType {
    constexpr uint8_t Method        = 0x00;
    constexpr uint8_t Identifier    = 0x01;
    constexpr uint8_t Salt          = 0x02;
    constexpr uint8_t PublicKey     = 0x03;
    constexpr uint8_t Proof         = 0x04;
    constexpr uint8_t EncryptedData = 0x05;
    constexpr uint8_t State         = 0x06;
    constexpr uint8_t Error         = 0x07;
    constexpr uint8_t RetryDelay    = 0x08;
    constexpr uint8_t Certificate   = 0x09;
    constexpr uint8_t Signature     = 0x0A;
    constexpr uint8_t Permissions   = 0x0B;
    constexpr uint8_t FragmentData  = 0x0C;
    constexpr uint8_t FragmentLast  = 0x0D;
    constexpr uint8_t Flags         = 0x13;
    constexpr uint8_t Separator     = 0xFF;
}

// TLV Method values.
namespace TlvMethod {
    constexpr uint8_t PairSetup     = 0;
    constexpr uint8_t MfiPairSetup  = 1;
    constexpr uint8_t Verify        = 2;
}

// TLV Error codes.
namespace TlvError {
    constexpr uint8_t Unknown        = 0x01;
    constexpr uint8_t Authentication = 0x02;
    constexpr uint8_t Backoff        = 0x03;
    constexpr uint8_t UnknownPeer    = 0x04;
    constexpr uint8_t MaxPeers       = 0x05;
    constexpr uint8_t MaxTries       = 0x06;
}

using TlvItem = std::pair<uint8_t, std::vector<uint8_t>>;
using TlvItems = std::vector<TlvItem>;

// Encode (type, value) items into a TLV8 buffer. Values longer than kMaxFragment are split
// into consecutive same-type fragments that Decode coalesces back together.
inline std::vector<uint8_t> TlvEncode(const std::vector<std::pair<uint8_t, const uint8_t*>>& items,
                                      const std::vector<size_t>& lens) {
    std::vector<uint8_t> out;
    for (size_t i = 0; i < items.size(); ++i) {
        const uint8_t ty = items[i].first;
        const uint8_t* val = items[i].second;
        const size_t len = lens[i];
        if (len == 0) {
            out.push_back(ty);
            out.push_back(0);
            continue;
        }
        size_t off = 0;
        while (off < len) {
            size_t n = (std::min)(kMaxFragment, len - off);
            out.push_back(ty);
            out.push_back(static_cast<uint8_t>(n));
            out.insert(out.end(), val + off, val + off + n);
            off += n;
        }
    }
    return out;
}

// Convenience overload taking (type, value) pairs directly.
inline std::vector<uint8_t> TlvEncode(const std::vector<std::pair<uint8_t, std::vector<uint8_t>>>& items) {
    std::vector<std::pair<uint8_t, const uint8_t*>> ptrs;
    std::vector<size_t> lens;
    for (const auto& [ty, val] : items) {
        ptrs.emplace_back(ty, val.data());
        lens.push_back(val.size());
    }
    return TlvEncode(ptrs, lens);
}

// Decode a TLV8 buffer into (type, value) items, coalescing fragmented values.
// Returns false if the buffer is truncated.
inline bool TlvDecode(const uint8_t* buf, size_t len, TlvItems& out) {
    out.clear();
    size_t i = 0;
    while (i < len) {
        if (i + 2 > len) return false;
        const uint8_t ty = buf[i];
        const size_t vlen = buf[i + 1];
        i += 2;
        if (i + vlen > len) return false;
        const uint8_t* val = buf + i;
        i += vlen;
        // Coalesce every run of consecutive same-type items into one value (matches the C
        // TLV8CopyCoalesced): a zero-length continuation adds nothing but keeps the run going,
        // a different type starts a new item, and the same type reappearing after a different
        // type is a new, distinct value.
        if (!out.empty() && out.back().first == ty) {
            out.back().second.insert(out.back().second.end(), val, val + vlen);
        } else {
            out.emplace_back(ty, std::vector<uint8_t>(val, val + vlen));
        }
    }
    return true;
}

inline bool TlvDecode(const std::vector<uint8_t>& buf, TlvItems& out) {
    return TlvDecode(buf.data(), buf.size(), out);
}

// Return the (coalesced) value of the first item with the given type, if present.
inline std::optional<std::vector<uint8_t>> TlvGet(const TlvItems& items, uint8_t ty) {
    for (const auto& [t, v] : items) {
        if (t == ty) return v;
    }
    return std::nullopt;
}

} // namespace CarPlayStation::Core::Session::Pairing
