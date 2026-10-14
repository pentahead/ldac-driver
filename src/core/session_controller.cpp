#include "core/session_controller.h"

#include <chrono>
#include <format>

#include "common/log.h"

namespace bta {

static constexpr const char* kLog = "session";
static constexpr uint8_t kLocalSbcSeid = 1, kLocalLdacSeid = 2;

const char* SessionStateName(SessionState s)
{
    switch (s) {
    case SessionState::Idle: return "idle";
    case SessionState::Connecting: return "connecting";
    case SessionState::Negotiating: return "negotiating";
    case SessionState::Streaming: return "streaming";
    case SessionState::Suspended: return "suspended";
    case SessionState::Error: return "error";
    }
    return "?";
}

Json SessionStatus::ToJson() const
{
    Json j = Json::MakeObject();
    j["state"] = SessionStateName(state);
    j["device"] = device;
    j["transport"] = transport;
    Json sc = Json::MakeArray();
    for (const auto& c : sinkCodecs) {
        Json e = Json::MakeObject();
        e["seid"] = static_cast<int>(c.seid);
        e["codec"] = CodecName(c.codec);
        e["description"] = c.description;
        e["usable"] = c.usable;
        sc.Push(e);
    }
    j["sinkCodecs"] = sc;
    j["sinkDelayReporting"] = sinkDelayReporting;
    j["configuration"] = configuration;
    j["activeCodec"] = codecConfirmed ? CodecName(activeCodec) : "";
    j["codecConfirmed"] = codecConfirmed;
    j["fallbackReason"] = fallbackReason;
    j["lastError"] = lastError;
    j["sinkDelayMs"] = sinkDelayMs;
    j["reconnects"] = reconnects;
    j["restartRequired"] = restartRequired;
    Json e = Json::MakeObject();
    e["running"] = engine.running;
    e["codec"] = CodecName(engine.codec);
    e["packets"] = engine.packets;
    e["pcmFramesSent"] = engine.pcmFramesSent;
    e["bytes"] = engine.bytes;
    e["codecFrames"] = engine.codecFrames;
    e["silenceFramesInserted"] = engine.silenceFramesInserted;
    e["droppedSourceFrames"] = engine.droppedSourceFrames;
    e["underrunEvents"] = engine.underrunEvents;
    e["sendErrors"] = engine.sendErrors;
    e["measuredKbps"] = engine.measuredKbps;
    e["nominalKbps"] = engine.nominalBitrate / 1000.0;
    e["quality"] = engine.quality;
    e["encodeAvgUs"] = engine.encodeAvgUs;
    e["encodeMaxUs"] = engine.encodeMaxUs;
    e["sinkBufferMs"] = engine.sinkBufferMs;
    e["queueDepth"] = engine.queueDepth;
    e["runtimeMs"] = engine.runtimeMs;
    j["engine"] = e;
    return j;
}

SessionController::SessionController(ControllerDeps deps, Settings settings)
    : deps_(std::move(deps)), settings_(std::move(settings))
{
    actor_ = std::thread([this] { ActorLoop(); });
}

SessionController::~SessionController()
{
    Post([this] { DoDisconnect("shutdown"); });
    {
        std::lock_guard lk(qmu_);
        quit_ = true;
    }
    qcv_.notify_all();
    actor_.join();
}

void SessionController::Post(std::function<void()> fn)
{
    {
        std::lock_guard lk(qmu_);
        queue_.push_back(std::move(fn));
    }
    qcv_.notify_all();
}

void SessionController::ActorLoop()
{
    for (;;) {
        std::function<void()> fn;
        {
            std::unique_lock lk(qmu_);
            qcv_.wait_for(lk, std::chrono::milliseconds(250), [&] { return quit_ || !queue_.empty(); });
            if (queue_.empty()) {
                if (quit_) return;
                lk.unlock();
                RefreshConfirmation();
                continue;
            }
            fn = std::move(queue_.front());
            queue_.pop_front();
        }
        fn();
        RefreshConfirmation();
    }
}

void SessionController::Connect(BdAddr addr)
{
    Post([this, addr] {
        reconnectAttempts_ = 0;
        DoConnect(addr, false);
    });
}

void SessionController::Disconnect()
{
    Post([this] { DoDisconnect("user request"); });
}

Settings SessionController::CurrentSettings() const
{
    std::lock_guard lk(mu_);
    return settings_;
}

SessionStatus SessionController::GetStatus() const
{
    std::lock_guard lk(mu_);
    return status_;
}

bool SessionController::WaitFor(const std::function<bool(const SessionStatus&)>& pred, uint32_t timeoutMs) const
{
    std::unique_lock lk(mu_);
    return statusCv_.wait_for(lk, std::chrono::milliseconds(timeoutMs), [&] { return pred(status_); });
}

void SessionController::SetState(SessionState s, const std::string& err)
{
    {
        std::lock_guard lk(mu_);
        if (status_.state != s) BTA_LOGI(kLog, "session state -> {}{}", SessionStateName(s), err.empty() ? "" : " (" + err + ")");
        status_.state = s;
        if (!err.empty()) status_.lastError = err;
        if (s != SessionState::Streaming) {
            status_.codecConfirmed = false;
            status_.activeCodec = CodecId::None;
        }
    }
    statusCv_.notify_all();
}

// Active codec is reported only when the encoder runs AND packets have been accepted by the transport.
void SessionController::RefreshConfirmation()
{
    EngineStats es;
    bool haveEngine = engine_ != nullptr;
    if (haveEngine) es = engine_->Stats();
    {
        std::lock_guard lk(mu_);
        if (haveEngine) status_.engine = es;
        bool confirmed = haveEngine && es.running && es.packets > 0 && status_.state == SessionState::Streaming &&
                         avdtp_ && avdtp_->State() == avdtp::StreamState::Streaming;
        status_.codecConfirmed = confirmed;
        status_.activeCodec = confirmed ? es.codec : CodecId::None;
        if (avdtp_) status_.sinkDelayMs = avdtp_->LastDelayReport() / 10.0;
    }
    statusCv_.notify_all();
}

a2dp::NegotiationPrefs SessionController::Prefs() const
{
    std::lock_guard lk(mu_);
    a2dp::NegotiationPrefs p;
    p.preference = settings_.codec;
    p.sampleRate = settings_.sampleRate;
    p.bitsPerSample = settings_.bitsPerSample;
    p.ldacQuality = settings_.ldacQuality;
    p.sbc = settings_.sbc;
    p.sourceRate = source_ ? source_->Format().sampleRate : 48000;
    return p;
}

void SessionController::DoConnect(BdAddr addr, bool isReconnect)
{
    DoDisconnect("reconnect");
    ++generation_;
    addr_ = addr;
    {
        std::lock_guard lk(mu_);
        uint32_t rc = status_.reconnects;
        status_ = {};
        status_.device = addr.ToString();
        status_.transport = deps_.connector ? deps_.connector->Name() : "none";
        status_.reconnects = rc + (isReconnect ? 1 : 0);
    }
    SetState(SessionState::Connecting);

    if (!deps_.connector) return SetState(SessionState::Error, "no transport configured");
    if (Status st = deps_.connector->CheckAvailable(addr); !st) return SetState(SessionState::Error, st.ToString());

    // Source first: its native rate guides sample-rate negotiation.
    Settings s = CurrentSettings();
    if (!deps_.sourceFactory) return SetState(SessionState::Error, "no audio source configured");
    auto src = deps_.sourceFactory(s);
    if (!src) return SetState(SessionState::Error, "audio source: " + src.status().ToString());
    source_ = std::move(*src);
    if (Status st = source_->Start(); !st) return SetState(SessionState::Error, "audio source start: " + st.ToString());

    avdtp::SourceOptions so;
    so.signalTimeoutMs = deps_.signalTimeoutMs;
    so.localSeps.push_back({kLocalSbcSeid, a2dp::LocalSbcCaps().ToCapability()});
    if (deps_.encoderAvailable(CodecId::Ldac, nullptr))
        so.localSeps.push_back({kLocalLdacSeid, a2dp::LocalLdacCaps().ToCapability()});
    so.prefs = Prefs();
    const uint64_t gen = generation_;
    avdtp::SourceEvents ev;
    ev.onDisconnected = [this, gen](const bta::Status& st) {
        Post([this, gen, st] {
            if (gen == generation_) OnLinkLost(st);
        });
    };
    ev.onRemoteStartSuspend = [this, gen](bool streaming) {
        Post([this, gen, streaming] {
            if (gen != generation_ || !engine_) return;
            if (!streaming) {
                engine_->Stop();
                SetState(SessionState::Suspended);
            } else if (!engine_->Running()) {
                auto enc = engine_->TakeEncoder();
                engine_ = std::make_unique<StreamEngine>(deps_.engine);
                IPcmSource* srcp = source_.get();
                Status st = engine_->Start(std::move(enc), srcp, avdtp_->Media(), cfg_, [this, gen](const bta::Status& e) {
                    Post([this, gen, e] {
                        if (gen == generation_) OnEngineFatal(e);
                    });
                });
                SetState(st ? SessionState::Streaming : SessionState::Error, st ? "" : st.ToString());
            }
        });
    };
    avdtp_ = std::make_unique<avdtp::AvdtpSource>(*deps_.connector, addr, so, ev);

    if (Status st = avdtp_->Connect(); !st) {
        SetState(SessionState::Error, "connect: " + st.ToString());
        if (reconnectAttempts_ < deps_.maxReconnectAttempts) {
            ++reconnectAttempts_;
            BTA_LOGI(kLog, "retrying in {} ms (attempt {}/{})", deps_.reconnectDelayMs, reconnectAttempts_,
                     deps_.maxReconnectAttempts);
            std::this_thread::sleep_for(std::chrono::milliseconds(deps_.reconnectDelayMs));
            Post([this, addr] { DoConnect(addr, true); });
        }
        return;
    }
    SetState(SessionState::Negotiating);
    if (NegotiateAndStream()) reconnectAttempts_ = 0;
}

bool SessionController::NegotiateAndStream()
{
    auto seps = avdtp_->DiscoverAll();
    if (!seps) {
        SetState(SessionState::Error, "discovery: " + seps.status().ToString());
        return false;
    }
    remoteSeps_ = std::move(*seps);
    a2dp::NegotiationPrefs prefs = Prefs();

    std::string ldacReason;
    bool ldacEncoder = deps_.encoderAvailable(CodecId::Ldac, &ldacReason);
    struct Candidate {
        a2dp::StreamConfig cfg;
        uint8_t seid;
        bool delay;
    };
    std::optional<Candidate> ldac, sbc;
    std::vector<SinkCodecInfo> infos;
    for (const auto& rs : remoteSeps_) {
        if (!rs.capsValid) continue;
        for (const auto& cc : rs.caps.codecs) {
            CodecId id = cc.Identify();
            SinkCodecInfo info{rs.info.seid, id, cc.Describe(), false};
            if (rs.info.inUse) info.description += " [in use]";
            if (id == CodecId::Ldac && !rs.info.inUse) {
                if (auto lc = a2dp::LdacCaps::Parse(cc.info)) {
                    auto r = a2dp::NegotiateLdac(*lc, prefs);
                    info.usable = r.ok() && ldacEncoder;
                    if (r && !ldac) ldac = Candidate{*r, rs.info.seid, rs.caps.delayReporting};
                    if (!r) ldacReason = r.status().message();
                }
            } else if (id == CodecId::Sbc && !rs.info.inUse) {
                if (auto sc = a2dp::SbcCaps::Parse(cc.info)) {
                    auto r = a2dp::NegotiateSbc(*sc, prefs);
                    info.usable = r.ok();
                    if (r && !sbc) sbc = Candidate{*r, rs.info.seid, rs.caps.delayReporting};
                }
            }
            infos.push_back(info);
        }
    }
    {
        std::lock_guard lk(mu_);
        status_.sinkCodecs = infos;
    }

    std::string fallback;
    std::optional<Candidate> chosen;
    if (prefs.preference == CodecPreference::Sbc) {
        chosen = sbc;
        fallback = "SBC selected by user preference";
    } else if (!ldac) {
        chosen = sbc;
        fallback = ldacReason.empty() ? "sink does not advertise LDAC" : "LDAC unusable: " + ldacReason;
    } else if (!ldacEncoder) {
        chosen = sbc;
        fallback = "LDAC encoder unavailable: " + ldacReason;
    } else {
        // Preflight the encoder with the minimum LDAC MTU so init problems are caught before configuring.
        auto probe = deps_.encoderFactory(CodecId::Ldac);
        Status pst = probe ? probe->Open(ldac->cfg, 1005) : bta::Status{Err::NotAvailable, "factory returned null"};
        if (pst) {
            chosen = ldac;
        } else {
            chosen = sbc;
            fallback = "LDAC encoder init failed: " + pst.message();
        }
    }
    if (!chosen) {
        SetState(SessionState::Error, "no usable codec (sink offers no compatible SBC endpoint)");
        return false;
    }
    if (!fallback.empty()) BTA_LOGW(kLog, "using SBC: {}", fallback);
    {
        std::lock_guard lk(mu_);
        status_.fallbackReason = fallback;
        status_.sinkDelayReporting = chosen->delay;
    }

    Status st = ConfigureAndOpen(chosen->cfg, chosen->seid, chosen->delay);
    if (!st && chosen->cfg.codec == CodecId::Ldac && sbc) {
        std::string why = "sink rejected LDAC configuration: " + st.message();
        BTA_LOGW(kLog, "{}; falling back to SBC", why);
        {
            std::lock_guard lk(mu_);
            status_.fallbackReason = why;
        }
        avdtp_->Abort();
        chosen = sbc;
        st = ConfigureAndOpen(chosen->cfg, chosen->seid, chosen->delay);
    }
    if (!st) {
        SetState(SessionState::Error, "configure: " + st.ToString());
        return false;
    }

    st = StartEngine(chosen->cfg);
    if (!st && chosen->cfg.codec == CodecId::Ldac && sbc) {
        FallbackToSbc("LDAC start failed: " + st.message());
        return true;
    }
    if (!st) {
        SetState(SessionState::Error, "start: " + st.ToString());
        return false;
    }
    return true;
}

Status SessionController::ConfigureAndOpen(const a2dp::StreamConfig& cfg, uint8_t remoteSeid, bool delayReporting)
{
    uint8_t local = cfg.codec == CodecId::Ldac ? kLocalLdacSeid : kLocalSbcSeid;
    if (Status st = avdtp_->SetConfiguration(remoteSeid, local, cfg, delayReporting); !st) return st;
    if (Status st = avdtp_->Open(); !st) return st;
    cfg_ = cfg;
    std::lock_guard lk(mu_);
    status_.configuration = cfg.Describe();
    return Status::Ok();
}

Status SessionController::StartEngine(const a2dp::StreamConfig& cfg)
{
    auto enc = deps_.encoderFactory(cfg.codec);
    if (!enc) return {Err::NotAvailable, std::string("no encoder for ") + CodecName(cfg.codec)};
    IL2capChannel* media = avdtp_->Media();
    if (!media) return {Err::StateError, "media channel not open"};
    if (Status st = enc->Open(cfg, media->OutMtu()); !st) return st;
    if (Status st = avdtp_->Start(); !st) return st;
    engine_ = std::make_unique<StreamEngine>(deps_.engine);
    const uint64_t gen = generation_;
    Status st = engine_->Start(std::move(enc), source_.get(), media, cfg, [this, gen](const bta::Status& e) {
        Post([this, gen, e] {
            if (gen == generation_) OnEngineFatal(e);
        });
    });
    if (!st) return st;
    SetState(SessionState::Streaming);
    return Status::Ok();
}

void SessionController::FallbackToSbc(const std::string& reason)
{
    BTA_LOGW(kLog, "falling back to SBC: {}", reason);
    {
        std::lock_guard lk(mu_);
        status_.fallbackReason = reason;
    }
    if (engine_) engine_->Stop();
    engine_.reset();
    std::optional<std::pair<a2dp::StreamConfig, avdtp::RemoteSep>> sbc;
    a2dp::NegotiationPrefs prefs = Prefs();
    for (const auto& rs : remoteSeps_)
        for (const auto& cc : rs.caps.codecs)
            if (cc.Identify() == CodecId::Sbc && !sbc)
                if (auto sc = a2dp::SbcCaps::Parse(cc.info))
                    if (auto r = a2dp::NegotiateSbc(*sc, prefs)) sbc = std::make_pair(*r, rs);
    if (!sbc) return SetState(SessionState::Error, "fallback impossible: no SBC endpoint");
    avdtp_->Suspend();
    Status st = avdtp_->Close();
    if (!st) avdtp_->Abort();
    st = ConfigureAndOpen(sbc->first, sbc->second.info.seid, sbc->second.caps.delayReporting);
    if (st) st = StartEngine(sbc->first);
    if (!st) SetState(SessionState::Error, "SBC fallback failed: " + st.ToString());
}

void SessionController::OnEngineFatal(const bta::Status& st)
{
    if (st.code() == Err::Disconnected) return OnLinkLost(st);
    if (st.code() == Err::EncoderError && cfg_.codec == CodecId::Ldac) return FallbackToSbc("LDAC encoder error: " + st.message());
    if (engine_) engine_->Stop();
    SetState(SessionState::Error, "stream: " + st.ToString());
}

void SessionController::OnLinkLost(const bta::Status& st)
{
    BTA_LOGW(kLog, "link lost: {}", st.ToString());
    DoDisconnect("link lost");
    SetState(SessionState::Error, "link lost: " + st.ToString());
    if (reconnectAttempts_ < deps_.maxReconnectAttempts) {
        ++reconnectAttempts_;
        BdAddr a = addr_;
        std::this_thread::sleep_for(std::chrono::milliseconds(deps_.reconnectDelayMs));
        Post([this, a] { DoConnect(a, true); });
    }
}

void SessionController::DoDisconnect(const std::string& reason)
{
    ++generation_;
    if (engine_) engine_->Stop();
    engine_.reset();
    if (avdtp_) {
        if (avdtp_->IsConnected() && avdtp_->State() != avdtp::StreamState::Idle) {
            avdtp_->Suspend();
            avdtp_->Close();
        }
        avdtp_->Disconnect();
        avdtp_.reset();
        BTA_LOGI(kLog, "disconnected ({})", reason);
    }
    if (source_) source_->Stop();
    source_.reset();
    remoteSeps_.clear();
    bool wasActive;
    {
        std::lock_guard lk(mu_);
        wasActive = status_.state != SessionState::Idle && status_.state != SessionState::Error;
        status_.engine.running = false;
    }
    if (wasActive) SetState(SessionState::Idle);
}

void SessionController::UpdateSettings(const Settings& s)
{
    Post([this, s] {
        Settings old;
        {
            std::lock_guard lk(mu_);
            old = settings_;
            settings_ = s;
        }
        bool live = engine_ && engine_->Running();
        bool needRestart = old.codec != s.codec || old.sampleRate != s.sampleRate || old.bitsPerSample != s.bitsPerSample ||
                           old.source != s.source || old.sourceEndpointId != s.sourceEndpointId ||
                           old.sbc.blocks != s.sbc.blocks || old.sbc.subbands != s.sbc.subbands || old.sbc.loudness != s.sbc.loudness;
        if (live && old.ldacQuality != s.ldacQuality && cfg_.codec == CodecId::Ldac) {
            if (s.ldacQuality == LdacQuality::Adaptive) {
                needRestart = true;  // ABR handle is created at encoder open
            } else {
                Status st = engine_->SetQuality(static_cast<int>(s.ldacQuality));
                BTA_LOGI(kLog, "live LDAC quality change -> {}: {}", LdacQualityName(s.ldacQuality), st.ToString());
                if (!st) needRestart = true;
            }
        }
        if (live && old.sbc.maxBitpool != s.sbc.maxBitpool && cfg_.codec == CodecId::Sbc) {
            int bp = std::min<int>(s.sbc.maxBitpool, cfg_.sbc.maxBitpool);
            if (!engine_->SetQuality(bp)) needRestart = true;
        }
        std::lock_guard lk(mu_);
        status_.restartRequired = live && needRestart;
    });
}

}  // namespace bta
