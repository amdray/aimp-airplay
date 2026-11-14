#include "pairing_plist.h"
#include <algorithm>
#include <cstring>
#include <stdexcept>
namespace airplay::pairing {
namespace {
void Require(bool valid, const char* error = "Некорректный ответ Apple TV") {
    if (!valid) throw std::runtime_error(error);
}
void Integer(Bytes& out, uint64_t n, unsigned width) {
    for (unsigned i = width; i; --i) out.push_back(static_cast<unsigned char>(n >> (8 * (i - 1))));
}
void Length(Bytes& out, unsigned type, size_t n) {
    out.push_back(static_cast<unsigned char>(type | std::min(n, size_t(15))));
    if (n >= 15) { out.push_back(0x12); Integer(out, n, 4); }
}
}
Bytes EncodePlist(const Dict& values, bool strings) {
    Bytes out{'b','p','l','i','s','t','0','0'};
    std::vector<size_t> offsets;
    for (const auto& entry : values) {
        offsets.push_back(out.size()); Length(out, 0x50, entry.first.size());
        out.insert(out.end(), entry.first.begin(), entry.first.end());
        offsets.push_back(out.size()); Length(out, strings ? 0x50 : 0x40, entry.second.size());
        out.insert(out.end(), entry.second.begin(), entry.second.end());
    }
    size_t top = offsets.size(); offsets.push_back(out.size()); Length(out, 0xd0, values.size());
    for (size_t i = 0; i < values.size(); ++i) out.push_back(static_cast<unsigned char>(i * 2));
    for (size_t i = 0; i < values.size(); ++i) out.push_back(static_cast<unsigned char>(i * 2 + 1));
    size_t table = out.size();
    for (size_t offset : offsets) Integer(out, offset, 4);
    out.insert(out.end(), 6, 0); out.push_back(4); out.push_back(1);
    Integer(out, offsets.size(), 8); Integer(out, top, 8); Integer(out, table, 8);
    return out;
}
Dict DecodePlist(const Bytes& bytes) {
    Require(bytes.size() >= 40 && bytes.size() <= 8192 && !memcmp(bytes.data(), "bplist00", 8));
    size_t end = bytes.size() - 32;
    auto number = [&](size_t offset, size_t width, size_t limit) -> uint64_t {
        Require(width && width <= 8 && offset <= limit && width <= limit - offset);
        uint64_t n = 0; for (size_t i = 0; i < width; ++i) n = (n << 8) | bytes[offset + i]; return n;
    };
    size_t ow = bytes[end + 6], rw = bytes[end + 7];
    uint64_t count = number(end + 8, 8, bytes.size()), top = number(end + 16, 8, bytes.size());
    uint64_t table = number(end + 24, 8, bytes.size());
    Require(ow && ow <= 8 && rw && rw <= 8 && count && count <= 64 && top < count && table >= 8 && table <= end && count <= (end - table) / ow);
    auto object = [&](uint64_t index) -> size_t {
        Require(index < count); auto offset = number(size_t(table + index * ow), ow, end);
        Require(offset >= 8 && offset < table); return size_t(offset);
    };
    auto length = [&](size_t& p, unsigned type) -> size_t {
        Require(p < table && (bytes[p] & 0xf0) == type);
        size_t n = bytes[p++] & 15;
        if (n == 15) {
            Require(p < table && (bytes[p] & 0xf0) == 0x10 && (bytes[p] & 15) <= 3);
            size_t width = size_t(1) << (bytes[p++] & 15);
            n = size_t(number(p, width, size_t(table))); p += width;
        }
        Require(n <= 8192); return n;
    };
    size_t p = object(top), n = length(p, 0xd0);
    Require(n <= 16 && n * 2 * rw <= table - p);
    Dict result;
    for (size_t i = 0; i < n; ++i) {
        size_t key = object(number(p + i * rw, rw, size_t(table)));
        size_t value = object(number(p + (n + i) * rw, rw, size_t(table)));
        size_t kl = length(key, 0x50), vl = length(value, 0x40);
        Require(kl <= table - key && vl <= table - value);
        std::string name(reinterpret_cast<const char*>(bytes.data() + key), kl);
        Require(result.emplace(name, Bytes(bytes.begin() + value, bytes.begin() + value + vl)).second);
    }
    return result;
}
}
