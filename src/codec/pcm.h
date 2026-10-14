// PCM utilities. The pipeline carries interleaved float32 in [-1, 1]; encoders take S16 or S32.
#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace bta {

enum class SampleFormat : uint8_t { S16, S24In32, S32, F32 };

size_t BytesPerSample(SampleFormat f);

// Float -> integer with rounding and saturation (no NaN propagation: NaN becomes 0).
void FloatToS16(std::span<const float> in, int16_t* out);
void FloatToS32(std::span<const float> in, int32_t* out);  // full 32-bit scale (LDAC S32)
void S16ToFloat(std::span<const int16_t> in, float* out);
void S32ToFloat(std::span<const int32_t> in, float* out);
void S24PackedToFloat(const uint8_t* in, size_t samples, float* out);

// Channel mapping on interleaved float: supports 1->2 (duplicate), N->2 (front L/R + downmix of center),
// 2->1 (average), N->N copy.
void RemapChannels(std::span<const float> in, int inCh, std::vector<float>& out, int outCh);

// Streaming windowed-sinc resampler (Kaiser window, 32 taps per phase, 256 phases, linear phase
// interpolation). Quality is adequate for 44.1 <-> 48 kHz family conversion (>90 dB stopband).
class Resampler {
public:
    Resampler(uint32_t inRate, uint32_t outRate, int channels);
    bool IsPassthrough() const { return in_ == out_; }
    // Appends output frames for the given input frames.
    void Process(std::span<const float> in, std::vector<float>& out);
    void Reset();
    uint32_t InRate() const { return in_; }
    uint32_t OutRate() const { return out_; }

private:
    uint32_t in_, out_;
    int ch_;
    double step_;   // input samples per output sample
    double pos_ = 0;  // position in history buffer (fractional input frame)
    std::vector<float> hist_;  // interleaved history
    std::vector<float> table_;  // [phase][tap]
    static constexpr int kTaps = 32;
    static constexpr int kPhases = 256;
};

}  // namespace bta
