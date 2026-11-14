#include "pcm.h"
#include <cstring>
#include <stdexcept>
#include <algorithm>
#include <cmath>
namespace airplay {
void PcmConverter::Reset() {
    if (resampler_) soxr_delete(resampler_);
    resampler_ = nullptr;
    rate_ = 0;
}
std::vector<int16_t> PcmConverter::Convert(const uint8_t* bytes, int frames, int bits, int channels, int rate) {
    if (!bytes || frames <= 0 || frames > 262144 || (channels != 1 && channels != 2) ||
        (bits != 16 && bits != 24 && bits != 32) || rate < 8000 || rate > 384000)
        throw std::runtime_error("Unsupported PCM format");
    std::vector<float> input(static_cast<size_t>(frames) * 2);
    auto read = [&](size_t index) {
        auto* sample = bytes + index * (bits / 8);
        int32_t value = 0;
        if (bits == 16) { int16_t v; memcpy(&v, sample, 2); return static_cast<float>(v) / 32768.f; }
        if (bits == 24) {
            uint32_t v = sample[0] | (uint32_t(sample[1]) << 8) | (uint32_t(sample[2]) << 16);
            if (v & 0x800000) v |= 0xff000000;
            memcpy(&value, &v, 4);
            return static_cast<float>(value) / 8388608.f;
        }
        memcpy(&value, sample, 4);
        return static_cast<float>(value) / 2147483648.f;
    };
    for (int i = 0; i < frames; ++i) {
        input[i * 2] = read(static_cast<size_t>(i) * channels);
        input[i * 2 + 1] = channels == 1 ? input[i * 2] : read(static_cast<size_t>(i) * channels + 1);
    }
    if (rate == 44100) {
        Reset();
        std::vector<int16_t> output(input.size());
        for (size_t i = 0; i < input.size(); ++i)
            output[i] = static_cast<int16_t>(std::clamp(std::lround(input[i] * 32768.f), -32768L, 32767L));
        return output;
    }
    if (rate_ != rate) {
        Reset();
        auto io = soxr_io_spec(SOXR_FLOAT32_I, SOXR_INT16_I);
        auto quality = soxr_quality_spec(SOXR_LQ, 0); // small streaming delay, band-limited conversion
        soxr_error_t error = nullptr;
        resampler_ = soxr_create(rate, 44100, 2, &error, &io, &quality, nullptr);
        if (error || !resampler_) throw std::runtime_error("Could not create resampler");
        rate_ = rate;
    }
    std::vector<int16_t> result;
    size_t consumed = 0;
    while (consumed < static_cast<size_t>(frames)) {
        int16_t output[8192];
        size_t used = 0, produced = 0;
        auto error = soxr_process(resampler_, input.data() + consumed * 2, frames - consumed, &used,
                                  output, 4096, &produced);
        if (error) throw std::runtime_error(error);
        consumed += used;
        result.insert(result.end(), output, output + produced * 2);
        if (!used && !produced) throw std::runtime_error("Resampler stalled");
    }
    return result;
}
}
