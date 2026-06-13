#pragma once
// Minimal CBOR (RFC 7049) encoder/decoder for IPC frame payloads.
// Supports: unsigned int, byte string, text string, bool, null, array, map.
// Not a general-purpose CBOR implementation; covers exactly the RPC surface.

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace cbor {

// ---------- Encoder helpers ----------

inline void encode_head(std::vector<uint8_t>& out, uint8_t major, uint64_t n) {
    major = static_cast<uint8_t>(major << 5u);
    if (n <= 23) {
        out.push_back(major | static_cast<uint8_t>(n));
    } else if (n <= 0xFFu) {
        out.push_back(major | 24u); out.push_back(static_cast<uint8_t>(n));
    } else if (n <= 0xFFFFu) {
        out.push_back(major | 25u);
        out.push_back(static_cast<uint8_t>(n >> 8)); out.push_back(static_cast<uint8_t>(n));
    } else if (n <= 0xFFFFFFFFu) {
        out.push_back(major | 26u);
        for (int i = 3; i >= 0; --i) out.push_back(static_cast<uint8_t>(n >> (8*i)));
    } else {
        out.push_back(major | 27u);
        for (int i = 7; i >= 0; --i) out.push_back(static_cast<uint8_t>(n >> (8*i)));
    }
}

inline void enc_uint(std::vector<uint8_t>& o, uint64_t v)              { encode_head(o, 0, v); }
inline void enc_bool(std::vector<uint8_t>& o, bool v)                  { o.push_back(v ? 0xF5u : 0xF4u); }
inline void enc_null(std::vector<uint8_t>& o)                          { o.push_back(0xF6u); }
inline void enc_text(std::vector<uint8_t>& o, std::string_view v) {
    encode_head(o, 3, v.size());
    o.insert(o.end(), reinterpret_cast<const uint8_t*>(v.data()),
                      reinterpret_cast<const uint8_t*>(v.data()) + v.size());
}
inline void enc_bytes(std::vector<uint8_t>& o, std::span<const uint8_t> v) {
    encode_head(o, 2, v.size());
    o.insert(o.end(), v.begin(), v.end());
}
inline void enc_bytes(std::vector<uint8_t>& o, std::span<const std::byte> v) {
    enc_bytes(o, std::span<const uint8_t>(
        reinterpret_cast<const uint8_t*>(v.data()), v.size()));
}

// ---------- Decoder ----------

struct Value;
using Array = std::vector<Value>;
using Map   = std::vector<std::pair<Value, Value>>;

struct Value {
    using Inner = std::variant<
        uint64_t,
        std::vector<uint8_t>,
        std::string,
        bool,
        std::monostate,
        Array,
        Map
    >;
    Inner inner;

    bool is_uint()  const { return std::holds_alternative<uint64_t>(inner); }
    bool is_bytes() const { return std::holds_alternative<std::vector<uint8_t>>(inner); }
    bool is_text()  const { return std::holds_alternative<std::string>(inner); }
    bool is_bool()  const { return std::holds_alternative<bool>(inner); }
    bool is_null()  const { return std::holds_alternative<std::monostate>(inner); }
    bool is_array() const { return std::holds_alternative<Array>(inner); }
    bool is_map()   const { return std::holds_alternative<Map>(inner); }

    uint64_t                        as_uint()  const { return std::get<uint64_t>(inner); }
    const std::vector<uint8_t>&     as_bytes() const { return std::get<std::vector<uint8_t>>(inner); }
    const std::string&              as_text()  const { return std::get<std::string>(inner); }
    bool                            as_bool()  const { return std::get<bool>(inner); }
    const Array&                    as_array() const { return std::get<Array>(inner); }
    const Map&                      as_map()   const { return std::get<Map>(inner); }
};

namespace detail {
inline uint64_t read_be(const uint8_t* p, int n) {
    uint64_t v = 0;
    for (int i = 0; i < n; ++i) v = (v << 8u) | p[i];
    return v;
}
inline Value decode_one(const uint8_t* d, size_t len, size_t& pos) {
    if (pos >= len) throw std::runtime_error("cbor: underrun");
    uint8_t ib = d[pos++];
    uint8_t mt = ib >> 5u, ai = ib & 0x1Fu;
    uint64_t arg;
    if      (ai <= 23) arg = ai;
    else if (ai == 24) { if (pos+1 > len) throw std::runtime_error("cbor: underrun"); arg = d[pos++]; }
    else if (ai == 25) { if (pos+2 > len) throw std::runtime_error("cbor: underrun"); arg = read_be(d+pos,2); pos+=2; }
    else if (ai == 26) { if (pos+4 > len) throw std::runtime_error("cbor: underrun"); arg = read_be(d+pos,4); pos+=4; }
    else if (ai == 27) { if (pos+8 > len) throw std::runtime_error("cbor: underrun"); arg = read_be(d+pos,8); pos+=8; }
    else               arg = ai;
    switch (mt) {
    case 0: return Value{arg};
    case 2: { if (pos+arg > len) throw std::runtime_error("cbor: bytes overrun");
              std::vector<uint8_t> b(d+pos, d+pos+arg); pos += arg; return Value{std::move(b)}; }
    case 3: { if (pos+arg > len) throw std::runtime_error("cbor: text overrun");
              std::string s(reinterpret_cast<const char*>(d+pos), arg); pos += arg; return Value{std::move(s)}; }
    case 4: { Array arr; for (uint64_t i=0;i<arg;++i) arr.push_back(decode_one(d,len,pos)); return Value{std::move(arr)}; }
    case 5: { Map m; for (uint64_t i=0;i<arg;++i) {
                  auto k=decode_one(d,len,pos); auto v=decode_one(d,len,pos);
                  m.emplace_back(std::move(k),std::move(v)); } return Value{std::move(m)}; }
    case 7:
        if (ib == 0xF4u) return Value{false};
        if (ib == 0xF5u) return Value{true};
        if (ib == 0xF6u) return Value{std::monostate{}};
        throw std::runtime_error("cbor: unsupported simple");
    default: throw std::runtime_error("cbor: unsupported major type");
    }
}
} // namespace detail

inline Value decode(std::span<const uint8_t> data) {
    size_t pos = 0;
    return detail::decode_one(data.data(), data.size(), pos);
}
inline Value decode(std::span<const std::byte> data) {
    return decode(std::span<const uint8_t>(
        reinterpret_cast<const uint8_t*>(data.data()), data.size()));
}

// Look up a key in a decoded CBOR map; returns nullptr if missing.
inline const Value* map_get(const Map& m, std::string_view key) {
    for (const auto& [k, v] : m)
        if (k.is_text() && k.as_text() == key) return &v;
    return nullptr;
}

// Fluent map builder for encoding outgoing frames.
class MapBuilder {
    std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>> e_;
    static std::vector<uint8_t> key(std::string_view s) {
        std::vector<uint8_t> b; enc_text(b, s); return b;
    }
public:
    MapBuilder& add_uint (std::string_view k, uint64_t v)
        { std::vector<uint8_t> b; enc_uint(b,v);  e_.emplace_back(key(k),std::move(b)); return *this; }
    MapBuilder& add_text (std::string_view k, std::string_view v)
        { std::vector<uint8_t> b; enc_text(b,v);  e_.emplace_back(key(k),std::move(b)); return *this; }
    MapBuilder& add_bytes(std::string_view k, std::span<const uint8_t> v)
        { std::vector<uint8_t> b; enc_bytes(b,v); e_.emplace_back(key(k),std::move(b)); return *this; }
    MapBuilder& add_bytes(std::string_view k, std::span<const std::byte> v)
        { std::vector<uint8_t> b; enc_bytes(b,v); e_.emplace_back(key(k),std::move(b)); return *this; }
    MapBuilder& add_bool (std::string_view k, bool v)
        { std::vector<uint8_t> b; enc_bool(b,v);  e_.emplace_back(key(k),std::move(b)); return *this; }

    std::vector<uint8_t> build() const {
        std::vector<uint8_t> out;
        encode_head(out, 5, e_.size());
        for (const auto& [k, v] : e_) {
            out.insert(out.end(), k.begin(), k.end());
            out.insert(out.end(), v.begin(), v.end());
        }
        return out;
    }
};

} // namespace cbor
