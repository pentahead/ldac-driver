// Codec encoder interface. Encoders emit complete media payloads (codec frames that fit one
// A2DP media packet); the pipeline adds the RTP + payload header.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "a2dp/codec_caps.h"
#include "codec/pcm.h"
#include "common/status.h"

namespace bta {

struct EncodedPayload {
    std::vector<uint8_t> bytes;  // concatenated codec frames
    uint8_t frames = 0;          // 1..15
    uint32_t pcmFrames = 0;      // PCM frames (per channel) represented
};

class IEncoder {
public:
    virtual ~IEncoder() = default;
    virtual CodecId Codec() const = 0;
    // l2capMtu: outgoing MTU of the media channel (RTP and payload header are subtracted internally).
    virtual Status Open(const a2dp::StreamConfig& cfg, uint16_t l2capMtu) = 0;
    virtual void Close() = 0;
    virtual SampleFormat InputFormat() const = 0;
    virtual int Channels() const = 0;
    virtual uint32_t InputFramesPerCall() const = 0;
    // pcm holds exactly InputFramesPerCall() interleaved frames in InputFormat().
    virtual Status Encode(const void* pcm, std::vector<EncodedPayload>& out) = 0;
    // Emits any partially filled payload (used on stop/suspend).
    virtual void Flush(std::vector<EncodedPayload>& out) { (void)out; }
    // Feedback for adaptive bitrate: called periodically with the transmit queue depth (packets).
    virtual void OnTransmitFeedback(uint32_t queueDepth) { (void)queueDepth; }
    virtual uint32_t NominalBitrate() const = 0;  // bit/s at the current quality
    virtual std::string QualityLabel() const = 0;
    // Live quality change (LDAC EQMID, SBC bitpool). Returns NotSupported if a reconfiguration is needed.
    virtual Status SetQuality(int level) = 0;
};

using EncoderFactory = std::function<std::unique_ptr<IEncoder>(CodecId)>;

std::unique_ptr<IEncoder> CreateEncoder(CodecId codec);
// True if the encoder can be created and initialised in this build/environment.
bool IsEncoderAvailable(CodecId codec, std::string* reason = nullptr);

std::unique_ptr<IEncoder> MakeSbcEncoder();
std::unique_ptr<IEncoder> MakeLdacEncoder();

}  // namespace bta
