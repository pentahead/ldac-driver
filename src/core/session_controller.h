// Orchestrates one A2DP source session: transport connect, AVDTP negotiation, codec selection with
// LDAC -> SBC fallback, the stream engine and live settings. All work runs on an internal actor
// thread; public methods are thread-safe and non-blocking (except WaitForState in tests).
#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

#include "a2dp/avdtp_source.h"
#include "codec/encoder.h"
#include "common/json.h"
#include "common/settings.h"
#include "pipeline/stream_engine.h"

namespace bta {

enum class SessionState { Idle, Connecting, Negotiating, Streaming, Suspended, Error };
const char* SessionStateName(SessionState s);

struct SinkCodecInfo {
    uint8_t seid;
    CodecId codec;
    std::string description;
    bool usable;  // we have an encoder and the parameters intersect
};

struct SessionStatus {
    SessionState state = SessionState::Idle;
    std::string device;
    std::string transport;
    std::vector<SinkCodecInfo> sinkCodecs;
    bool sinkDelayReporting = false;
    std::string configuration;              // negotiated configuration
    CodecId activeCodec = CodecId::None;    // only set when packets are confirmed on the media channel
    bool codecConfirmed = false;
    std::string fallbackReason;             // why LDAC was not used (if applicable)
    std::string lastError;
    double sinkDelayMs = 0;
    uint32_t reconnects = 0;
    bool restartRequired = false;           // settings changed that need renegotiation
    EngineStats engine;

    Json ToJson() const;
};

using SourceFactory = std::function<Result<std::unique_ptr<IPcmSource>>(const Settings&)>;

struct ControllerDeps {
    IL2capConnector* connector = nullptr;
    EncoderFactory encoderFactory = CreateEncoder;
    std::function<bool(CodecId, std::string*)> encoderAvailable = IsEncoderAvailable;
    SourceFactory sourceFactory;
    EngineOptions engine;
    uint32_t signalTimeoutMs = 3000;
    int maxReconnectAttempts = 0;
    uint32_t reconnectDelayMs = 2000;
};

class SessionController {
public:
    SessionController(ControllerDeps deps, Settings settings);
    ~SessionController();

    void Connect(BdAddr addr);
    void Disconnect();
    void UpdateSettings(const Settings& s);
    Settings CurrentSettings() const;
    SessionStatus GetStatus() const;
    // Test helper: waits until pred(status) is true or the timeout elapses.
    bool WaitFor(const std::function<bool(const SessionStatus&)>& pred, uint32_t timeoutMs) const;

private:
    void Post(std::function<void()> fn);
    void ActorLoop();
    void DoConnect(BdAddr addr, bool isReconnect);
    void DoDisconnect(const std::string& reason);
    bool NegotiateAndStream();
    Status ConfigureAndOpen(const a2dp::StreamConfig& cfg, uint8_t remoteSeid, bool delayReporting);
    Status StartEngine(const a2dp::StreamConfig& cfg);
    void FallbackToSbc(const std::string& reason);
    void OnEngineFatal(const bta::Status& st);
    void OnLinkLost(const bta::Status& st);
    void SetState(SessionState s, const std::string& err = {});
    a2dp::NegotiationPrefs Prefs() const;
    void RefreshConfirmation();

    ControllerDeps deps_;
    Settings settings_;

    mutable std::mutex mu_;  // guards status_, settings_
    mutable std::condition_variable statusCv_;
    SessionStatus status_;

    std::mutex qmu_;
    std::condition_variable qcv_;
    std::deque<std::function<void()>> queue_;
    bool quit_ = false;
    std::thread actor_;

    // Actor-thread-only state.
    BdAddr addr_;
    std::unique_ptr<avdtp::AvdtpSource> avdtp_;
    std::vector<avdtp::RemoteSep> remoteSeps_;
    std::unique_ptr<IPcmSource> source_;
    std::unique_ptr<StreamEngine> engine_;
    a2dp::StreamConfig cfg_;
    uint64_t generation_ = 0;  // invalidates callbacks from torn-down sessions
    int reconnectAttempts_ = 0;
};

}  // namespace bta
