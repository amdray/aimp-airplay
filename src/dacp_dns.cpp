#include "dacp_dns.h"
#include <algorithm>
#include <cctype>
namespace airplay::dacp {
namespace {
void Short(Bytes& bytes, unsigned n) { bytes.push_back(static_cast<unsigned char>(n >> 8)); bytes.push_back(static_cast<unsigned char>(n)); }
void Name(Bytes& bytes, const std::string& name) {
    size_t start = 0;
    while (start < name.size()) {
        auto end = name.find('.', start); if (end == std::string::npos) end = name.size();
        bytes.push_back(static_cast<unsigned char>(end - start)); bytes.insert(bytes.end(), name.begin() + start, name.begin() + end); start = end + 1;
    }
    bytes.push_back(0);
}
void Record(Bytes& bytes, const std::string& name, unsigned type, const Bytes& data) {
    Name(bytes, name); Short(bytes, type); Short(bytes, type == 12 ? 1 : 0x8001);
    bytes.insert(bytes.end(), {0, 0, 0, 120}); Short(bytes, unsigned(data.size())); bytes.insert(bytes.end(), data.begin(), data.end());
}
}
Bytes Advertisement(const std::string& id, unsigned short port, in_addr local) {
    std::string type = "_dacp._tcp.local", instance = "iTunes_Ctrl_" + id + "." + type, host = "aimp-airplay-" + id + ".local";
    Bytes bytes{0,0,0x84,0,0,0,0,5,0,0,0,0}, data;
    Name(data, instance); Record(bytes, type, 12, data); data.clear();
    Short(data, 0); Short(data, 0); Short(data, port); Name(data, host); Record(bytes, instance, 33, data); data.clear();
    for (const auto& text : {std::string("txtvers=1"), std::string("Ver=131075"), std::string("DbId=") + id, std::string("OSsi=0x1F5")}) {
        data.push_back(static_cast<unsigned char>(text.size())); data.insert(data.end(), text.begin(), text.end());
    }
    Record(bytes, instance, 16, data); data.assign(reinterpret_cast<unsigned char*>(&local), reinterpret_cast<unsigned char*>(&local) + 4);
    auto hostOffset = bytes.size();
    Record(bytes, host, 1, data);
    // RFC 6762 section 6.1: this DACP endpoint has an A record, no AAAA.
    // NSEC next-name points to the A owner; window 0 lists only type A.
    data.clear(); Short(data, 0xc000 | unsigned(hostOffset));
    data.insert(data.end(), {0, 1, 0x40});
    Record(bytes, host, 47, data);
    return bytes;
}
bool RelevantQuery(const unsigned char* bytes, size_t size, const std::string& id, bool* unicast) {
    if (size < 12 || (bytes[2] & 0x80)) return false;
    unsigned count = unsigned(bytes[4]) * 256 + bytes[5]; if (!count || count > 16) return false;
    std::string instance = "itunes_ctrl_" + id + "._dacp._tcp.local", host = "aimp-airplay-" + id + ".local";
    std::transform(instance.begin(), instance.end(), instance.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    std::transform(host.begin(), host.end(), host.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    size_t p = 12;
    for (unsigned i = 0; i < count; ++i) {
        std::string name; size_t cursor = p, resume = 0; unsigned steps = 0;
        for (;;) {
            if (cursor >= size || ++steps > 32) return false;
            unsigned n = bytes[cursor++];
            if ((n & 0xc0) == 0xc0) {
                if (cursor >= size) return false;
                if (!resume) resume = cursor + 1;
                cursor = ((n & 63) << 8) | bytes[cursor]; continue;
            }
            if (n > 63 || n > size - cursor) return false;
            if (!n) break;
            if (!name.empty()) name += '.';
            name.append(reinterpret_cast<const char*>(bytes + cursor), n); cursor += n;
            if (name.size() > 255) return false;
        }
        p = resume ? resume : cursor;
        if (p > size || size - p < 4) return false;
        unsigned type = unsigned(bytes[p]) * 256 + bytes[p + 1];
        unsigned queryClass = unsigned(bytes[p + 2]) * 256 + bytes[p + 3]; p += 4;
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        if ((name == "_dacp._tcp.local" && (type == 12 || type == 255)) || (name == instance && (type == 33 || type == 16 || type == 255)) || (name == host && (type == 1 || type == 28 || type == 255))) {
            if (unicast) *unicast = (queryClass & 0x8000) != 0;
            return true;
        }
    }
    return false;
}
}
