#include "a2dp/codec_caps.h"

#include <algorithm>
#include <format>

namespace bta::a2dp {

namespace {

uint32_t ReadLe32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }
uint16_t ReadLe16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

bool SingleBit(uint8_t v) { return v != 0 && (v & (v - 1)) == 0; }

struct RateBit {
    uint32_t hz;
    uint8_t bit;
};
constexpr RateBit kSbcRates[] = {{48000, sbcbits::kFreq48000}, {44100, sbcbits::kFreq44100},
                                 {32000, sbcbits::kFreq32000}, {16000, sbcbits::kFreq16000}};
constexpr RateBit kLdacRates[] = {{48000, ldacbits::kFreq48000}, {44100, ldacbits::kFreq44100},
                                  {96000, ldacbits::kFreq96000}, {88200, ldacbits::kFreq88200}};

// Picks a rate: explicit preference, else the source rate, else first supported in table order.
template <size_t N>
std::optional<RateBit> PickRate(uint8_t mask, const RateBit (&table)[N], uint32_t preferred, uint32_t source)
{
    auto find = [&](uint32_t hz) -> std::optional<RateBit> {
        for (const auto& r : table)
            if (r.hz == hz && (mask & r.bit)) return r;
        return std::nullopt;
    };
    if (preferred) {
        if (auto r = find(preferred)) return r;
    }
    if (auto r = find(source)) return r;
    // Prefer a rate in the same family as the source (44.1k multiples vs 48k multiples).
    bool fam441 = source && (source % 11025 == 0);
    for (const auto& r : table)
        if ((mask & r.bit) && ((r.hz % 11025 == 0) == fam441)) return r;
    for (const auto& r : table)
        if (mask & r.bit) return r;
    return std::nullopt;
}

}  // namespace

uint32_t SbcFreqHz(uint8_t bit)
{
    for (const auto& r : kSbcRates)
        if (r.bit == bit) return r.hz;
    return 0;
}

uint32_t LdacFreqHz(uint8_t bit)
{
    for (const auto& r : kLdacRates)
        if (r.bit == bit) return r.hz;
    if (bit == ldacbits::kFreq176400) return 176400;
    if (bit == ldacbits::kFreq192000) return 192000;
    return 0;
}

// ---------------------------------------------------------------------------
CodecId CodecCapability::Identify() const
{
    if (mediaType != kMediaTypeAudio) return CodecId::None;
    if (codecType == kCodecTypeSbc) return CodecId::Sbc;
    if (codecType == kCodecTypeMpeg24) return CodecId::Aac;
    if (codecType != kCodecTypeVendor || info.size() < 6) return CodecId::None;
    uint32_t vendor = ReadLe32(info.data());
    uint16_t codec = ReadLe16(info.data() + 4);
    if (vendor == kVendorSony && codec == kCodecIdLdac) return CodecId::Ldac;
    if (vendor == kVendorQualcommAptX && codec == 0x0001) return CodecId::AptX;
    if (vendor == kVendorQualcommAptXHd && codec == 0x0024) return CodecId::AptXHd;
    if (vendor == kVendorQualcommAptXHd && codec == 0x00AD) return CodecId::AptXAdaptive;
    if (vendor == kVendorCsr && codec == 0x0002) return CodecId::AptXLl;
    return CodecId::OtherVendor;
}

std::vector<uint8_t> CodecCapability::Serialize() const
{
    std::vector<uint8_t> out;
    out.push_back(static_cast<uint8_t>(mediaType << 4));
    out.push_back(codecType);
    out.insert(out.end(), info.begin(), info.end());
    return out;
}

std::optional<CodecCapability> CodecCapability::Parse(std::span<const uint8_t> payload)
{
    if (payload.size() < 2) return std::nullopt;
    CodecCapability c;
    c.mediaType = payload[0] >> 4;
    c.codecType = payload[1];
    c.info.assign(payload.begin() + 2, payload.end());
    return c;
}

std::string CodecCapability::Describe() const
{
    std::string hex;
    for (uint8_t b : info) hex += std::format("{:02X}", b);
    std::string extra;
    if (codecType == kCodecTypeVendor && info.size() >= 6)
        extra = std::format(" vendor=0x{:08X} id=0x{:04X}", ReadLe32(info.data()), ReadLe16(info.data() + 4));
    return std::format("{} (type 0x{:02X}{}) info={}", CodecName(Identify()), codecType, extra, hex);
}

// ---------------------------------------------------------------------------
std::optional<SbcCaps> SbcCaps::Parse(std::span<const uint8_t> info)
{
    if (info.size() != 4) return std::nullopt;
    SbcCaps c;
    c.freq = info[0] & 0xF0;
    c.channelModes = info[0] & 0x0F;
    c.blocks = info[1] & 0xF0;
    c.subbands = info[1] & 0x0C;
    c.allocation = info[1] & 0x03;
    c.minBitpool = info[2];
    c.maxBitpool = info[3];
    return c;
}

std::vector<uint8_t> SbcCaps::Build() const
{
    return {static_cast<uint8_t>(freq | channelModes), static_cast<uint8_t>(blocks | subbands | allocation), minBitpool,
            maxBitpool};
}

std::optional<LdacCaps> LdacCaps::Parse(std::span<const uint8_t> info)
{
    if (info.size() != 8) return std::nullopt;
    if (ReadLe32(info.data()) != kVendorSony || ReadLe16(info.data() + 4) != kCodecIdLdac) return std::nullopt;
    LdacCaps c;
    c.freq = info[6] & 0x3F;
    c.channelModes = info[7] & 0x07;
    return c;
}

std::vector<uint8_t> LdacCaps::Build() const
{
    return {static_cast<uint8_t>(kVendorSony & 0xFF), static_cast<uint8_t>((kVendorSony >> 8) & 0xFF), 0, 0,
            static_cast<uint8_t>(kCodecIdLdac & 0xFF), static_cast<uint8_t>(kCodecIdLdac >> 8), freq, channelModes};
}

SbcCaps LocalSbcCaps()
{
    using namespace sbcbits;
    SbcCaps c;
    c.freq = kFreq16000 | kFreq32000 | kFreq44100 | kFreq48000;
    c.channelModes = kMono | kDual | kStereo | kJoint;
    c.blocks = kBlocks4 | kBlocks8 | kBlocks12 | kBlocks16;
    c.subbands = kSubbands4 | kSubbands8;
    c.allocation = kAllocSnr | kAllocLoudness;
    c.minBitpool = 2;
    c.maxBitpool = 250;
    return c;
}

LdacCaps LocalLdacCaps()
{
    using namespace ldacbits;
    // libldac encodes 44.1/48/88.2/96 kHz only (ga_smplrate_ldac); 176.4/192 are not advertised.
    return {static_cast<uint8_t>(kFreq44100 | kFreq48000 | kFreq88200 | kFreq96000),
            static_cast<uint8_t>(kMono | kDual | kStereo)};
}

// ---------------------------------------------------------------------------
Result<StreamConfig> NegotiateSbc(const SbcCaps& sinkIn, const NegotiationPrefs& p)
{
    using namespace sbcbits;
    SbcCaps local = LocalSbcCaps();
    SbcCaps s;
    s.freq = sinkIn.freq & local.freq;
    s.channelModes = sinkIn.channelModes & local.channelModes;
    s.blocks = sinkIn.blocks & local.blocks;
    s.subbands = sinkIn.subbands & local.subbands;
    s.allocation = sinkIn.allocation & local.allocation;
    if (!s.freq || !s.channelModes || !s.blocks || !s.subbands || !s.allocation)
        return Status{Err::NotSupported, "no common SBC parameters with sink"};

    StreamConfig cfg;
    cfg.codec = CodecId::Sbc;
    auto rate = PickRate(s.freq, kSbcRates, p.sampleRate, p.sourceRate);
    if (!rate) return Status{Err::NotSupported, "no common SBC sampling frequency"};
    cfg.sampleRate = rate->hz;
    cfg.sbc.freq = rate->bit;

    for (uint8_t m : {kJoint, kStereo, kDual, kMono})
        if (s.channelModes & m) {
            cfg.sbc.channelModes = m;
            break;
        }
    cfg.channels = (cfg.sbc.channelModes == kMono) ? 1 : 2;

    auto blockBit = [](uint8_t n) -> uint8_t {
        switch (n) {
        case 4: return kBlocks4;
        case 8: return kBlocks8;
        case 12: return kBlocks12;
        default: return kBlocks16;
        }
    };
    uint8_t wantBlocks = blockBit(p.sbc.blocks);
    cfg.sbc.blocks = (s.blocks & wantBlocks) ? wantBlocks : 0;
    if (!cfg.sbc.blocks)
        for (uint8_t b : {kBlocks16, kBlocks12, kBlocks8, kBlocks4})
            if (s.blocks & b) {
                cfg.sbc.blocks = b;
                break;
            }
    uint8_t wantSub = (p.sbc.subbands == 4) ? kSubbands4 : kSubbands8;
    cfg.sbc.subbands = (s.subbands & wantSub) ? wantSub : ((s.subbands & kSubbands8) ? kSubbands8 : kSubbands4);
    uint8_t wantAlloc = p.sbc.loudness ? kAllocLoudness : kAllocSnr;
    cfg.sbc.allocation = (s.allocation & wantAlloc) ? wantAlloc : (s.allocation & kAllocLoudness ? kAllocLoudness : kAllocSnr);

    // Bitpool: our preferred maximum, bounded by sink range and the SBC syntax limit.
    int nsub = (cfg.sbc.subbands == kSubbands8) ? 8 : 4;
    int syntaxMax = (cfg.sbc.channelModes == kMono || cfg.sbc.channelModes == kDual) ? 16 * nsub : 32 * nsub;
    int minBp = std::max<int>(2, sinkIn.minBitpool);
    int maxBp = std::min({static_cast<int>(sinkIn.maxBitpool), syntaxMax, static_cast<int>(p.sbc.maxBitpool)});
    if (sinkIn.minBitpool > sinkIn.maxBitpool || maxBp < minBp)
        return Status{Err::NotSupported, std::format("invalid SBC bitpool range {}..{}", sinkIn.minBitpool, sinkIn.maxBitpool)};
    cfg.sbc.minBitpool = static_cast<uint8_t>(minBp);
    cfg.sbc.maxBitpool = static_cast<uint8_t>(maxBp);
    cfg.bitsPerSample = 16;
    return cfg;
}

Result<StreamConfig> NegotiateLdac(const LdacCaps& sinkIn, const NegotiationPrefs& p)
{
    using namespace ldacbits;
    LdacCaps local = LocalLdacCaps();
    uint8_t freq = sinkIn.freq & local.freq;
    uint8_t modes = sinkIn.channelModes & local.channelModes;
    if (!freq || !modes) return Status{Err::NotSupported, "no common LDAC parameters with sink"};
    StreamConfig cfg;
    cfg.codec = CodecId::Ldac;
    auto rate = PickRate(freq, kLdacRates, p.sampleRate, p.sourceRate);
    if (!rate) return Status{Err::NotSupported, "no common LDAC sampling frequency"};
    cfg.sampleRate = rate->hz;
    cfg.ldac.freq = rate->bit;
    for (uint8_t m : {kStereo, kDual, kMono})
        if (modes & m) {
            cfg.ldac.channelModes = m;
            break;
        }
    cfg.channels = (cfg.ldac.channelModes == kMono) ? 1 : 2;
    cfg.bitsPerSample = (p.bitsPerSample == 24) ? 24 : (p.bitsPerSample == 16 ? 16 : 24);
    switch (p.ldacQuality) {
    case LdacQuality::High: cfg.ldacEqmid = 0; break;
    case LdacQuality::Standard: cfg.ldacEqmid = 1; break;
    case LdacQuality::Mobile: cfg.ldacEqmid = 2; break;
    case LdacQuality::Adaptive:
        cfg.ldacEqmid = 0;  // ABR starts at HQ and steps down on congestion
        cfg.ldacAbr = true;
        break;
    }
    return cfg;
}

Result<StreamConfig> ConfigFromCapability(const CodecCapability& cap, const NegotiationPrefs& p)
{
    switch (cap.Identify()) {
    case CodecId::Sbc: {
        auto c = SbcCaps::Parse(cap.info);
        if (!c) return Status{Err::ProtocolError, "bad SBC configuration length"};
        if (!SingleBit(c->freq) || !SingleBit(c->channelModes) || !SingleBit(c->blocks) || !SingleBit(c->subbands) ||
            !SingleBit(c->allocation))
            return Status{Err::InvalidArgument, "SBC configuration must select one value per field"};
        if (c->minBitpool < 2 || c->minBitpool > c->maxBitpool)
            return Status{Err::InvalidArgument, "invalid SBC bitpool configuration"};
        NegotiationPrefs q = p;
        q.sampleRate = SbcFreqHz(c->freq);
        q.sbc.maxBitpool = c->maxBitpool;
        auto r = NegotiateSbc(*c, q);
        if (!r) return r;
        r->sbc.minBitpool = c->minBitpool;
        return r;
    }
    case CodecId::Ldac: {
        auto c = LdacCaps::Parse(cap.info);
        if (!c) return Status{Err::ProtocolError, "bad LDAC configuration"};
        if (!SingleBit(c->freq) || !SingleBit(c->channelModes))
            return Status{Err::InvalidArgument, "LDAC configuration must select one value per field"};
        NegotiationPrefs q = p;
        q.sampleRate = LdacFreqHz(c->freq);
        auto r = NegotiateLdac(*c, q);
        if (r && r->sampleRate != q.sampleRate)
            return Status{Err::NotSupported, "LDAC sampling frequency not supported locally"};
        return r;
    }
    default: return Status{Err::NotSupported, "codec not supported: " + cap.Describe()};
    }
}

// ---------------------------------------------------------------------------
CodecCapability StreamConfig::ToCapability() const
{
    if (codec == CodecId::Ldac) return ldac.ToCapability();
    return sbc.ToCapability();
}

uint32_t StreamConfig::SamplesPerCodecFrame() const
{
    if (codec == CodecId::Sbc) {
        int blocks = sbc.blocks == sbcbits::kBlocks4 ? 4 : sbc.blocks == sbcbits::kBlocks8 ? 8 : sbc.blocks == sbcbits::kBlocks12 ? 12 : 16;
        int sub = sbc.subbands == sbcbits::kSubbands4 ? 4 : 8;
        return static_cast<uint32_t>(blocks * sub);
    }
    if (codec == CodecId::Ldac) return sampleRate > 48000 ? 256 : 128;
    return 0;
}

std::string StreamConfig::Describe() const
{
    using namespace sbcbits;
    if (codec == CodecId::Sbc) {
        const char* mode = sbc.channelModes == kJoint ? "joint" : sbc.channelModes == kStereo ? "stereo" : sbc.channelModes == kDual ? "dual" : "mono";
        int blocks = sbc.blocks == kBlocks4 ? 4 : sbc.blocks == kBlocks8 ? 8 : sbc.blocks == kBlocks12 ? 12 : 16;
        return std::format("SBC {} Hz {} blocks={} subbands={} alloc={} bitpool={}..{}", sampleRate, mode, blocks,
                           sbc.subbands == kSubbands8 ? 8 : 4, sbc.allocation == kAllocLoudness ? "loudness" : "snr",
                           sbc.minBitpool, sbc.maxBitpool);
    }
    if (codec == CodecId::Ldac) {
        const char* mode = ldac.channelModes == ldacbits::kStereo ? "stereo" : ldac.channelModes == ldacbits::kDual ? "dual" : "mono";
        static const char* kEq[] = {"HQ", "SQ", "MQ"};
        return std::format("LDAC {} Hz {} {}-bit eqmid={}{}", sampleRate, mode, bitsPerSample,
                           (ldacEqmid >= 0 && ldacEqmid < 3) ? kEq[ldacEqmid] : "?", ldacAbr ? " (ABR)" : "");
    }
    return "none";
}

}  // namespace bta::a2dp
