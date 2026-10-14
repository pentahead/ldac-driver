// A2DP media packets: RTP header (RFC 3550, 12 bytes) + 1-byte codec payload header
// (SBC: A2DP sec. 4.3.4; LDAC: same layout per AOSP a2dp_vendor_ldac_constants.h: F|S|L|RFA|NumFrames).
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace bta::a2dp {

constexpr size_t kRtpHeaderSize = 12;
constexpr size_t kMediaPayloadHeaderSize = 1;
constexpr size_t kMediaOverhead = kRtpHeaderSize + kMediaPayloadHeaderSize;
constexpr uint8_t kRtpPayloadType = 96;  // dynamic
constexpr uint8_t kMaxFramesPerPacket = 15;

class RtpPacketizer {
public:
    explicit RtpPacketizer(uint32_t ssrc = 1) : ssrc_(ssrc) {}

    // frames: number of codec frames in payload (1..15); pcmFrames advances the RTP timestamp.
    std::vector<uint8_t> Build(std::span<const uint8_t> codecPayload, uint8_t frames, uint32_t pcmFrames);
    uint16_t NextSequence() const { return seq_; }
    uint32_t NextTimestamp() const { return ts_; }
    void Reset() { seq_ = 0, ts_ = 0; }

private:
    uint32_t ssrc_;
    uint16_t seq_ = 0;
    uint32_t ts_ = 0;
};

struct ParsedMediaPacket {
    uint16_t sequence;
    uint32_t timestamp;
    uint32_t ssrc;
    uint8_t payloadType;
    bool fragmented, startFragment, lastFragment;
    uint8_t frames;
    std::span<const uint8_t> payload;  // codec frames
};
std::optional<ParsedMediaPacket> ParseMediaPacket(std::span<const uint8_t> pkt);

// LDAC frame header (libldac pack_frame_header_ldac): sync 0xAA(8) smplrate_id(3) chconfig_id(2)
// frame_length-1(9) frame_status(2). frame_length counts the bytes *after* the 3-byte header
// (verified against libldac output: 48 kHz HQ frames are 3 + 327 = 330 bytes, two per 660-byte packet).
struct LdacFrameHeader {
    uint8_t sampleRateId, channelConfigId, frameStatus;
    uint16_t frameLength;  // bytes following the header
    size_t TotalSize() const { return 3u + frameLength; }
};
std::optional<LdacFrameHeader> ParseLdacFrameHeader(std::span<const uint8_t> data);

}  // namespace bta::a2dp
