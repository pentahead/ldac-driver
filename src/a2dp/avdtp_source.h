// AVDTP source-role session over an IL2capConnector (one signalling channel, one media channel).
#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <thread>

#include "a2dp/avdtp.h"
#include "transport/l2cap.h"

namespace bta::avdtp {

enum class StreamState { Idle, Configured, Open, Streaming, Closing, Aborting };
const char* StreamStateName(StreamState s);

struct RemoteSep {
    SepInfo info;
    bool capsValid = false;
    Capabilities caps;
    std::string capsError;
};

struct LocalSep {
    uint8_t seid;
    a2dp::CodecCapability caps;
};

struct SourceOptions {
    uint32_t signalTimeoutMs = 3000;
    uint16_t signalInMtu = kDefaultSignalMtu;
    uint16_t mediaInMtu = 1024;
    uint16_t mediaFlushTimeoutMs = 0;
    bool useGetAllCapabilities = true;  // AVDTP 1.3; falls back to GET_CAPABILITIES on reject
    std::vector<LocalSep> localSeps;    // advertised to sink-initiated procedures
    a2dp::NegotiationPrefs prefs;       // validates sink-initiated configuration
};

struct SourceEvents {
    std::function<void(StreamState)> onStateChanged;
    std::function<void(uint16_t delayTenthMs)> onDelayReport;
    std::function<void(bool streaming)> onRemoteStartSuspend;
    std::function<void(const a2dp::StreamConfig&)> onRemoteConfigured;
    std::function<void(const Status&)> onDisconnected;
};

class AvdtpSource {
public:
    AvdtpSource(IL2capConnector& connector, BdAddr addr, SourceOptions opts, SourceEvents events);
    ~AvdtpSource();
    AvdtpSource(const AvdtpSource&) = delete;
    AvdtpSource& operator=(const AvdtpSource&) = delete;

    Status Connect();     // opens the signalling channel
    void Disconnect();    // closes everything without signalling

    Result<std::vector<SepInfo>> Discover();
    Result<Capabilities> GetCapabilities(uint8_t remoteSeid);
    Result<std::vector<RemoteSep>> DiscoverAll();  // DISCOVER + capabilities of every audio SNK SEP

    Status SetConfiguration(uint8_t remoteSeid, uint8_t localSeid, const a2dp::StreamConfig& cfg, bool delayReporting);
    Status Reconfigure(const a2dp::StreamConfig& cfg);  // valid in Open state
    Status Open();     // OPEN, then connect the media transport channel
    Status Start();
    Status Suspend();
    Status Close();
    Status Abort();

    // Valid until Disconnect() or the next Open(); callers must stop using it before either.
    IL2capChannel* Media() const { return media_.get(); }
    StreamState State() const;
    bool IsConnected() const;
    a2dp::StreamConfig Config() const;
    uint8_t RemoteSeid() const { return remoteSeid_; }
    uint16_t LastDelayReport() const { return delay_.load(); }
    bool DelayReportingConfigured() const { return delayReportingCfg_; }

private:
    struct Pending {
        bool done = false;
        Message msg;
    };

    Result<Message> Transact(Signal sig, std::vector<uint8_t> payload, uint32_t timeoutMs = 0);
    Status SendMessage(const Message& m);
    void RxLoop();
    void HandleCommand(const Message& m);
    void Respond(const Message& cmd, MsgType type, std::vector<uint8_t> payload = {});
    void SetState(StreamState s);
    void CloseMedia();
    void ReleaseMedia();
    Status StatusFromReply(const Message& reply, Signal sig);
    const LocalSep* FindLocal(uint8_t seid) const;

    IL2capConnector& conn_;
    BdAddr addr_;
    SourceOptions opts_;
    SourceEvents ev_;

    std::unique_ptr<IL2capChannel> sig_;
    std::unique_ptr<IL2capChannel> media_;
    std::thread rx_;
    std::atomic<bool> stop_{false};
    std::thread mediaAcceptThread_;

    mutable std::mutex mu_;  // state, pending, config
    std::condition_variable cv_;
    std::map<uint8_t, Pending> pending_;
    uint8_t nextLabel_ = 0;
    StreamState state_ = StreamState::Idle;
    a2dp::StreamConfig config_;
    uint8_t remoteSeid_ = 0;
    uint8_t localSeid_ = 0;
    bool delayReportingCfg_ = false;
    std::atomic<uint16_t> delay_{0};
    std::mutex sendMu_;
    Reassembler reasm_;
};

}  // namespace bta::avdtp
