#pragma once
// Minimal Apple binary property list (bplist00) encoder for AirPlay/CarPlay.
//
// Supports: Dictionaries, Arrays, Strings, Integers, Booleans, Binary Data.
// Generates standard bplist00 bytes matching Apple PropertyListSerialization.

#include <cstdint>
#include <vector>
#include <string>
#include <utility>
#include <cstring>
#include <algorithm>

namespace CarPlayStation::Core::Session::Pairing {

struct BplistValue;
using BplistArray = std::vector<BplistValue>;
using BplistDict = std::vector<std::pair<std::string, BplistValue>>;

struct BplistValue {
    enum class Type {
        Boolean,
        Integer,
        String,
        Data,
        Array,
        Dict
    };

    Type type{Type::Boolean};
    bool bool_val{false};
    int64_t int_val{0};
    std::string str_val;
    std::vector<uint8_t> data_val;
    BplistArray array_val;
    BplistDict dict_val;

    BplistValue() : type(Type::Boolean), bool_val(false) {}
    BplistValue(bool b) : type(Type::Boolean), bool_val(b) {}
    BplistValue(int i) : type(Type::Integer), int_val(i) {}
    BplistValue(int64_t i) : type(Type::Integer), int_val(i) {}
    BplistValue(uint16_t u) : type(Type::Integer), int_val(static_cast<int64_t>(u)) {}
    BplistValue(uint32_t u) : type(Type::Integer), int_val(static_cast<int64_t>(u)) {}
    BplistValue(uint64_t u) : type(Type::Integer), int_val(static_cast<int64_t>(u)) {}
    BplistValue(const char* s) : type(Type::String), str_val(s ? s : "") {}
    BplistValue(std::string s) : type(Type::String), str_val(std::move(s)) {}
    BplistValue(std::vector<uint8_t> d) : type(Type::Data), data_val(std::move(d)) {}
    BplistValue(BplistArray a) : type(Type::Array), array_val(std::move(a)) {}
    BplistValue(BplistDict d) : type(Type::Dict), dict_val(std::move(d)) {}

    [[nodiscard]] const BplistValue* Find(const std::string& key) const {
        if (type != Type::Dict) return nullptr;
        for (const auto& kv : dict_val) {
            if (kv.first == key) return &kv.second;
        }
        return nullptr;
    }

    [[nodiscard]] int64_t AsInt(int64_t def = 0) const {
        return (type == Type::Integer) ? int_val : def;
    }

    [[nodiscard]] uint64_t AsUInt(uint64_t def = 0) const {
        return (type == Type::Integer) ? static_cast<uint64_t>(int_val) : def;
    }

    [[nodiscard]] std::string AsString(const std::string& def = "") const {
        return (type == Type::String) ? str_val : def;
    }

    [[nodiscard]] bool AsBool(bool def = false) const {
        return (type == Type::Boolean) ? bool_val : def;
    }

    [[nodiscard]] const BplistArray* AsArray() const {
        return (type == Type::Array) ? &array_val : nullptr;
    }

    [[nodiscard]] const BplistDict* AsDict() const {
        return (type == Type::Dict) ? &dict_val : nullptr;
    }
};

class BplistDecoder {
public:
    static BplistValue Decode(const uint8_t* data, size_t size) {
        if (!data || size < 40) return BplistValue(false);
        if (memcmp(data, "bplist00", 8) != 0) return BplistValue(false);

        size_t trailer = size - 32;
        uint8_t offset_size = data[trailer + 6];
        uint8_t ref_size = data[trailer + 7];
        if (offset_size == 0 || ref_size == 0) return BplistValue(false);

        auto read_be = [](const uint8_t* p, size_t bytes) -> uint64_t {
            uint64_t val = 0;
            for (size_t i = 0; i < bytes; ++i) {
                val = (val << 8) | p[i];
            }
            return val;
        };

        uint64_t num_objects = read_be(data + trailer + 8, 8);
        uint64_t top_object = read_be(data + trailer + 16, 8);
        uint64_t offset_table_offset = read_be(data + trailer + 24, 8);

        if (num_objects == 0 || top_object >= num_objects) return BplistValue(false);
        if (offset_table_offset + num_objects * offset_size > trailer) return BplistValue(false);

        std::vector<uint64_t> offsets(num_objects);
        for (size_t i = 0; i < num_objects; ++i) {
            offsets[i] = read_be(data + offset_table_offset + i * offset_size, offset_size);
            if (offsets[i] >= trailer) return BplistValue(false);
        }

        std::vector<bool> visiting(num_objects, false);
        auto read_object = [&](auto& self, size_t index) -> BplistValue {
            if (index >= num_objects || visiting[index]) return BplistValue(false);
            visiting[index] = true;
            size_t pos = static_cast<size_t>(offsets[index]);
            if (pos >= trailer) {
                visiting[index] = false;
                return BplistValue(false);
            }

            uint8_t marker = data[pos++];
            uint8_t high = marker >> 4;
            uint8_t low = marker & 0x0F;

            auto read_count = [&]() -> size_t {
                if (low != 0x0F) return low;
                if (pos >= trailer) return 0;
                uint8_t count_marker = data[pos++];
                size_t int_bytes = 1ULL << (count_marker & 0x0F);
                if (pos + int_bytes > trailer) return 0;
                size_t cnt = static_cast<size_t>(read_be(data + pos, int_bytes));
                pos += int_bytes;
                return cnt;
            };

            BplistValue result;
            switch (high) {
                case 0x0: {
                    if (low == 0x08) result = BplistValue(false);
                    else if (low == 0x09) result = BplistValue(true);
                    else result = BplistValue(false);
                    break;
                }
                case 0x1: { // Integer
                    size_t int_bytes = 1ULL << low;
                    if (pos + int_bytes <= trailer) {
                        uint64_t val = read_be(data + pos, int_bytes);
                        result = BplistValue(val);
                    }
                    break;
                }
                case 0x4: { // Data
                    size_t count = read_count();
                    if (pos + count <= trailer) {
                        result = BplistValue(std::vector<uint8_t>(data + pos, data + pos + count));
                    }
                    break;
                }
                case 0x5: { // ASCII String
                    size_t count = read_count();
                    if (pos + count <= trailer) {
                        result = BplistValue(std::string(reinterpret_cast<const char*>(data + pos), count));
                    }
                    break;
                }
                case 0x6: { // UTF-16BE String
                    size_t count = read_count();
                    std::string s;
                    for (size_t c = 0; c < count && pos + 2 <= trailer; ++c, pos += 2) {
                        uint16_t ch = (static_cast<uint16_t>(data[pos]) << 8) | data[pos + 1];
                        if (ch < 0x80) s.push_back(static_cast<char>(ch));
                        else s.push_back('?');
                    }
                    result = BplistValue(std::move(s));
                    break;
                }
                case 0xA: { // Array
                    size_t count = read_count();
                    BplistArray arr;
                    arr.reserve(count);
                    for (size_t i = 0; i < count && pos + ref_size <= trailer; ++i) {
                        size_t ref = static_cast<size_t>(read_be(data + pos, ref_size));
                        pos += ref_size;
                        arr.push_back(self(self, ref));
                    }
                    result = BplistValue(std::move(arr));
                    break;
                }
                case 0xD: { // Dict
                    size_t count = read_count();
                    std::vector<size_t> key_refs(count);
                    for (size_t i = 0; i < count && pos + ref_size <= trailer; ++i) {
                        key_refs[i] = static_cast<size_t>(read_be(data + pos, ref_size));
                        pos += ref_size;
                    }
                    std::vector<size_t> val_refs(count);
                    for (size_t i = 0; i < count && pos + ref_size <= trailer; ++i) {
                        val_refs[i] = static_cast<size_t>(read_be(data + pos, ref_size));
                        pos += ref_size;
                    }
                    BplistDict dict;
                    dict.reserve(count);
                    for (size_t i = 0; i < count; ++i) {
                        auto k = self(self, key_refs[i]).AsString();
                        auto v = self(self, val_refs[i]);
                        dict.emplace_back(std::move(k), std::move(v));
                    }
                    result = BplistValue(std::move(dict));
                    break;
                }
                default:
                    result = BplistValue(false);
                    break;
            }
            visiting[index] = false;
            return result;
        };

        return read_object(read_object, static_cast<size_t>(top_object));
    }
};

class BplistEncoder {
public:
    static std::vector<uint8_t> Encode(const BplistValue& root) {
        BplistEncoder enc;
        enc.Add(root);
        return enc.Build();
    }

private:
    struct ObjectNode {
        uint8_t marker{0};
        std::vector<uint8_t> raw_payload;
        std::vector<size_t> child_refs;
        bool is_container{false};
    };

    std::vector<ObjectNode> nodes_;

    size_t Add(const BplistValue& val) {
        size_t idx = nodes_.size();
        nodes_.emplace_back();

        switch (val.type) {
            case BplistValue::Type::Boolean: {
                nodes_[idx].marker = val.bool_val ? 0x09 : 0x08;
                nodes_[idx].is_container = false;
                break;
            }
            case BplistValue::Type::Integer: {
                int64_t v = val.int_val;
                if (v >= 0 && v <= 0xFF) {
                    nodes_[idx].marker = 0x10;
                    nodes_[idx].raw_payload.push_back(static_cast<uint8_t>(v));
                } else if (v >= 0 && v <= 0xFFFF) {
                    nodes_[idx].marker = 0x11;
                    nodes_[idx].raw_payload.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
                    nodes_[idx].raw_payload.push_back(static_cast<uint8_t>(v & 0xFF));
                } else if (v >= 0 && v <= 0xFFFFFFFFLL) {
                    nodes_[idx].marker = 0x12;
                    for (int i = 3; i >= 0; --i) {
                        nodes_[idx].raw_payload.push_back(static_cast<uint8_t>((v >> (i * 8)) & 0xFF));
                    }
                } else {
                    nodes_[idx].marker = 0x13;
                    for (int i = 7; i >= 0; --i) {
                        nodes_[idx].raw_payload.push_back(static_cast<uint8_t>((v >> (i * 8)) & 0xFF));
                    }
                }
                nodes_[idx].is_container = false;
                break;
            }
            case BplistValue::Type::String: {
                size_t sz = val.str_val.size();
                nodes_[idx].is_container = false;
                if (sz < 15) {
                    nodes_[idx].marker = static_cast<uint8_t>(0x50 | sz);
                } else {
                    nodes_[idx].marker = 0x5F;
                    if (sz <= 0xFF) {
                        nodes_[idx].raw_payload.push_back(0x10);
                        nodes_[idx].raw_payload.push_back(static_cast<uint8_t>(sz));
                    } else {
                        nodes_[idx].raw_payload.push_back(0x11);
                        nodes_[idx].raw_payload.push_back(static_cast<uint8_t>((sz >> 8) & 0xFF));
                        nodes_[idx].raw_payload.push_back(static_cast<uint8_t>(sz & 0xFF));
                    }
                }
                nodes_[idx].raw_payload.insert(nodes_[idx].raw_payload.end(),
                                              val.str_val.begin(), val.str_val.end());
                break;
            }
            case BplistValue::Type::Data: {
                size_t sz = val.data_val.size();
                nodes_[idx].is_container = false;
                if (sz < 15) {
                    nodes_[idx].marker = static_cast<uint8_t>(0x40 | sz);
                } else {
                    nodes_[idx].marker = 0x4F;
                    if (sz <= 0xFF) {
                        nodes_[idx].raw_payload.push_back(0x10);
                        nodes_[idx].raw_payload.push_back(static_cast<uint8_t>(sz));
                    } else {
                        nodes_[idx].raw_payload.push_back(0x11);
                        nodes_[idx].raw_payload.push_back(static_cast<uint8_t>((sz >> 8) & 0xFF));
                        nodes_[idx].raw_payload.push_back(static_cast<uint8_t>(sz & 0xFF));
                    }
                }
                nodes_[idx].raw_payload.insert(nodes_[idx].raw_payload.end(),
                                              val.data_val.begin(), val.data_val.end());
                break;
            }
            case BplistValue::Type::Array: {
                nodes_[idx].is_container = true;
                size_t sz = val.array_val.size();
                if (sz < 15) {
                    nodes_[idx].marker = static_cast<uint8_t>(0xA0 | sz);
                } else {
                    nodes_[idx].marker = 0xAF;
                    if (sz <= 0xFF) {
                        nodes_[idx].raw_payload.push_back(0x10);
                        nodes_[idx].raw_payload.push_back(static_cast<uint8_t>(sz));
                    } else {
                        nodes_[idx].raw_payload.push_back(0x11);
                        nodes_[idx].raw_payload.push_back(static_cast<uint8_t>((sz >> 8) & 0xFF));
                        nodes_[idx].raw_payload.push_back(static_cast<uint8_t>(sz & 0xFF));
                    }
                }
                std::vector<size_t> child_indices;
                child_indices.reserve(sz);
                for (const auto& item : val.array_val) {
                    child_indices.push_back(Add(item));
                }
                nodes_[idx].child_refs = std::move(child_indices);
                break;
            }
            case BplistValue::Type::Dict: {
                nodes_[idx].is_container = true;
                size_t sz = val.dict_val.size();
                if (sz < 15) {
                    nodes_[idx].marker = static_cast<uint8_t>(0xD0 | sz);
                } else {
                    nodes_[idx].marker = 0xDF;
                    if (sz <= 0xFF) {
                        nodes_[idx].raw_payload.push_back(0x10);
                        nodes_[idx].raw_payload.push_back(static_cast<uint8_t>(sz));
                    } else {
                        nodes_[idx].raw_payload.push_back(0x11);
                        nodes_[idx].raw_payload.push_back(static_cast<uint8_t>((sz >> 8) & 0xFF));
                        nodes_[idx].raw_payload.push_back(static_cast<uint8_t>(sz & 0xFF));
                    }
                }
                std::vector<size_t> all_refs;
                all_refs.reserve(sz * 2);
                for (const auto& kv : val.dict_val) {
                    all_refs.push_back(Add(BplistValue(kv.first)));
                }
                for (const auto& kv : val.dict_val) {
                    all_refs.push_back(Add(kv.second));
                }
                nodes_[idx].child_refs = std::move(all_refs);
                break;
            }
        }
        return idx;
    }

    std::vector<uint8_t> Build() {
        uint8_t ref_size = (nodes_.size() > 0xFF) ? 2 : 1;
        std::vector<uint8_t> out;
        const char magic[8] = {'b', 'p', 'l', 'i', 's', 't', '0', '0'};
        out.insert(out.end(), magic, magic + 8);

        std::vector<size_t> offsets;
        offsets.reserve(nodes_.size());

        for (const auto& node : nodes_) {
            offsets.push_back(out.size());
            out.push_back(node.marker);
            out.insert(out.end(), node.raw_payload.begin(), node.raw_payload.end());
            if (node.is_container) {
                for (size_t ref : node.child_refs) {
                    if (ref_size == 1) {
                        out.push_back(static_cast<uint8_t>(ref & 0xFF));
                    } else {
                        out.push_back(static_cast<uint8_t>((ref >> 8) & 0xFF));
                        out.push_back(static_cast<uint8_t>(ref & 0xFF));
                    }
                }
            }
        }

        size_t offset_table_offset = out.size();
        uint8_t offset_size = 1;
        if (offset_table_offset > 0xFFFF) {
            offset_size = 4;
        } else if (offset_table_offset > 0xFF) {
            offset_size = 2;
        }

        for (size_t off : offsets) {
            for (int i = offset_size - 1; i >= 0; --i) {
                out.push_back(static_cast<uint8_t>((off >> (i * 8)) & 0xFF));
            }
        }

        // 32-byte trailer
        uint8_t trailer[32] = {0};
        trailer[6] = offset_size;
        trailer[7] = ref_size;
        uint64_t num_obj = nodes_.size();
        for (int i = 0; i < 8; ++i) {
            trailer[8 + 7 - i] = static_cast<uint8_t>((num_obj >> (i * 8)) & 0xFF);
        }
        trailer[16 + 7] = 0; // top object is 0
        uint64_t off_table = offset_table_offset;
        for (int i = 0; i < 8; ++i) {
            trailer[24 + 7 - i] = static_cast<uint8_t>((off_table >> (i * 8)) & 0xFF);
        }

        out.insert(out.end(), trailer, trailer + 32);
        return out;
    }
};

} // namespace CarPlayStation::Core::Session::Pairing

