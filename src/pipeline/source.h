// PCM sources feeding the stream engine. All deliver interleaved float32.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include "common/status.h"
#include "pipeline/pcm_ring.h"

namespace bta {

struct PcmFormatDesc {
    uint32_t sampleRate = 48000;
    int channels = 2;
};

class IPcmSource {
public:
    virtual ~IPcmSource() = default;
    virtual std::string Name() const = 0;
    virtual Status Start() = 0;
    virtual void Stop() = 0;
    virtual PcmFormatDesc Format() const = 0;
    // Real-time sources produce data at the device clock; non-real-time sources produce on demand.
    virtual bool IsRealtime() const = 0;
    virtual size_t Available() const = 0;
    virtual size_t Read(float* dst, size_t frames) = 0;  // non-blocking
    virtual Status Health() const { return Status::Ok(); }  // reports device loss etc.
};

// Sine generator (diagnostics / "tone" source). Non-real-time: the engine paces it.
class ToneSource : public IPcmSource {
public:
    ToneSource(uint32_t rate, int channels, double freqHz = 1000.0, double amplitude = 0.25);
    std::string Name() const override { return "tone"; }
    Status Start() override { return Status::Ok(); }
    void Stop() override {}
    PcmFormatDesc Format() const override { return {rate_, ch_}; }
    bool IsRealtime() const override { return false; }
    size_t Available() const override { return SIZE_MAX / 4; }
    size_t Read(float* dst, size_t frames) override;

private:
    uint32_t rate_;
    int ch_;
    double freq_, amp_, phase_ = 0;
};

// Real-time source fed by an external producer through a PcmRing (tests, driver bridge).
class RingSource : public IPcmSource {
public:
    RingSource(PcmFormatDesc fmt, size_t capacityFrames) : fmt_(fmt), ring_(capacityFrames, fmt.channels) {}
    std::string Name() const override { return "ring"; }
    Status Start() override { return Status::Ok(); }
    void Stop() override {}
    PcmFormatDesc Format() const override { return fmt_; }
    bool IsRealtime() const override { return true; }
    size_t Available() const override { return ring_.Available(); }
    size_t Read(float* dst, size_t frames) override { return ring_.Read(dst, frames); }
    PcmRing& Ring() { return ring_; }

private:
    PcmFormatDesc fmt_;
    PcmRing ring_;
};

}  // namespace bta
