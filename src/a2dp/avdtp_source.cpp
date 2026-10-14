#include "a2dp/avdtp_source.h"

#include <chrono>

#include "common/log.h"

namespace bta::avdtp {

static constexpr const char* kLog = "avdtp";

const char* StreamStateName(StreamState s)
{
    switch (s) {
    case StreamState::Idle: return "idle";
    case StreamState::Configured: return "configured";
    case StreamState::Open: return "open";
    case StreamState::Streaming: return "streaming";
    case StreamState::Closing: return "closing";
    case StreamState::Aborting: return "aborting";
    }
    return "?";
}

AvdtpSource::AvdtpSource(IL2capConnector& connector, BdAddr addr, SourceOptions opts, SourceEvents events)
    : conn_(connector), addr_(addr), opts_(std::move(opts)), ev_(std::move(events)) {}

AvdtpSource::~AvdtpSource() { Disconnect(); }

Status AvdtpSource::Connect()
{
    if (IsConnected()) return Status::Ok();
    L2capConnectParams p;
    p.addr = addr_;
    p.psm = kPsmAvdtp;
    p.inMtu = opts_.signalInMtu;
    auto ch = conn_.Connect(p);
    if (!ch) return ch.status();
    sig_ = std::move(*ch);
    stop_ = false;
    reasm_.Reset();
    rx_ = std::thread([this] { RxLoop(); });
    BTA_LOGI(kLog, "signalling channel open to {} via {} (out MTU {})", addr_.ToString(), conn_.Name(), sig_->OutMtu());
    return Status::Ok();
}

void AvdtpSource::Disconnect()
{
    stop_ = true;
    if (sig_) sig_->Close();
    CloseMedia();
    if (rx_.joinable() && rx_.get_id() != std::this_thread::get_id()) rx_.join();
    if (mediaAcceptThread_.joinable()) mediaAcceptThread_.join();
    {
        std::lock_guard lk(mu_);
        for (auto& [label, p] : pending_) p.done = true;  // wake waiters (msg stays empty)
        cv_.notify_all();
    }
    sig_.reset();
    ReleaseMedia();
    SetState(StreamState::Idle);
}

bool AvdtpSource::IsConnected() const { return sig_ && sig_->IsOpen() && !stop_; }

StreamState AvdtpSource::State() const
{
    std::lock_guard lk(mu_);
    return state_;
}

a2dp::StreamConfig AvdtpSource::Config() const
{
    std::lock_guard lk(mu_);
    return config_;
}

void AvdtpSource::SetState(StreamState s)
{
    bool changed;
    {
        std::lock_guard lk(mu_);
        changed = state_ != s;
        state_ = s;
    }
    if (changed) {
        BTA_LOGI(kLog, "stream state -> {}", StreamStateName(s));
        if (ev_.onStateChanged) ev_.onStateChanged(s);
    }
}

// Closes (but does not destroy) the media channel: the stream engine may still hold a raw pointer
// and be inside Send(); a closed channel fails fast with Err::Disconnected. The object is released
// in Disconnect()/ReleaseMedia() once the owner has stopped the engine.
void AvdtpSource::CloseMedia()
{
    std::lock_guard lk(mu_);
    if (media_) media_->Close();
}

void AvdtpSource::ReleaseMedia()
{
    std::unique_ptr<IL2capChannel> m;
    {
        std::lock_guard lk(mu_);
        m = std::move(media_);
    }
    if (m) m->Close();
}

Status AvdtpSource::SendMessage(const Message& m)
{
    if (!sig_) return {Err::Disconnected, "not connected"};
    std::lock_guard lk(sendMu_);
    for (auto& pkt : EncodeMessage(m, sig_->OutMtu())) {
        Status st = sig_->Send(pkt);
        if (!st) return st;
    }
    return Status::Ok();
}

Result<Message> AvdtpSource::Transact(Signal sig, std::vector<uint8_t> payload, uint32_t timeoutMs)
{
    if (!IsConnected()) return Status{Err::Disconnected, "signalling channel not connected"};
    Message m;
    m.type = MsgType::Command;
    m.signal = sig;
    m.payload = std::move(payload);
    {
        std::lock_guard lk(mu_);
        m.label = nextLabel_;
        nextLabel_ = static_cast<uint8_t>((nextLabel_ + 1) & 0x0F);
        pending_[m.label] = Pending{};
    }
    BTA_LOGD(kLog, "-> {} label={} len={}", SignalName(sig), m.label, m.payload.size());
    Status st = SendMessage(m);
    std::unique_lock lk(mu_);
    if (!st) {
        pending_.erase(m.label);
        return st;
    }
    uint32_t to = timeoutMs ? timeoutMs : opts_.signalTimeoutMs;
    bool ok = cv_.wait_for(lk, std::chrono::milliseconds(to), [&] { return pending_[m.label].done; });
    Pending p = std::move(pending_[m.label]);
    pending_.erase(m.label);
    if (!ok) return Status{Err::Timeout, std::string(SignalName(sig)) + " timed out"};
    if (stop_ || !sig_) return Status{Err::Disconnected, "disconnected while waiting"};
    if (p.msg.signal != sig) return Status{Err::ProtocolError, "response signal mismatch"};
    return std::move(p.msg);
}

Status AvdtpSource::StatusFromReply(const Message& reply, Signal sig)
{
    if (reply.type == MsgType::Accept) return Status::Ok();
    if (reply.type == MsgType::GeneralReject)
        return {Err::NotSupported, std::string(SignalName(sig)) + " general reject"};
    uint8_t code = reply.payload.empty() ? 0 : reply.payload.back();
    return {Err::Rejected, std::string(SignalName(sig)) + " rejected: " + ErrorName(code)};
}

void AvdtpSource::RxLoop()
{
    std::vector<uint8_t> pkt;
    while (!stop_) {
        Status st = sig_->Receive(pkt, 200);
        if (st.code() == Err::Timeout) continue;
        if (!st) {
            if (!stop_) {
                BTA_LOGW(kLog, "signalling channel lost: {}", st.ToString());
                stop_ = true;
                CloseMedia();
                {
                    std::lock_guard lk(mu_);
                    for (auto& [l, p] : pending_) p.done = true;
                    state_ = StreamState::Idle;
                }
                cv_.notify_all();
                if (ev_.onStateChanged) ev_.onStateChanged(StreamState::Idle);
                if (ev_.onDisconnected) ev_.onDisconnected(st);
            }
            return;
        }
        std::string err;
        auto msg = reasm_.Feed(pkt, &err);
        if (!err.empty()) {
            BTA_LOGW(kLog, "dropping malformed signalling packet: {}", err);
            continue;
        }
        if (!msg) continue;
        if (msg->type == MsgType::Command) {
            HandleCommand(*msg);
            continue;
        }
        std::lock_guard lk(mu_);
        auto it = pending_.find(msg->label);
        if (it == pending_.end() || it->second.done) {
            BTA_LOGW(kLog, "unsolicited response label={} {}", msg->label, SignalName(msg->signal));
            continue;
        }
        BTA_LOGD(kLog, "<- {} {} label={}", SignalName(msg->signal),
                 msg->type == MsgType::Accept ? "accept" : (msg->type == MsgType::Reject ? "reject" : "general-reject"),
                 msg->label);
        it->second.msg = std::move(*msg);
        it->second.done = true;
        cv_.notify_all();
    }
}

const LocalSep* AvdtpSource::FindLocal(uint8_t seid) const
{
    for (const auto& s : opts_.localSeps)
        if (s.seid == seid) return &s;
    return nullptr;
}

void AvdtpSource::Respond(const Message& cmd, MsgType type, std::vector<uint8_t> payload)
{
    Message r;
    r.label = cmd.label;
    r.type = type;
    r.signal = cmd.signal;
    r.payload = std::move(payload);
    Status st = SendMessage(r);
    if (!st) BTA_LOGW(kLog, "failed to respond to {}: {}", SignalName(cmd.signal), st.ToString());
}

// Acceptor side: commands initiated by the sink.
void AvdtpSource::HandleCommand(const Message& m)
{
    BTA_LOGD(kLog, "remote command {} label={}", SignalName(m.signal), m.label);
    auto acp = [&]() -> uint8_t { return m.payload.empty() ? 0 : SeidFromByte(m.payload[0]); };
    StreamState st = State();

    switch (m.signal) {
    case Signal::Discover: {
        std::vector<SepInfo> seps;
        for (const auto& l : opts_.localSeps)
            seps.push_back({l.seid, st != StreamState::Idle && l.seid == localSeid_, a2dp::kMediaTypeAudio, false});
        Respond(m, MsgType::Accept, BuildDiscoverResponse(seps));
        return;
    }
    case Signal::GetCapabilities:
    case Signal::GetAllCapabilities: {
        const LocalSep* l = FindLocal(acp());
        if (!l) return Respond(m, MsgType::Reject, {error::kBadAcpSeid});
        Capabilities c;
        c.mediaTransport = true;
        c.codecs.push_back(l->caps);
        c.delayReporting = (m.signal == Signal::GetAllCapabilities);
        Respond(m, MsgType::Accept, c.Serialize());
        return;
    }
    case Signal::SetConfiguration: {
        if (m.payload.size() < 2) return Respond(m, MsgType::Reject, {0x00, error::kBadLength});
        const LocalSep* l = FindLocal(acp());
        if (!l) return Respond(m, MsgType::Reject, {0x00, error::kBadAcpSeid});
        if (st != StreamState::Idle) return Respond(m, MsgType::Reject, {0x00, error::kSepInUse});
        uint8_t bad = 0;
        auto caps = Capabilities::Parse(std::span(m.payload).subspan(2), &bad, true);
        if (!caps) return Respond(m, MsgType::Reject, {bad, bad ? error::kBadServCategory : error::kBadPayloadFormat});
        if (!caps->mediaTransport || caps->codecs.size() != 1)
            return Respond(m, MsgType::Reject, {static_cast<uint8_t>(Category::MediaCodec), error::kInvalidCapabilities});
        if (caps->codecs[0].Identify() != l->caps.Identify())
            return Respond(m, MsgType::Reject, {static_cast<uint8_t>(Category::MediaCodec), error::kInvalidCodecType});
        auto cfg = a2dp::ConfigFromCapability(caps->codecs[0], opts_.prefs);
        if (!cfg) {
            BTA_LOGW(kLog, "rejecting sink configuration: {}", cfg.status().ToString());
            return Respond(m, MsgType::Reject, {static_cast<uint8_t>(Category::MediaCodec), error::kUnsupportedConfiguration});
        }
        {
            std::lock_guard lk(mu_);
            config_ = *cfg;
            localSeid_ = l->seid;
            remoteSeid_ = SeidFromByte(m.payload[1]);
            delayReportingCfg_ = caps->delayReporting;
        }
        Respond(m, MsgType::Accept);
        SetState(StreamState::Configured);
        BTA_LOGI(kLog, "sink configured stream: {}", cfg->Describe());
        if (ev_.onRemoteConfigured) ev_.onRemoteConfigured(*cfg);
        return;
    }
    case Signal::GetConfiguration: {
        if (st == StreamState::Idle || acp() != localSeid_) return Respond(m, MsgType::Reject, {error::kBadAcpSeid});
        Capabilities c;
        c.mediaTransport = true;
        c.codecs.push_back(Config().ToCapability());
        c.delayReporting = delayReportingCfg_;
        Respond(m, MsgType::Accept, c.Serialize());
        return;
    }
    case Signal::Reconfigure: {
        if (acp() != localSeid_) return Respond(m, MsgType::Reject, {0x00, error::kBadAcpSeid});
        if (st != StreamState::Open) return Respond(m, MsgType::Reject, {0x00, error::kBadState});
        uint8_t bad = 0;
        auto caps = Capabilities::Parse(std::span(m.payload).subspan(1), &bad, true);
        if (!caps || caps->codecs.size() != 1)
            return Respond(m, MsgType::Reject, {static_cast<uint8_t>(Category::MediaCodec), error::kInvalidCapabilities});
        auto cfg = a2dp::ConfigFromCapability(caps->codecs[0], opts_.prefs);
        if (!cfg || cfg->codec != Config().codec)
            return Respond(m, MsgType::Reject, {static_cast<uint8_t>(Category::MediaCodec), error::kUnsupportedConfiguration});
        {
            std::lock_guard lk(mu_);
            config_ = *cfg;
        }
        Respond(m, MsgType::Accept);
        if (ev_.onRemoteConfigured) ev_.onRemoteConfigured(*cfg);
        return;
    }
    case Signal::Open: {
        if (acp() != localSeid_) return Respond(m, MsgType::Reject, {error::kBadAcpSeid});
        if (st != StreamState::Configured) return Respond(m, MsgType::Reject, {error::kBadState});
        Respond(m, MsgType::Accept);
        SetState(StreamState::Open);
        // The INT (sink) now opens the media transport channel.
        if (mediaAcceptThread_.joinable()) mediaAcceptThread_.join();
        mediaAcceptThread_ = std::thread([this] {
            auto ch = conn_.Accept(addr_, kPsmAvdtp, 5000);
            if (!ch) {
                BTA_LOGW(kLog, "sink did not open media channel: {}", ch.status().ToString());
                return;
            }
            std::lock_guard lk(mu_);
            media_ = std::move(*ch);
        });
        return;
    }
    case Signal::Start: {
        if (acp() != localSeid_) return Respond(m, MsgType::Reject, {m.payload.empty() ? uint8_t(0) : m.payload[0], error::kBadAcpSeid});
        if (st != StreamState::Open && st != StreamState::Streaming)
            return Respond(m, MsgType::Reject, {m.payload[0], error::kBadState});
        Respond(m, MsgType::Accept);
        SetState(StreamState::Streaming);
        if (ev_.onRemoteStartSuspend) ev_.onRemoteStartSuspend(true);
        return;
    }
    case Signal::Suspend: {
        if (acp() != localSeid_) return Respond(m, MsgType::Reject, {m.payload.empty() ? uint8_t(0) : m.payload[0], error::kBadAcpSeid});
        if (st != StreamState::Streaming) return Respond(m, MsgType::Reject, {m.payload[0], error::kBadState});
        Respond(m, MsgType::Accept);
        SetState(StreamState::Open);
        if (ev_.onRemoteStartSuspend) ev_.onRemoteStartSuspend(false);
        return;
    }
    case Signal::Close: {
        if (acp() != localSeid_ || st == StreamState::Idle) return Respond(m, MsgType::Reject, {error::kBadAcpSeid});
        Respond(m, MsgType::Accept);
        CloseMedia();
        SetState(StreamState::Idle);
        return;
    }
    case Signal::Abort: {
        Respond(m, MsgType::Accept);  // ABORT is always accepted
        CloseMedia();
        SetState(StreamState::Idle);
        return;
    }
    case Signal::DelayReport: {
        if (m.payload.size() != 3) return Respond(m, MsgType::Reject, {error::kBadLength});
        if (acp() != localSeid_) return Respond(m, MsgType::Reject, {error::kBadAcpSeid});
        uint16_t d = static_cast<uint16_t>((m.payload[1] << 8) | m.payload[2]);
        delay_ = d;
        Respond(m, MsgType::Accept);
        BTA_LOGI(kLog, "sink delay report: {:.1f} ms", d / 10.0);
        if (ev_.onDelayReport) ev_.onDelayReport(d);
        return;
    }
    default: {
        Message r;
        r.label = m.label;
        r.type = MsgType::GeneralReject;
        r.signal = m.signal;
        SendMessage(r);
        return;
    }
    }
}

// ---------------------------------------------------------------------------
// Initiator procedures.

Result<std::vector<SepInfo>> AvdtpSource::Discover()
{
    auto r = Transact(Signal::Discover, {});
    if (!r) return r.status();
    if (Status st = StatusFromReply(*r, Signal::Discover); !st) return st;
    auto seps = ParseDiscoverResponse(r->payload);
    if (!seps) return Status{Err::ProtocolError, "malformed DISCOVER response"};
    return *seps;
}

Result<Capabilities> AvdtpSource::GetCapabilities(uint8_t remoteSeid)
{
    Result<Message> r = Status{Err::NotSupported, ""};
    if (opts_.useGetAllCapabilities) {
        r = Transact(Signal::GetAllCapabilities, {SeidByte(remoteSeid)});
        if (r && r->type != MsgType::Accept) {
            BTA_LOGI(kLog, "GET_ALL_CAPABILITIES not accepted, falling back to GET_CAPABILITIES");
            r = Status{Err::NotSupported, ""};
        }
    }
    if (!r || r->type != MsgType::Accept) {
        if (r.status().code() == Err::Timeout || r.status().code() == Err::Disconnected) return r.status();
        r = Transact(Signal::GetCapabilities, {SeidByte(remoteSeid)});
        if (!r) return r.status();
        if (Status st = StatusFromReply(*r, Signal::GetCapabilities); !st) return st;
    }
    auto caps = Capabilities::Parse(r->payload, nullptr, false);
    if (!caps) return Status{Err::ProtocolError, "malformed capabilities"};
    return *caps;
}

Result<std::vector<RemoteSep>> AvdtpSource::DiscoverAll()
{
    auto seps = Discover();
    if (!seps) return seps.status();
    std::vector<RemoteSep> out;
    for (const auto& s : *seps) {
        if (!s.isSink || s.mediaType != a2dp::kMediaTypeAudio) continue;
        RemoteSep rs;
        rs.info = s;
        auto caps = GetCapabilities(s.seid);
        if (caps) {
            rs.capsValid = true;
            rs.caps = std::move(*caps);
            for (const auto& c : rs.caps.codecs)
                BTA_LOGI(kLog, "sink SEP {}{}: {}{}", s.seid, s.inUse ? " (in use)" : "", c.Describe(),
                         rs.caps.delayReporting ? " +delay-reporting" : "");
        } else {
            if (caps.status().code() == Err::Disconnected || caps.status().code() == Err::Timeout) return caps.status();
            rs.capsError = caps.status().ToString();
            BTA_LOGW(kLog, "sink SEP {} capabilities failed: {}", s.seid, rs.capsError);
        }
        out.push_back(std::move(rs));
    }
    return out;
}

Status AvdtpSource::SetConfiguration(uint8_t remoteSeid, uint8_t localSeid, const a2dp::StreamConfig& cfg, bool delayReporting)
{
    if (State() != StreamState::Idle) return {Err::StateError, "stream already configured"};
    Capabilities c;
    c.mediaTransport = true;
    c.codecs.push_back(cfg.ToCapability());
    c.delayReporting = delayReporting;
    std::vector<uint8_t> payload{SeidByte(remoteSeid), SeidByte(localSeid)};
    auto body = c.Serialize();
    payload.insert(payload.end(), body.begin(), body.end());
    auto r = Transact(Signal::SetConfiguration, std::move(payload));
    if (!r) return r.status();
    if (Status st = StatusFromReply(*r, Signal::SetConfiguration); !st) return st;
    {
        std::lock_guard lk(mu_);
        config_ = cfg;
        remoteSeid_ = remoteSeid;
        localSeid_ = localSeid;
        delayReportingCfg_ = delayReporting;
    }
    SetState(StreamState::Configured);
    BTA_LOGI(kLog, "configured remote SEP {}: {}", remoteSeid, cfg.Describe());
    return Status::Ok();
}

Status AvdtpSource::Reconfigure(const a2dp::StreamConfig& cfg)
{
    if (State() != StreamState::Open) return {Err::StateError, "RECONFIGURE requires Open state"};
    Capabilities c;
    c.codecs.push_back(cfg.ToCapability());
    std::vector<uint8_t> payload{SeidByte(remoteSeid_)};
    auto body = c.Serialize();
    payload.insert(payload.end(), body.begin(), body.end());
    auto r = Transact(Signal::Reconfigure, std::move(payload));
    if (!r) return r.status();
    if (Status st = StatusFromReply(*r, Signal::Reconfigure); !st) return st;
    std::lock_guard lk(mu_);
    config_ = cfg;
    return Status::Ok();
}

Status AvdtpSource::Open()
{
    if (State() != StreamState::Configured) return {Err::StateError, "OPEN requires Configured state"};
    auto r = Transact(Signal::Open, {SeidByte(remoteSeid_)});
    if (!r) return r.status();
    if (Status st = StatusFromReply(*r, Signal::Open); !st) return st;
    SetState(StreamState::Open);
    L2capConnectParams p;
    p.addr = addr_;
    p.psm = kPsmAvdtp;
    p.inMtu = opts_.mediaInMtu;
    p.flushTimeoutMs = opts_.mediaFlushTimeoutMs;
    auto ch = conn_.Connect(p);
    if (!ch) {
        BTA_LOGE(kLog, "media channel connect failed: {}", ch.status().ToString());
        Abort();
        return ch.status();
    }
    ReleaseMedia();  // previous channel (if any) belongs to a stopped stream
    {
        std::lock_guard lk(mu_);
        media_ = std::move(*ch);
    }
    BTA_LOGI(kLog, "media channel open (out MTU {})", media_->OutMtu());
    return Status::Ok();
}

Status AvdtpSource::Start()
{
    StreamState s = State();
    if (s == StreamState::Streaming) return Status::Ok();
    if (s != StreamState::Open) return {Err::StateError, "START requires Open state"};
    auto r = Transact(Signal::Start, {SeidByte(remoteSeid_)});
    if (!r) return r.status();
    if (Status st = StatusFromReply(*r, Signal::Start); !st) return st;
    SetState(StreamState::Streaming);
    return Status::Ok();
}

Status AvdtpSource::Suspend()
{
    if (State() != StreamState::Streaming) return Status::Ok();
    auto r = Transact(Signal::Suspend, {SeidByte(remoteSeid_)});
    if (!r) return r.status();
    if (Status st = StatusFromReply(*r, Signal::Suspend); !st) return st;
    SetState(StreamState::Open);
    return Status::Ok();
}

Status AvdtpSource::Close()
{
    StreamState s = State();
    if (s == StreamState::Idle) return Status::Ok();
    SetState(StreamState::Closing);
    auto r = Transact(Signal::Close, {SeidByte(remoteSeid_)});
    CloseMedia();
    SetState(StreamState::Idle);
    if (!r) return r.status();
    return StatusFromReply(*r, Signal::Close);
}

Status AvdtpSource::Abort()
{
    if (State() == StreamState::Idle) return Status::Ok();
    SetState(StreamState::Aborting);
    auto r = Transact(Signal::Abort, {SeidByte(remoteSeid_)}, 1000);
    CloseMedia();
    SetState(StreamState::Idle);
    if (!r) return r.status();
    return StatusFromReply(*r, Signal::Abort);
}

}  // namespace bta::avdtp
