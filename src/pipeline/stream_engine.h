// Real-time media pump: source PCM -> remap/resample -> encoder -> RTP -> L2CAP media channel.
// Paces transmission so the sink holds ~sendAheadMs of audio; inserts silence on source underrun
// (after priming) and drops backlog on source overrun (clock drift).
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "a2dp/codec_caps.h"
#include "a2dp/media.h"
#include "codec/encoder.h"
#include "pipeline/source.h"
#include "transport/l2cap.h"

namespace bta {

struct EngineOptions {
    uint32_t sendAheadMs = 40;
    uint32_t lowWaterMs = 10;       // pad silence below this (after priming)
    uint32_t primeTimeoutMs = 500;  // start anyway (with silence) if the source stays silent
    uint32_t tickMs = 4;
    uint32_t maxBacklogMs = 250;    // drop staged source audio beyond this
    uint32_t feedbackIntervalMs = 20;
    bool realtimeThread = true;     // MMCSS "Pro Audio" + high-res timer
};

struct EngineStats {
    bool running = false;
    CodecId codec = CodecId::None;
    uint64_t packets = 0;
    uint64_t bytes = 0;
    uint64_t codecFrames = 0;
    uint64_t pcmFramesSent = 0;
    uint64_t silenceFramesInserted = 0;
    uint64_t droppedSourceFrames = 0;
    uint64_t sendErrors = 0;
    uint32_t underrunEvents = 0;
    double measuredKbps = 0;      // over the last ~1 s
    double encodeAvgUs = 0, encodeMaxUs = 0;
    double sinkBufferMs = 0;      // estimated audio buffered at the sink
    uint32_t queueDepth = 0;
    uint32_t nominalBitrate = 0;
    std::string quality;
    uint64_t runtimeMs = 0;
};

class StreamEngine {
public:
    using FatalHandler = std::function<void(const Status&)>;

    explicit StreamEngine(EngineOptions opts = {}) : opts_(opts) {}
    ~StreamEngine() { Stop(); }

    // encoder must already be Open() for cfg and the media channel's MTU.
    Status Start(std::unique_ptr<IEncoder> encoder, IPcmSource* source, IL2capChannel* media,
                 const a2dp::StreamConfig& cfg, FatalHandler onFatal);
    void Stop();
    bool Running() const { return running_; }
    EngineStats Stats() const;
    Status SetQuality(int level);
    std::unique_ptr<IEncoder> TakeEncoder();  // after Stop()

private:
    void Run();
    void Fail(const Status& st);

    EngineOptions opts_;
    std::unique_ptr<IEncoder> enc_;
    IPcmSource* src_ = nullptr;
    IL2capChannel* media_ = nullptr;
    a2dp::StreamConfig cfg_;
    FatalHandler onFatal_;
    std::thread th_;
    std::atomic<bool> stop_{false}, running_{false};
    mutable std::mutex statsMu_;
    std::mutex encMu_;
    EngineStats stats_;
};

}  // namespace bta
