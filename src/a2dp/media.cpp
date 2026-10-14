#include "a2dp/media.h"

namespace bta::a2dp {

std::vector<uint8_t> RtpPacketizer::Build(std::span<const uint8_t> payload, uint8_t frames, uint32_t pcmFrames)
{
    std::vector<uint8_t> p;
    p.reserve(kMediaOverhead + payload.size());
    p.push_back(0x80);  // V=2, P=0, X=0, CC=0
    p.push_back(kRtpPayloadType & 0x7F);
    p.push_back(static_cast<uint8_t>(seq_ >> 8));
    p.push_back(static_cast<uint8_t>(seq_));
    for (int s = 24; s >= 0; s -= 8) p.push_back(static_cast<uint8_t>(ts_ >> s));
    for (int s = 24; s >= 0; s -= 8) p.push_back(static_cast<uint8_t>(ssrc_ >> s));
    p.push_back(static_cast<uint8_t>(frames & 0x0F));
    p.insert(p.end(), payload.begin(), payload.end());
    ++seq_;
    ts_ += pcmFrames;
    return p;
}

std::optional<ParsedMediaPacket> ParseMediaPacket(std::span<const uint8_t> p)
{
    if (p.size() < kMediaOverhead) return std::nullopt;
    if ((p[0] >> 6) != 2) return std::nullopt;  // RTP version
    size_t csrc = p[0] & 0x0F;
    bool ext = (p[0] & 0x10) != 0;
    size_t off = kRtpHeaderSize + csrc * 4;
    if (p.size() < off + 1) return std::nullopt;
    if (ext) {
        if (p.size() < off + 4) return std::nullopt;
        size_t words = static_cast<size_t>((p[off + 2] << 8) | p[off + 3]);
        off += 4 + words * 4;
        if (p.size() < off + 1) return std::nullopt;
    }
    ParsedMediaPacket m;
    m.payloadType = p[1] & 0x7F;
    m.sequence = static_cast<uint16_t>((p[2] << 8) | p[3]);
    m.timestamp = (static_cast<uint32_t>(p[4]) << 24) | (p[5] << 16) | (p[6] << 8) | p[7];
    m.ssrc = (static_cast<uint32_t>(p[8]) << 24) | (p[9] << 16) | (p[10] << 8) | p[11];
    uint8_t h = p[off];
    m.fragmented = (h & 0x80) != 0;
    m.startFragment = (h & 0x40) != 0;
    m.lastFragment = (h & 0x20) != 0;
    m.frames = h & 0x0F;
    m.payload = p.subspan(off + 1);
    return m;
}

std::optional<LdacFrameHeader> ParseLdacFrameHeader(std::span<const uint8_t> d)
{
    if (d.size() < 3 || d[0] != 0xAA) return std::nullopt;
    uint32_t bits = (static_cast<uint32_t>(d[1]) << 8) | d[2];  // 16 bits after sync
    LdacFrameHeader h;
    h.sampleRateId = static_cast<uint8_t>((bits >> 13) & 0x07);
    h.channelConfigId = static_cast<uint8_t>((bits >> 11) & 0x03);
    h.frameLength = static_cast<uint16_t>(((bits >> 2) & 0x1FF) + 1);
    h.frameStatus = static_cast<uint8_t>(bits & 0x03);
    return h;
}

}  // namespace bta::a2dp
