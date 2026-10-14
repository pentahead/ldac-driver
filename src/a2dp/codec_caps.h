// A2DP codec information elements (A2DP spec 1.3/1.4 sec. 4) and stream-configuration negotiation.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "common/settings.h"
#include "common/status.h"
#include "common/types.h"

namespace bta::a2dp {

constexpr uint8_t kMediaTypeAudio = 0x00;
constexpr uint8_t kCodecTypeSbc = 0x00;
constexpr uint8_t kCodecTypeMpeg24 = 0x02;  // AAC
constexpr uint8_t kCodecTypeVendor = 0xFF;

constexpr uint32_t kVendorSony = 0x0000012D;
constexpr uint16_t kCodecIdLdac = 0x00AA;
constexpr uint32_t kVendorQualcommAptX = 0x0000004F;
constexpr uint32_t kVendorQualcommAptXHd = 0x000000D7;
constexpr uint32_t kVendorCsr = 0x0000000A;

namespace sbcbits {
constexpr uint8_t kFreq16000 = 0x80, kFreq32000 = 0x40, kFreq44100 = 0x20, kFreq48000 = 0x10;
constexpr uint8_t kMono = 0x08, kDual = 0x04, kStereo = 0x02, kJoint = 0x01;
constexpr uint8_t kBlocks4 = 0x80, kBlocks8 = 0x40, kBlocks12 = 0x20, kBlocks16 = 0x10;
constexpr uint8_t kSubbands4 = 0x08, kSubbands8 = 0x04;
constexpr uint8_t kAllocSnr = 0x02, kAllocLoudness = 0x01;
}  // namespace sbcbits

namespace ldacbits {
constexpr uint8_t kFreq44100 = 0x20, kFreq48000 = 0x10, kFreq88200 = 0x08, kFreq96000 = 0x04,
                  kFreq176400 = 0x02, kFreq192000 = 0x01;
constexpr uint8_t kMono = 0x04, kDual = 0x02, kStereo = 0x01;
}  // namespace ldacbits

// Raw "Media Codec" service capability payload (after category/LOSC).
struct CodecCapability {
    uint8_t mediaType = kMediaTypeAudio;
    uint8_t codecType = kCodecTypeSbc;
    std::vector<uint8_t> info;  // codec-specific information elements

    CodecId Identify() const;
    std::vector<uint8_t> Serialize() const;  // mediaType<<4, codecType, info...
    static std::optional<CodecCapability> Parse(std::span<const uint8_t> payload);
    std::string Describe() const;
};

struct SbcCaps {
    uint8_t freq = 0, channelModes = 0, blocks = 0, subbands = 0, allocation = 0;
    uint8_t minBitpool = 2, maxBitpool = 53;

    static std::optional<SbcCaps> Parse(std::span<const uint8_t> info);  // 4 octets
    std::vector<uint8_t> Build() const;
    CodecCapability ToCapability() const { return {kMediaTypeAudio, kCodecTypeSbc, Build()}; }
};

struct LdacCaps {
    uint8_t freq = 0, channelModes = 0;

    static std::optional<LdacCaps> Parse(std::span<const uint8_t> info);  // 8 octets (vendor+codec+2)
    std::vector<uint8_t> Build() const;
    CodecCapability ToCapability() const { return {kMediaTypeAudio, kCodecTypeVendor, Build()}; }
};

// Fully resolved stream configuration (one value per field).
struct StreamConfig {
    CodecId codec = CodecId::None;
    uint32_t sampleRate = 0;
    uint8_t channels = 2;
    uint8_t bitsPerSample = 16;  // PCM fed to the encoder
    SbcCaps sbc;                 // single-bit fields when codec == Sbc
    LdacCaps ldac;               // single-bit fields when codec == Ldac
    int ldacEqmid = 0;           // libldac EQMID (0=HQ,1=SQ,2=MQ)
    bool ldacAbr = false;

    CodecCapability ToCapability() const;
    std::string Describe() const;
    uint32_t SamplesPerCodecFrame() const;  // per channel
};

struct NegotiationPrefs {
    CodecPreference preference = CodecPreference::Auto;
    uint32_t sampleRate = 0;      // 0 = auto
    uint32_t sourceRate = 48000;  // native rate of the PCM source (avoids resampling when possible)
    uint8_t bitsPerSample = 0;    // 0 = auto
    LdacQuality ldacQuality = LdacQuality::Adaptive;
    SbcSettings sbc;
};

Result<StreamConfig> NegotiateSbc(const SbcCaps& sink, const NegotiationPrefs& p);
Result<StreamConfig> NegotiateLdac(const LdacCaps& sink, const NegotiationPrefs& p);

// Validates that a received configuration (e.g. from sink-initiated SET_CONFIGURATION)
// selects exactly one value per field and stays within our local capabilities.
Result<StreamConfig> ConfigFromCapability(const CodecCapability& cfg, const NegotiationPrefs& p);

uint32_t SbcFreqHz(uint8_t bit);
uint32_t LdacFreqHz(uint8_t bit);

// Local SEP capabilities we advertise.
SbcCaps LocalSbcCaps();
LdacCaps LocalLdacCaps();

}  // namespace bta::a2dp
