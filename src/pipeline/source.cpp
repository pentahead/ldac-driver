#include "pipeline/source.h"

#include <cmath>
#include <numbers>

namespace bta {

ToneSource::ToneSource(uint32_t rate, int channels, double freqHz, double amplitude)
    : rate_(rate), ch_(channels), freq_(freqHz), amp_(amplitude) {}

size_t ToneSource::Read(float* dst, size_t frames)
{
    const double inc = 2 * std::numbers::pi * freq_ / rate_;
    for (size_t f = 0; f < frames; ++f) {
        float v = static_cast<float>(amp_ * std::sin(phase_));
        for (int c = 0; c < ch_; ++c) dst[f * ch_ + c] = v;
        phase_ += inc;
        if (phase_ > 2 * std::numbers::pi) phase_ -= 2 * std::numbers::pi;
    }
    return frames;
}

}  // namespace bta
