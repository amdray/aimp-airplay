#pragma once
#include "apiAlbumArt.h"
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace airplay::aimp {
struct CoverRequest {
    IAIMPCore* core;
    std::vector<uint8_t> bytes;
    std::wstring trackKey;
    std::atomic<bool> completed{false};
};
void CALLBACK ReceiveCoverAsync(IAIMPImage* image, IAIMPImageContainer* container, void* user);
}
