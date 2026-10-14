// SBC encoder backed by Google libsbc (Apache-2.0).
#include <sbc.h>

#include <format>

#include "a2dp/media.h"
#include "codec/encoder.h"
#include "common/log.h"

namespace bta {

namespace {

class SbcEncoder : public IEncoder {
public:
    CodecId Codec() const override { return CodecId::Sbc; }

    Status Open(const a2dp::StreamConfig& cfg, uint16_t l2capMtu) override
    {
        using namespace a2dp::sbcbits;
        if (cfg.codec != CodecId::Sbc) return {Err::InvalidArgument, "not an SBC configuration"};
        frame_ = {};
        frame_.msbc = false;
        switch (cfg.sbc.freq) {
        case kFreq16000: frame_.freq = SBC_FREQ_16K; break;
        case kFreq32000: frame_.freq = SBC_FREQ_32K; break;
        case kFreq44100: frame_.freq = SBC_FREQ_44K1; break;
        case kFreq48000: frame_.freq = SBC_FREQ_48K; break;
        default: return {Err::InvalidArgument, "bad SBC sampling frequency"};
        }
        switch (cfg.sbc.channelModes) {
        case kMono: frame_.mode = SBC_MODE_MONO; break;
        case kDual: frame_.mode = SBC_MODE_DUAL_CHANNEL; break;
        case kStereo: frame_.mode = SBC_MODE_STEREO; break;
        case kJoint: frame_.mode = SBC_MODE_JOINT_STEREO; break;
        default: return {Err::InvalidArgument, "bad SBC channel mode"};
        }
        frame_.nblocks = cfg.sbc.blocks == kBlocks4 ? 4 : cfg.sbc.blocks == kBlocks8 ? 8 : cfg.sbc.blocks == kBlocks12 ? 12 : 16;
        frame_.nsubbands = cfg.sbc.subbands == kSubbands4 ? 4 : 8;
        frame_.bam = cfg.sbc.allocation == kAllocLoudness ? SBC_BAM_LOUDNESS : SBC_BAM_SNR;
        minBitpool_ = cfg.sbc.minBitpool;
        maxBitpool_ = cfg.sbc.maxBitpool;
        frame_.bitpool = maxBitpool_;
        channels_ = frame_.mode == SBC_MODE_MONO ? 1 : 2;
        if (l2capMtu <= a2dp::kMediaOverhead) return {Err::InvalidArgument, "media MTU too small"};
        maxPayload_ = l2capMtu - a2dp::kMediaOverhead;
        if (sbc_get_frame_size(&frame_) > maxPayload_) return {Err::InvalidArgument, "SBC frame exceeds media MTU"};
        sbc_reset(&state_);
        pending_ = {};
        open_ = true;
        BTA_LOGI("sbc", "encoder open: {} frame={} B bitrate={} kbps payload<={} B", cfg.Describe(),
                 sbc_get_frame_size(&frame_), sbc_get_frame_bitrate(&frame_) / 1000, maxPayload_);
        return Status::Ok();
    }

    void Close() override { open_ = false; }
    SampleFormat InputFormat() const override { return SampleFormat::S16; }
    int Channels() const override { return channels_; }
    uint32_t InputFramesPerCall() const override { return static_cast<uint32_t>(frame_.nblocks * frame_.nsubbands); }

    Status Encode(const void* pcm, std::vector<EncodedPayload>& out) override
    {
        if (!open_) return {Err::StateError, "SBC encoder not open"};
        const auto* s = static_cast<const int16_t*>(pcm);
        unsigned size = sbc_get_frame_size(&frame_);
        uint8_t buf[512];
        if (size > sizeof buf) return {Err::EncoderError, "SBC frame too large"};
        int rc = sbc_encode(&state_, s, channels_, channels_ == 2 ? s + 1 : nullptr, channels_, &frame_, buf, size);
        if (rc != 0) return {Err::EncoderError, std::format("sbc_encode failed ({})", rc)};
        if (pending_.frames > 0 &&
            (pending_.bytes.size() + size > maxPayload_ || pending_.frames >= a2dp::kMaxFramesPerPacket ||
             frame_.bitpool != pendingBitpool_)) {
            out.push_back(std::move(pending_));
            pending_ = {};
        }
        pending_.bytes.insert(pending_.bytes.end(), buf, buf + size);
        pending_.frames++;
        pending_.pcmFrames += InputFramesPerCall();
        pendingBitpool_ = frame_.bitpool;
        // Emit when another frame would not fit (keeps latency at one packet).
        if (pending_.bytes.size() + size > maxPayload_ || pending_.frames >= a2dp::kMaxFramesPerPacket) {
            out.push_back(std::move(pending_));
            pending_ = {};
        }
        return Status::Ok();
    }

    void Flush(std::vector<EncodedPayload>& out) override
    {
        if (pending_.frames) out.push_back(std::move(pending_));
        pending_ = {};
    }

    uint32_t NominalBitrate() const override { return sbc_get_frame_bitrate(&frame_); }
    std::string QualityLabel() const override { return std::format("bitpool {}", frame_.bitpool); }

    Status SetQuality(int bitpool) override
    {
        if (bitpool < minBitpool_ || bitpool > maxBitpool_)
            return {Err::InvalidArgument, std::format("bitpool must be within configured {}..{}", minBitpool_, maxBitpool_)};
        sbc_frame trial = frame_;
        trial.bitpool = bitpool;
        if (sbc_get_frame_size(&trial) > maxPayload_) return {Err::InvalidArgument, "frame would exceed MTU"};
        frame_.bitpool = bitpool;  // bitpool may change per frame (A2DP sec. 4.3.2)
        return Status::Ok();
    }

    // Simple adaptive bitpool: step down when the transmit queue builds up, recover slowly.
    void OnTransmitFeedback(uint32_t depth) override
    {
        if (!adaptive_) return;
        if (depth >= 6 && frame_.bitpool > minBitpool_ + 4) {
            SetQuality(std::max<int>(minBitpool_, frame_.bitpool - 4));
            goodTicks_ = 0;
        } else if (depth <= 1 && frame_.bitpool < maxBitpool_ && ++goodTicks_ > 50) {
            SetQuality(frame_.bitpool + 1);
            goodTicks_ = 0;
        }
    }

private:
    sbc_t state_{};
    sbc_frame frame_{};
    int channels_ = 2;
    int minBitpool_ = 2, maxBitpool_ = 53;
    size_t maxPayload_ = 0;
    bool open_ = false;
    bool adaptive_ = true;
    int goodTicks_ = 0;
    EncodedPayload pending_;
    int pendingBitpool_ = 0;
};

}  // namespace

std::unique_ptr<IEncoder> MakeSbcEncoder() { return std::make_unique<SbcEncoder>(); }

}  // namespace bta
