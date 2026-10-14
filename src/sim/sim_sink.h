// Simulated A2DP sink (AVDTP acceptor + media validator) used by automated tests and by the
// service's --simulate mode. It decodes SBC with libsbc and walks/validates LDAC frame headers.
// It is NOT a replacement for hardware validation.
#pragma once

#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

#include "a2dp/avdtp.h"
#include "transport/l2cap.h"

namespace bta::sim {

struct SimSinkOptions {
    bool ldac = true;
    a2dp::LdacCaps ldacCaps{0x3C /*44.1/48/88.2/96*/, 0x07};
    a2dp::SbcCaps sbcCaps{0xF0, 0x0F, 0xF0, 0x0C, 0x03, 2, 53};
    bool supportGetAllCapabilities = true;
    bool rejectLdacConfiguration = false;
    bool delayReporting = true;
    uint16_t delayReportTenthMs = 1500;  // 150 ms
    bool decodeSbc = true;
    size_t maxDecodedSamples = 48000 * 10;
    uint32_t ignoreCommandsAfter = 0;  // >0: stop answering after N commands (timeout injection)
};

struct SimSinkStats {
    std::string state = "idle";
    CodecId configuredCodec = CodecId::None;
    uint32_t sampleRate = 0;
    int channels = 0;
    uint64_t mediaPackets = 0, mediaBytes = 0, codecFrames = 0;
    uint64_t sbcFrames = 0, ldacFrames = 0;
    uint64_t sequenceErrors = 0, malformedPackets = 0, frameCountMismatches = 0;
    uint64_t lastTimestamp = 0, timestampErrors = 0;
    std::vector<int16_t> decodedLeft;  // SBC decode output (left/mono)
    std::vector<std::string> signals;  // received command names in order
    std::vector<uint8_t> ldacSampleRateIds;
};

class SimulatedSink {
public:
    explicit SimulatedSink(SimSinkOptions opts);
    ~SimulatedSink();

    // Hook for LoopbackConnector: first channel = signalling, later = media.
    void OnChannel(uint16_t psm, std::unique_ptr<IL2capChannel> ch);

    SimSinkStats Stats() const;
    // Fault injection: sink-initiated commands.
    void SendCommand(avdtp::Signal sig, std::vector<uint8_t> payload = {});
    void DropLink();
    uint8_t ConfiguredSourceSeid() const { return srcSeid_; }

private:
    void SignalLoop(IL2capChannel* ch);
    void MediaLoop(IL2capChannel* ch);
    void Handle(const avdtp::Message& m, IL2capChannel* ch);
    void Reply(IL2capChannel* ch, const avdtp::Message& cmd, avdtp::MsgType t, std::vector<uint8_t> payload = {});
    void ValidateMedia(const std::vector<uint8_t>& pkt);

    SimSinkOptions opts_;
    std::unique_ptr<IL2capChannel> sig_, media_;
    std::thread sigThread_, mediaThread_;
    // Channels/threads of earlier links stay alive until destruction (no use-after-free on relink).
    std::vector<std::unique_ptr<IL2capChannel>> retiredChannels_;
    std::vector<std::thread> retiredThreads_;
    std::atomic<bool> stop_{false};
    mutable std::mutex mu_;
    std::mutex sendMu_;
    SimSinkStats st_;
    uint8_t srcSeid_ = 0, cfgSeid_ = 0;
    uint8_t label_ = 8;
    bool delayPending_ = false;
    uint16_t expectSeq_ = 0;
    bool haveSeq_ = false;
    uint32_t commandCount_ = 0;
    void* sbcDecoder_ = nullptr;  // sbc_t
};

}  // namespace bta::sim
