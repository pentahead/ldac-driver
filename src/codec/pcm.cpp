#include "codec/pcm.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace bta {

size_t BytesPerSample(SampleFormat f)
{
    switch (f) {
    case SampleFormat::S16: return 2;
    case SampleFormat::S24In32:
    case SampleFormat::S32:
    case SampleFormat::F32: return 4;
    }
    return 0;
}

void FloatToS16(std::span<const float> in, int16_t* out)
{
    for (size_t i = 0; i < in.size(); ++i) {
        float v = in[i];
        if (!(v == v)) v = 0;  // NaN
        double s = std::nearbyint(static_cast<double>(v) * 32768.0);
        out[i] = static_cast<int16_t>(std::clamp(s, -32768.0, 32767.0));
    }
}

void FloatToS32(std::span<const float> in, int32_t* out)
{
    for (size_t i = 0; i < in.size(); ++i) {
        float v = in[i];
        if (!(v == v)) v = 0;
        double s = std::nearbyint(static_cast<double>(v) * 2147483648.0);
        out[i] = static_cast<int32_t>(std::clamp(s, -2147483648.0, 2147483647.0));
    }
}

void S16ToFloat(std::span<const int16_t> in, float* out)
{
    for (size_t i = 0; i < in.size(); ++i) out[i] = in[i] / 32768.0f;
}

void S32ToFloat(std::span<const int32_t> in, float* out)
{
    for (size_t i = 0; i < in.size(); ++i) out[i] = static_cast<float>(in[i] / 2147483648.0);
}

void S24PackedToFloat(const uint8_t* in, size_t samples, float* out)
{
    for (size_t i = 0; i < samples; ++i) {
        int32_t v = (in[3 * i] << 8) | (in[3 * i + 1] << 16) | (in[3 * i + 2] << 24);
        out[i] = static_cast<float>(v / 2147483648.0);
    }
}

void RemapChannels(std::span<const float> in, int inCh, std::vector<float>& out, int outCh)
{
    size_t frames = in.size() / static_cast<size_t>(inCh);
    out.resize(frames * static_cast<size_t>(outCh));
    if (inCh == outCh) {
        std::copy(in.begin(), in.begin() + static_cast<ptrdiff_t>(frames * inCh), out.begin());
        return;
    }
    for (size_t f = 0; f < frames; ++f) {
        const float* s = in.data() + f * inCh;
        float* d = out.data() + f * outCh;
        if (outCh == 2) {
            if (inCh == 1) {
                d[0] = d[1] = s[0];
            } else {
                // WAVE channel order: FL, FR, FC, LFE, BL, BR, ... Mix centre at -3 dB.
                float c = inCh > 2 ? s[2] * 0.7071f : 0.0f;
                float l = s[0] + c, r = s[1] + c;
                if (inCh >= 6) {
                    l += s[4] * 0.7071f;
                    r += s[5] * 0.7071f;
                }
                d[0] = l;
                d[1] = r;
            }
        } else if (outCh == 1) {
            float acc = 0;
            int n = std::min(inCh, 2);
            for (int c = 0; c < n; ++c) acc += s[c];
            d[0] = acc / static_cast<float>(n);
        } else {
            for (int c = 0; c < outCh; ++c) d[c] = c < inCh ? s[c] : 0.0f;
        }
    }
}

// ---------------------------------------------------------------------------
static double BesselI0(double x)
{
    double sum = 1, term = 1;
    for (int k = 1; k < 50; ++k) {
        term *= (x / (2 * k)) * (x / (2 * k));
        sum += term;
        if (term < 1e-12 * sum) break;
    }
    return sum;
}

Resampler::Resampler(uint32_t inRate, uint32_t outRate, int channels)
    : in_(inRate), out_(outRate), ch_(channels), step_(static_cast<double>(inRate) / outRate)
{
    // Cutoff at the lower Nyquist with 8% transition guard.
    double cutoff = 0.92 * std::min(1.0, static_cast<double>(outRate) / inRate);
    const double beta = 9.0;
    table_.resize(static_cast<size_t>(kPhases + 1) * kTaps);
    for (int ph = 0; ph <= kPhases; ++ph) {
        double frac = static_cast<double>(ph) / kPhases;
        double sum = 0;
        for (int t = 0; t < kTaps; ++t) {
            double x = (t - (kTaps / 2 - 1)) - frac;  // distance from interpolation point
            double sinc = x == 0 ? 1.0 : std::sin(std::numbers::pi * cutoff * x) / (std::numbers::pi * cutoff * x);
            double w = x / (kTaps / 2.0);
            double win = std::fabs(w) >= 1 ? 0 : BesselI0(beta * std::sqrt(1 - w * w)) / BesselI0(beta);
            double v = cutoff * sinc * win;
            table_[static_cast<size_t>(ph) * kTaps + t] = static_cast<float>(v);
            sum += v;
        }
        for (int t = 0; t < kTaps; ++t) table_[static_cast<size_t>(ph) * kTaps + t] /= static_cast<float>(sum);
    }
    Reset();
}

void Resampler::Reset()
{
    hist_.assign(static_cast<size_t>(kTaps) * ch_, 0.0f);
    pos_ = kTaps / 2 - 1;
}

void Resampler::Process(std::span<const float> in, std::vector<float>& out)
{
    if (IsPassthrough()) {
        out.insert(out.end(), in.begin(), in.end());
        return;
    }
    hist_.insert(hist_.end(), in.begin(), in.end());
    const size_t frames = hist_.size() / ch_;
    // Need taps/2 frames of look-ahead beyond pos.
    while (pos_ + kTaps / 2 + 1 < frames) {
        int base = static_cast<int>(std::floor(pos_));
        double frac = pos_ - base;
        double phf = frac * kPhases;
        int ph = static_cast<int>(phf);
        float a = static_cast<float>(phf - ph);
        const float* c0 = &table_[static_cast<size_t>(ph) * kTaps];
        const float* c1 = &table_[static_cast<size_t>(ph + 1) * kTaps];
        int first = base - (kTaps / 2 - 1);
        for (int c = 0; c < ch_; ++c) {
            float acc = 0;
            for (int t = 0; t < kTaps; ++t) {
                int idx = first + t;
                float s = idx >= 0 ? hist_[static_cast<size_t>(idx) * ch_ + c] : 0.0f;
                acc += s * (c0[t] + a * (c1[t] - c0[t]));
            }
            out.push_back(acc);
        }
        pos_ += step_;
    }
    // Drop consumed history, keep kTaps frames before pos.
    int keepFrom = static_cast<int>(std::floor(pos_)) - kTaps;
    if (keepFrom > 0) {
        hist_.erase(hist_.begin(), hist_.begin() + static_cast<ptrdiff_t>(keepFrom) * ch_);
        pos_ -= keepFrom;
    }
}

}  // namespace bta
