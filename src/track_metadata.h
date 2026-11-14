#pragma once
#include <string>
#include <cstdint>
#include <vector>

namespace airplay {
struct TrackMetadata {
    std::wstring title, artist, album;
    std::vector<uint8_t> artwork;
    uint64_t streamStart = 0, detected = 0, coverConsumed = 0;
};
}
