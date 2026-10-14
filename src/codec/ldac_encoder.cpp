// LDAC encoder backed by Sony libldac from AOSP (Apache-2.0) plus the AOSP LDAC ABR helper.
// NOTE: per libldac NOTICE, shipping LDAC in a product requires Sony's certification process.
#include <ldacBT.h>
#include <ldacBT_abr.h>

#include <format>

#include "a2dp/media.h"
#include "codec/encoder.h"
#include "common/log.h"

namespace bta {

namespace {

constexpr uint32_t kAbrIntervalMs = 20;

class LdacEncoder : public IEncoder {
public:
    ~LdacEncoder() override { Close(); }
    CodecId Codec() const override { return CodecId::Ldac; }

    Status Open(const a2dp::StreamConfig& cfg, uint16_t l2capMtu) override
    {
        using namespace a2dp::ldacbits;
        Close();
        if (cfg.codec != CodecId::Ldac) return {Err::InvalidArgument, "not an LDAC configuration"};
        int cm;
        switch (cfg.ldac.channelModes) {
        case kStereo: cm = LDACBT_CHANNEL_MODE_STEREO; channels_ = 2; break;
        case kDual: cm = LDACBT_CHANNEL_MODE_DUAL_CHANNEL; channels_ = 2; break;
        case kMono: cm = LDACBT_CHANNEL_MODE_MONO; channels_ = 1; break;
        default: return {Err::InvalidArgument, "bad LDAC channel mode"};
        }
        fmt_ = cfg.bitsPerSample == 16 ? SampleFormat::S16 : SampleFormat::S32;
        LDACBT_SMPL_FMT_T lfmt = fmt_ == SampleFormat::S16 ? LDACBT_SMPL_FMT_S16 : LDACBT_SMPL_FMT_S32;

        h_ = ldacBT_get_handle();
        if (!h_) return {Err::EncoderError, "ldacBT_get_handle failed"};
        // libldac takes the full L2CAP MTU and reserves its own 18-byte header budget; >= 679 required.
        int rc = ldacBT_init_handle_encode(h_, l2capMtu, cfg.ldacEqmid, cm, lfmt, static_cast<int>(cfg.sampleRate));
        if (rc != 0) {
            int ec = ldacBT_get_error_code(h_);
            Close();
            return {Err::EncoderError, std::format("ldacBT_init_handle_encode failed: api={} handle={} block={} (mtu={}, sf={}, eqmid={})",
                                                   LDACBT_API_ERR(ec), LDACBT_HANDLE_ERR(ec), LDACBT_BLOCK_ERR(ec), l2capMtu,
                                                   cfg.sampleRate, cfg.ldacEqmid)};
        }
        sampleRate_ = cfg.sampleRate;
        abrEnabled_ = cfg.ldacAbr;
        if (abrEnabled_) {
            abr_ = ldac_ABR_get_handle();
            if (!abr_ || ldac_ABR_Init(abr_, kAbrIntervalMs) != 0) {
                BTA_LOGW("ldac", "ABR unavailable; using fixed EQMID");
                if (abr_) ldac_ABR_free_handle(abr_);
                abr_ = nullptr;
                abrEnabled_ = false;
            }
        }
        pcmSinceEmit_ = 0;
        BTA_LOGI("ldac", "encoder open: {} mtu={} version=0x{:06X} bitrate={} kbps", cfg.Describe(), l2capMtu,
                 ldacBT_get_version(), ldacBT_get_bitrate(h_));
        return Status::Ok();
    }

    void Close() override
    {
        if (abr_) ldac_ABR_free_handle(abr_);
        abr_ = nullptr;
        if (h_) {
            ldacBT_close_handle(h_);
            ldacBT_free_handle(h_);
        }
        h_ = nullptr;
    }

    SampleFormat InputFormat() const override { return fmt_; }
    int Channels() const override { return channels_; }
    uint32_t InputFramesPerCall() const override { return LDACBT_ENC_LSU; }

    Status Encode(const void* pcm, std::vector<EncodedPayload>& out) override
    {
        if (!h_) return {Err::StateError, "LDAC encoder not open"};
        int used = 0, size = 0, frames = 0;
        uint8_t stream[LDACBT_MAX_NBYTES];
        int rc = ldacBT_encode(h_, const_cast<void*>(pcm), &used, stream, &size, &frames);
        if (rc != 0) {
            int ec = ldacBT_get_error_code(h_);
            if (LDACBT_FATAL(ec)) return {Err::EncoderError, std::format("ldacBT_encode fatal error 0x{:X}", ec)};
            BTA_LOGW("ldac", "ldacBT_encode non-fatal error 0x{:X}", ec);
        }
        pcmSinceEmit_ += InputFramesPerCall();
        if (size > 0) {
            if (frames <= 0 || frames > a2dp::kMaxFramesPerPacket)
                return {Err::EncoderError, std::format("unexpected LDAC frame count {}", frames)};
            EncodedPayload p;
            p.bytes.assign(stream, stream + size);
            p.frames = static_cast<uint8_t>(frames);
            p.pcmFrames = pcmSinceEmit_;
            pcmSinceEmit_ = 0;
            out.push_back(std::move(p));
        }
        return Status::Ok();
    }

    void OnTransmitFeedback(uint32_t depth) override
    {
        if (abr_ && h_) ldac_ABR_Proc(h_, abr_, depth, abrEnabled_ ? 1 : 0);
    }

    uint32_t NominalBitrate() const override { return h_ ? static_cast<uint32_t>(ldacBT_get_bitrate(h_)) * 1000 : 0; }

    std::string QualityLabel() const override
    {
        if (!h_) return "closed";
        static const char* kNames[] = {"HQ", "SQ", "MQ"};
        int eq = ldacBT_get_eqmid(h_);
        std::string name = (eq >= 0 && eq < 3) ? kNames[eq] : std::format("eqmid {}", eq);
        return abrEnabled_ ? name + " (ABR)" : name;
    }

    Status SetQuality(int eqmid) override
    {
        if (!h_) return {Err::StateError, "LDAC encoder not open"};
        if (eqmid < LDACBT_EQMID_HQ || eqmid > LDACBT_EQMID_MQ) return {Err::InvalidArgument, "EQMID must be 0..2"};
        abrEnabled_ = false;
        if (ldacBT_set_eqmid(h_, eqmid) != 0) return {Err::EncoderError, "ldacBT_set_eqmid failed"};
        return Status::Ok();
    }

private:
    HANDLE_LDAC_BT h_ = nullptr;
    HANDLE_LDAC_ABR abr_ = nullptr;
    bool abrEnabled_ = false;
    SampleFormat fmt_ = SampleFormat::S32;
    int channels_ = 2;
    uint32_t sampleRate_ = 0;
    uint32_t pcmSinceEmit_ = 0;
};

}  // namespace

std::unique_ptr<IEncoder> MakeLdacEncoder() { return std::make_unique<LdacEncoder>(); }

std::unique_ptr<IEncoder> CreateEncoder(CodecId codec)
{
    switch (codec) {
    case CodecId::Sbc: return MakeSbcEncoder();
    case CodecId::Ldac: return MakeLdacEncoder();
    default: return nullptr;
    }
}

bool IsEncoderAvailable(CodecId codec, std::string* reason)
{
    if (codec == CodecId::Sbc) return true;
    if (codec != CodecId::Ldac) {
        if (reason) *reason = std::string(CodecName(codec)) + " encoder not included";
        return false;
    }
    HANDLE_LDAC_BT h = ldacBT_get_handle();
    if (!h) {
        if (reason) *reason = "libldac handle allocation failed";
        return false;
    }
    ldacBT_free_handle(h);
    return true;
}

}  // namespace bta
