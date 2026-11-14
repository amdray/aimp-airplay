#pragma once
#include <cstdint>
#include <vector>
#include <soxr.h>
namespace airplay {
class PcmConverter {
public:
    ~PcmConverter() { Reset(); }
    void Reset();
    std::vector<int16_t> Convert(const uint8_t* bytes, int frames, int bits, int channels, int rate);
private:
    soxr_t resampler_ = nullptr;
    int rate_ = 0;
};
}
