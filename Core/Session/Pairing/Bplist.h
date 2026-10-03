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
    BplistValue(const char* s) : type(Type::String), str_val(s) {}
    BplistValue(std::string s) : type(Type::String), str_val(std::move(s)) {}
    BplistValue(std::vector<uint8_t> d) : type(Type::Data), data_val(std::move(d)) {}
    BplistValue(BplistArray a) : type(Type::Array), array_val(std::move(a)) {}
    BplistValue(BplistDict d) : type(Type::Dict), dict_val(std::move(d)) {}
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
                std::vector<size_t> key_indices;
                std::vector<size_t> val_indices;
                key_indices.reserve(sz);
                val_indices.reserve(sz);
                for (const auto& kv : val.dict_val) {
                    key_indices.push_back(Add(BplistValue(kv.first)));
                }
                for (const auto& kv : val.dict_val) {
                    val_indices.push_back(Add(kv.second));
                }
                nodes_[idx].child_refs = std::move(key_indices);
                nodes_[idx].child_refs.insert(nodes_[idx].child_refs.end(),
                                              val_indices.begin(), val_indices.end());
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

