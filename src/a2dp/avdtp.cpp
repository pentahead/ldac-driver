#include "a2dp/avdtp.h"

#include <format>

namespace bta::avdtp {

const char* SignalName(Signal s)
{
    switch (s) {
    case Signal::Discover: return "DISCOVER";
    case Signal::GetCapabilities: return "GET_CAPABILITIES";
    case Signal::SetConfiguration: return "SET_CONFIGURATION";
    case Signal::GetConfiguration: return "GET_CONFIGURATION";
    case Signal::Reconfigure: return "RECONFIGURE";
    case Signal::Open: return "OPEN";
    case Signal::Start: return "START";
    case Signal::Close: return "CLOSE";
    case Signal::Suspend: return "SUSPEND";
    case Signal::Abort: return "ABORT";
    case Signal::SecurityControl: return "SECURITY_CONTROL";
    case Signal::GetAllCapabilities: return "GET_ALL_CAPABILITIES";
    case Signal::DelayReport: return "DELAYREPORT";
    }
    return "UNKNOWN";
}

std::string ErrorName(uint8_t code)
{
    switch (code) {
    case error::kBadHeaderFormat: return "BAD_HEADER_FORMAT";
    case error::kBadLength: return "BAD_LENGTH";
    case error::kBadAcpSeid: return "BAD_ACP_SEID";
    case error::kSepInUse: return "SEP_IN_USE";
    case error::kSepNotInUse: return "SEP_NOT_IN_USE";
    case error::kBadServCategory: return "BAD_SERV_CATEGORY";
    case error::kBadPayloadFormat: return "BAD_PAYLOAD_FORMAT";
    case error::kNotSupportedCommand: return "NOT_SUPPORTED_COMMAND";
    case error::kInvalidCapabilities: return "INVALID_CAPABILITIES";
    case error::kUnsupportedConfiguration: return "UNSUPPORTED_CONFIGURATION";
    case error::kBadState: return "BAD_STATE";
    case error::kInvalidCodecType: return "INVALID_CODEC_TYPE";
    case error::kNotSupportedCodecType: return "NOT_SUPPORTED_CODEC_TYPE";
    case error::kInvalidSamplingFrequency: return "INVALID_SAMPLING_FREQUENCY";
    }
    return std::format("0x{:02X}", code);
}

static uint8_t Hdr(uint8_t label, PacketType pt, MsgType mt)
{
    return static_cast<uint8_t>(((label & 0x0F) << 4) | (static_cast<uint8_t>(pt) << 2) | static_cast<uint8_t>(mt));
}

std::vector<std::vector<uint8_t>> EncodeMessage(const Message& m, uint16_t mtu)
{
    std::vector<std::vector<uint8_t>> out;
    const uint8_t sig = static_cast<uint8_t>(m.signal) & 0x3F;
    if (mtu < 8) mtu = 8;
    if (m.payload.size() + 2 <= mtu) {
        std::vector<uint8_t> p{Hdr(m.label, PacketType::Single, m.type), sig};
        p.insert(p.end(), m.payload.begin(), m.payload.end());
        out.push_back(std::move(p));
        return out;
    }
    // Start packet carries 3 header bytes, continue/end carry 1.
    size_t firstChunk = mtu - 3;
    size_t rest = m.payload.size() - firstChunk;
    size_t nosp = 1 + (rest + (mtu - 2)) / (mtu - 1);
    std::vector<uint8_t> start{Hdr(m.label, PacketType::Start, m.type), static_cast<uint8_t>(nosp), sig};
    start.insert(start.end(), m.payload.begin(), m.payload.begin() + static_cast<ptrdiff_t>(firstChunk));
    out.push_back(std::move(start));
    size_t pos = firstChunk;
    while (pos < m.payload.size()) {
        size_t n = std::min<size_t>(mtu - 1, m.payload.size() - pos);
        bool last = pos + n == m.payload.size();
        std::vector<uint8_t> p{Hdr(m.label, last ? PacketType::End : PacketType::Continue, m.type)};
        p.insert(p.end(), m.payload.begin() + static_cast<ptrdiff_t>(pos), m.payload.begin() + static_cast<ptrdiff_t>(pos + n));
        out.push_back(std::move(p));
        pos += n;
    }
    return out;
}

void Reassembler::Reset()
{
    active_ = false;
    remaining_ = 0;
    cur_ = {};
}

std::optional<Message> Reassembler::Feed(std::span<const uint8_t> pkt, std::string* error)
{
    auto fail = [&](const char* why) -> std::optional<Message> {
        if (error) *error = why;
        Reset();
        return std::nullopt;
    };
    if (pkt.empty()) return fail("empty packet");
    uint8_t label = pkt[0] >> 4;
    auto pt = static_cast<PacketType>((pkt[0] >> 2) & 0x03);
    auto mt = static_cast<MsgType>(pkt[0] & 0x03);

    switch (pt) {
    case PacketType::Single: {
        if (pkt.size() < 2) return fail("single packet too short");
        if (active_) Reset();  // a new single packet aborts a pending fragmented one
        Message m;
        m.label = label;
        m.type = mt;
        m.signal = static_cast<Signal>(pkt[1] & 0x3F);
        m.payload.assign(pkt.begin() + 2, pkt.end());
        return m;
    }
    case PacketType::Start: {
        if (pkt.size() < 3) return fail("start packet too short");
        if (pkt[1] < 2) return fail("invalid NOSP");
        Reset();
        active_ = true;
        cur_.label = label;
        cur_.type = mt;
        cur_.signal = static_cast<Signal>(pkt[2] & 0x3F);
        cur_.payload.assign(pkt.begin() + 3, pkt.end());
        remaining_ = static_cast<uint8_t>(pkt[1] - 1);
        return std::nullopt;
    }
    case PacketType::Continue:
    case PacketType::End: {
        if (!active_ || label != cur_.label) return fail("unexpected continuation packet");
        if (remaining_ == 0) return fail("too many continuation packets");
        cur_.payload.insert(cur_.payload.end(), pkt.begin() + 1, pkt.end());
        --remaining_;
        if (pt == PacketType::End) {
            if (remaining_ != 0) return fail("end packet before NOSP reached");
            Message m = std::move(cur_);
            Reset();
            return m;
        }
        if (remaining_ == 0) return fail("NOSP exhausted without end packet");
        if (cur_.payload.size() > 64 * 1024) return fail("message too large");
        return std::nullopt;
    }
    }
    return fail("bad packet type");
}

std::vector<uint8_t> BuildDiscoverResponse(const std::vector<SepInfo>& seps)
{
    std::vector<uint8_t> out;
    for (const auto& s : seps) {
        out.push_back(static_cast<uint8_t>((s.seid << 2) | (s.inUse ? 0x02 : 0)));
        out.push_back(static_cast<uint8_t>((s.mediaType << 4) | (s.isSink ? 0x08 : 0)));
    }
    return out;
}

std::optional<std::vector<SepInfo>> ParseDiscoverResponse(std::span<const uint8_t> p)
{
    if (p.empty() || (p.size() % 2) != 0) return std::nullopt;
    std::vector<SepInfo> out;
    for (size_t i = 0; i < p.size(); i += 2) {
        SepInfo s;
        s.seid = p[i] >> 2;
        s.inUse = (p[i] & 0x02) != 0;
        s.mediaType = p[i + 1] >> 4;
        s.isSink = (p[i + 1] & 0x08) != 0;
        if (s.seid == 0 || s.seid > 0x3E) return std::nullopt;
        out.push_back(s);
    }
    return out;
}

std::vector<uint8_t> Capabilities::Serialize() const
{
    std::vector<uint8_t> out;
    auto put = [&](Category c, const std::vector<uint8_t>& d) {
        out.push_back(static_cast<uint8_t>(c));
        out.push_back(static_cast<uint8_t>(d.size()));
        out.insert(out.end(), d.begin(), d.end());
    };
    if (mediaTransport) put(Category::MediaTransport, {});
    for (const auto& o : other) put(o.category, o.data);
    for (const auto& c : codecs) put(Category::MediaCodec, c.Serialize());
    if (delayReporting) put(Category::DelayReporting, {});
    return out;
}

std::optional<Capabilities> Capabilities::Parse(std::span<const uint8_t> d, uint8_t* badCategory, bool strict)
{
    Capabilities caps;
    size_t i = 0;
    while (i < d.size()) {
        if (i + 2 > d.size()) return std::nullopt;
        uint8_t cat = d[i];
        uint8_t losc = d[i + 1];
        if (i + 2 + losc > d.size()) return std::nullopt;
        auto body = d.subspan(i + 2, losc);
        switch (static_cast<Category>(cat)) {
        case Category::MediaTransport:
            if (losc != 0) return std::nullopt;
            caps.mediaTransport = true;
            break;
        case Category::DelayReporting:
            if (losc != 0) return std::nullopt;
            caps.delayReporting = true;
            break;
        case Category::MediaCodec: {
            auto c = a2dp::CodecCapability::Parse(body);
            if (!c) return std::nullopt;
            caps.codecs.push_back(std::move(*c));
            break;
        }
        case Category::ContentProtection:
            caps.contentProtection = true;
            caps.other.push_back({static_cast<Category>(cat), {body.begin(), body.end()}});
            break;
        case Category::Reporting:
        case Category::Recovery:
        case Category::HeaderCompression:
        case Category::Multiplexing:
            caps.other.push_back({static_cast<Category>(cat), {body.begin(), body.end()}});
            break;
        default:
            if (strict) {
                if (badCategory) *badCategory = cat;
                return std::nullopt;
            }
            break;
        }
        i += 2 + losc;
    }
    return caps;
}

}  // namespace bta::avdtp
