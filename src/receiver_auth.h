#pragma once
#include "receivers.h"
#include <cstdint>

namespace airplay {
inline uint64_t ReceiverFlags(const std::string& text) {
    uint64_t parts[2]{};
    size_t p = 0;
    for (unsigned part = 0; part < 2; ++part) {
        if (text.compare(p, 2, "0x") == 0 || text.compare(p, 2, "0X") == 0) p += 2;
        unsigned digits = 0;
        while (p < text.size() && text[p] != ',') {
            unsigned char c = text[p++];
            unsigned n = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 16;
            if (n == 16 || ++digits > 8) return 0;
            parts[part] = (parts[part] << 4) | n;
        }
        if (!digits) return 0;
        if (p == text.size()) return parts[0] | (parts[1] << 32);
        if (part == 1) return 0;
        ++p;
    }
    return 0;
}
inline bool ReceiverNeedsPin(const Receiver& receiver) {
    return (ReceiverFlags(receiver.pairingFlags) & 0x208) != 0;
}
inline bool ReceiverUsesTransientPairing(const Receiver& receiver) {
    auto features = ReceiverFlags(receiver.features);
    return receiver.pairingSecret.empty() && !ReceiverNeedsPin(receiver) &&
           (features & ((uint64_t(1) << 43) | (uint64_t(1) << 48))) != 0;
}
}
