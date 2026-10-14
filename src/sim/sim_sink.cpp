#include "sim/sim_sink.h"

#include <sbc.h>

#include "a2dp/media.h"
#include "common/log.h"

namespace bta::sim {

using namespace avdtp;
static constexpr const char* kLog = "simsink";
static constexpr uint8_t kSbcSeid = 1, kLdacSeid = 2;

SimulatedSink::SimulatedSink(SimSinkOptions opts) : opts_(std::move(opts))
{
    auto* d = new sbc_t{};
    sbc_reset(d);
    sbcDecoder_ = d;
}

SimulatedSink::~SimulatedSink()
{
    stop_ = true;
    {
        std::lock_guard lk(mu_);
        if (sig_) sig_->Close();
        if (media_) media_->Close();
        for (auto& c : retiredChannels_) c->Close();
    }
    if (sigThread_.joinable()) sigThread_.join();
    if (mediaThread_.joinable()) mediaThread_.join();
    for (auto& t : retiredThreads_)
        if (t.joinable()) t.join();
    delete static_cast<sbc_t*>(sbcDecoder_);
}

void SimulatedSink::OnChannel(uint16_t, std::unique_ptr<IL2capChannel> ch)
{
    std::lock_guard lk(mu_);
    auto retire = [&](std::unique_ptr<IL2capChannel>& c, std::thread& t) {
        if (c) {
            c->Close();
            retiredChannels_.push_back(std::move(c));
        }
        if (t.joinable()) retiredThreads_.push_back(std::move(t));
    };
    IL2capChannel* raw = ch.get();
    if (!sig_ || !sig_->IsOpen()) {
        retire(sig_, sigThread_);
        retire(media_, mediaThread_);
        sig_ = std::move(ch);
        sigThread_ = std::thread([this, raw] { SignalLoop(raw); });
    } else {
        retire(media_, mediaThread_);
        media_ = std::move(ch);
        haveSeq_ = false;
        mediaThread_ = std::thread([this, raw] { MediaLoop(raw); });
    }
}

SimSinkStats SimulatedSink::Stats() const
{
    std::lock_guard lk(mu_);
    return st_;
}

void SimulatedSink::DropLink()
{
    std::lock_guard lk(mu_);
    if (sig_) sig_->Close();
    if (media_) media_->Close();
    st_.state = "disconnected";
}

void SimulatedSink::SendCommand(Signal sig, std::vector<uint8_t> payload)
{
    Message m;
    IL2capChannel* ch;
    {
        std::lock_guard lk(mu_);
        ch = sig_.get();
        m.label = label_;
        label_ = static_cast<uint8_t>((label_ + 1) & 0x0F);
    }
    m.type = MsgType::Command;
    m.signal = sig;
    m.payload = std::move(payload);
    if (!ch) return;
    std::lock_guard lk(sendMu_);
    for (auto& p : EncodeMessage(m, ch->OutMtu())) ch->Send(p);
}

void SimulatedSink::Reply(IL2capChannel* ch, const Message& cmd, MsgType t, std::vector<uint8_t> payload)
{
    Message r;
    r.label = cmd.label;
    r.type = t;
    r.signal = cmd.signal;
    r.payload = std::move(payload);
    std::lock_guard lk(sendMu_);
    for (auto& p : EncodeMessage(r, ch->OutMtu())) ch->Send(p);
}

void SimulatedSink::SignalLoop(IL2capChannel* ch)
{
    Reassembler reasm;
    std::vector<uint8_t> pkt;
    while (!stop_) {
        Status s = ch->Receive(pkt, 100);
        if (s.code() == Err::Timeout) continue;
        if (!s) break;
        auto m = reasm.Feed(pkt);
        if (!m) continue;
        if (m->type != MsgType::Command) continue;  // responses to our own commands are ignored
        Handle(*m, ch);
    }
}

void SimulatedSink::Handle(const Message& m, IL2capChannel* ch)
{
    {
        std::lock_guard lk(mu_);
        st_.signals.push_back(SignalName(m.signal));
        ++commandCount_;
        if (opts_.ignoreCommandsAfter && commandCount_ > opts_.ignoreCommandsAfter) return;
    }
    uint8_t acp = m.payload.empty() ? 0 : SeidFromByte(m.payload[0]);
    auto capsFor = [&](uint8_t seid, bool all) -> std::optional<Capabilities> {
        Capabilities c;
        c.mediaTransport = true;
        if (seid == kSbcSeid) c.codecs.push_back(opts_.sbcCaps.ToCapability());
        else if (seid == kLdacSeid && opts_.ldac) c.codecs.push_back(opts_.ldacCaps.ToCapability());
        else return std::nullopt;
        c.delayReporting = all && opts_.delayReporting;
        return c;
    };

    switch (m.signal) {
    case Signal::Discover: {
        std::vector<SepInfo> seps{{kSbcSeid, false, 0, true}};
        if (opts_.ldac) seps.push_back({kLdacSeid, false, 0, true});
        Reply(ch, m, MsgType::Accept, BuildDiscoverResponse(seps));
        break;
    }
    case Signal::GetAllCapabilities:
        if (!opts_.supportGetAllCapabilities) {
            Reply(ch, m, MsgType::GeneralReject);
            break;
        }
        [[fallthrough]];
    case Signal::GetCapabilities: {
        auto c = capsFor(acp, m.signal == Signal::GetAllCapabilities);
        if (!c) Reply(ch, m, MsgType::Reject, {error::kBadAcpSeid});
        else Reply(ch, m, MsgType::Accept, c->Serialize());
        break;
    }
    case Signal::SetConfiguration: {
        auto caps = Capabilities::Parse(std::span(m.payload).subspan(2));
        if (!caps || caps->codecs.size() != 1 || !capsFor(acp, false)) {
            Reply(ch, m, MsgType::Reject, {0x07, error::kInvalidCapabilities});
            break;
        }
        CodecId id = caps->codecs[0].Identify();
        if (id == CodecId::Ldac && opts_.rejectLdacConfiguration) {
            Reply(ch, m, MsgType::Reject, {0x07, error::kUnsupportedConfiguration});
            break;
        }
        {
            std::lock_guard lk(mu_);
            st_.configuredCodec = id;
            if (id == CodecId::Sbc) {
                auto s = a2dp::SbcCaps::Parse(caps->codecs[0].info);
                st_.sampleRate = s ? a2dp::SbcFreqHz(s->freq) : 0;
                st_.channels = (s && s->channelModes == a2dp::sbcbits::kMono) ? 1 : 2;
            } else if (id == CodecId::Ldac) {
                auto l = a2dp::LdacCaps::Parse(caps->codecs[0].info);
                st_.sampleRate = l ? a2dp::LdacFreqHz(l->freq) : 0;
                st_.channels = (l && l->channelModes == a2dp::ldacbits::kMono) ? 1 : 2;
            }
            cfgSeid_ = acp;
            srcSeid_ = SeidFromByte(m.payload[1]);
            delayPending_ = caps->delayReporting;
            st_.state = "configured";
        }
        sbc_reset(static_cast<sbc_t*>(sbcDecoder_));
        Reply(ch, m, MsgType::Accept);
        break;
    }
    case Signal::Reconfigure: {
        auto caps = Capabilities::Parse(std::span(m.payload).subspan(1));
        if (!caps || caps->codecs.size() != 1) {
            Reply(ch, m, MsgType::Reject, {0x07, error::kInvalidCapabilities});
            break;
        }
        Reply(ch, m, MsgType::Accept);
        break;
    }
    case Signal::Open: {
        Reply(ch, m, MsgType::Accept);
        bool sendDelay;
        uint8_t src;
        {
            std::lock_guard lk(mu_);
            st_.state = "open";
            sendDelay = delayPending_;
            src = srcSeid_;
        }
        if (sendDelay)
            SendCommand(Signal::DelayReport, {SeidByte(src), static_cast<uint8_t>(opts_.delayReportTenthMs >> 8),
                                              static_cast<uint8_t>(opts_.delayReportTenthMs & 0xFF)});
        break;
    }
    case Signal::Start: {
        std::lock_guard lk(mu_);
        st_.state = "streaming";
        Reply(ch, m, MsgType::Accept);
        break;
    }
    case Signal::Suspend: {
        std::lock_guard lk(mu_);
        st_.state = "open";
        Reply(ch, m, MsgType::Accept);
        break;
    }
    case Signal::Close:
    case Signal::Abort: {
        Reply(ch, m, MsgType::Accept);
        std::lock_guard lk(mu_);
        st_.state = "idle";
        st_.configuredCodec = CodecId::None;
        if (media_) media_->Close();
        break;
    }
    default: Reply(ch, m, MsgType::GeneralReject); break;
    }
}

void SimulatedSink::MediaLoop(IL2capChannel* ch)
{
    std::vector<uint8_t> pkt;
    while (!stop_) {
        Status s = ch->Receive(pkt, 100);
        if (s.code() == Err::Timeout) continue;
        if (!s) break;
        ValidateMedia(pkt);
    }
}

void SimulatedSink::ValidateMedia(const std::vector<uint8_t>& pkt)
{
    std::lock_guard lk(mu_);
    auto mp = a2dp::ParseMediaPacket(pkt);
    if (!mp || mp->payloadType != a2dp::kRtpPayloadType || mp->frames == 0) {
        st_.malformedPackets++;
        return;
    }
    if (haveSeq_ && mp->sequence != expectSeq_) st_.sequenceErrors++;
    if (haveSeq_ && mp->timestamp <= st_.lastTimestamp && st_.mediaPackets > 0) st_.timestampErrors++;
    haveSeq_ = true;
    expectSeq_ = static_cast<uint16_t>(mp->sequence + 1);
    st_.lastTimestamp = mp->timestamp;
    st_.mediaPackets++;
    st_.mediaBytes += pkt.size();

    auto data = mp->payload;
    unsigned counted = 0;
    if (st_.configuredCodec == CodecId::Sbc) {
        auto* dec = static_cast<sbc_t*>(sbcDecoder_);
        while (!data.empty()) {
            sbc_frame f;
            if (sbc_probe(data.data(), &f) != 0) {
                st_.malformedPackets++;
                break;
            }
            unsigned size = sbc_get_frame_size(&f);
            if (size > data.size()) {
                st_.malformedPackets++;
                break;
            }
            if (opts_.decodeSbc && st_.decodedLeft.size() < opts_.maxDecodedSamples) {
                int16_t l[SBC_MAX_SAMPLES], r[SBC_MAX_SAMPLES];
                if (sbc_decode(dec, data.data(), size, &f, l, 1, r, 1) == 0)
                    st_.decodedLeft.insert(st_.decodedLeft.end(), l, l + f.nblocks * f.nsubbands);
                else
                    st_.malformedPackets++;
            }
            data = data.subspan(size);
            ++counted;
            st_.sbcFrames++;
        }
    } else if (st_.configuredCodec == CodecId::Ldac) {
        while (!data.empty()) {
            auto h = a2dp::ParseLdacFrameHeader(data);
            if (!h || h->TotalSize() > data.size()) {
                st_.malformedPackets++;
                break;
            }
            if (st_.ldacSampleRateIds.empty() || st_.ldacSampleRateIds.back() != h->sampleRateId)
                st_.ldacSampleRateIds.push_back(h->sampleRateId);
            data = data.subspan(h->TotalSize());
            ++counted;
            st_.ldacFrames++;
        }
    } else {
        st_.malformedPackets++;
        return;
    }
    st_.codecFrames += counted;
    if (counted != mp->frames) st_.frameCountMismatches++;
}

}  // namespace bta::sim
