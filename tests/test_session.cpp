// End-to-end (offline) integration: SessionController + AvdtpSource + StreamEngine + real encoders
// over loopback L2CAP channels into the SimulatedSink, which decodes/validates the media.
#include <chrono>
#include <cmath>
#include <numbers>
#include <thread>

#include "a2dp/avdtp_source.h"
#include "core/session_controller.h"
#include "sim/sim_sink.h"
#include "transport/loopback.h"
#include "testing.h"

using namespace bta;
using namespace std::chrono_literals;

namespace {

const BdAddr kAddr{0x010203044187ULL};

struct Rig {
    sim::SimulatedSink sink;
    LoopbackConnector conn;
    std::unique_ptr<SessionController> ctl;

    explicit Rig(sim::SimSinkOptions so, Settings s = {}, EncoderFactory ef = CreateEncoder)
        : sink(so), conn([this](uint16_t psm, std::unique_ptr<IL2capChannel> ch) { sink.OnChannel(psm, std::move(ch)); }, 1005)
    {
        ControllerDeps d;
        d.connector = &conn;
        d.encoderFactory = ef;
        d.signalTimeoutMs = 800;
        d.engine.sendAheadMs = 40;
        d.sourceFactory = [](const Settings&) -> Result<std::unique_ptr<IPcmSource>> {
            return std::unique_ptr<IPcmSource>(std::make_unique<ToneSource>(48000, 2, 1000.0, 0.3));
        };
        ctl = std::make_unique<SessionController>(d, s);
    }

    bool WaitStreaming(uint64_t minPackets, uint32_t ms = 5000)
    {
        return ctl->WaitFor([&](const SessionStatus& st) { return st.codecConfirmed && st.engine.packets >= minPackets; }, ms);
    }
};

// Encoder wrapper that fails after N encode calls (fault injection).
class FailingEncoder : public IEncoder {
public:
    FailingEncoder(std::unique_ptr<IEncoder> inner, int failOpen, int failAfter)
        : inner_(std::move(inner)), failOpen_(failOpen), failAfter_(failAfter) {}
    CodecId Codec() const override { return inner_->Codec(); }
    Status Open(const a2dp::StreamConfig& c, uint16_t mtu) override
    {
        if (failOpen_) return {Err::EncoderError, "injected open failure"};
        return inner_->Open(c, mtu);
    }
    void Close() override { inner_->Close(); }
    SampleFormat InputFormat() const override { return inner_->InputFormat(); }
    int Channels() const override { return inner_->Channels(); }
    uint32_t InputFramesPerCall() const override { return inner_->InputFramesPerCall(); }
    Status Encode(const void* pcm, std::vector<EncodedPayload>& out) override
    {
        if (failAfter_ >= 0 && calls_++ >= failAfter_) return {Err::EncoderError, "injected encode failure"};
        return inner_->Encode(pcm, out);
    }
    uint32_t NominalBitrate() const override { return inner_->NominalBitrate(); }
    std::string QualityLabel() const override { return inner_->QualityLabel(); }
    Status SetQuality(int l) override { return inner_->SetQuality(l); }

private:
    std::unique_ptr<IEncoder> inner_;
    int failOpen_, failAfter_;
    int calls_ = 0;
};

}  // namespace

TEST(Session, LdacNegotiatedAndStreamed)
{
    Settings s;
    s.ldacQuality = LdacQuality::High;
    Rig rig({}, s);
    rig.ctl->Connect(kAddr);
    REQUIRE(rig.WaitStreaming(40));
    std::this_thread::sleep_for(1000ms);
    auto st = rig.ctl->GetStatus();
    auto ss = rig.sink.Stats();
    CHECK(st.activeCodec == CodecId::Ldac);
    CHECK(st.fallbackReason.empty());
    CHECK(ss.configuredCodec == CodecId::Ldac);
    CHECK_EQ(ss.sampleRate, 48000u);
    CHECK(ss.ldacFrames > 300);
    CHECK_EQ(ss.malformedPackets, uint64_t(0));
    CHECK_EQ(ss.sequenceErrors, uint64_t(0));
    CHECK_EQ(ss.frameCountMismatches, uint64_t(0));
    CHECK_NEAR(st.sinkDelayMs, 150.0, 0.01);  // DELAYREPORT received
    CHECK(st.sinkDelayReporting);
    // Real-time pacing: ~1.0x real time (frames are 128 samples @ 48 kHz).
    // Compare values from the same engine snapshot (status is refreshed periodically).
    double audioSec = st.engine.pcmFramesSent / 48000.0;
    double runtime = st.engine.runtimeMs / 1000.0;
    CHECK(ss.ldacFrames * 128 >= st.engine.pcmFramesSent - 2 * 128);  // everything sent reached the sink
    printf("      LDAC session: %.2f s audio in %.2f s, %.0f kbps measured, quality %s, encode avg %.0f us\n", audioSec,
           runtime, st.engine.measuredKbps, st.engine.quality.c_str(), st.engine.encodeAvgUs);
    CHECK(audioSec >= runtime && audioSec < runtime + 0.1);  // runtime + send-ahead (40 ms) + feedback interval + tick
    CHECK_EQ(st.engine.underrunEvents, 0u);
    // GET_ALL_CAPABILITIES used, then SET_CONFIGURATION/OPEN/START
    bool sawGetAll = false, sawStart = false;
    for (auto& n : ss.signals) {
        sawGetAll |= n == "GET_ALL_CAPABILITIES";
        sawStart |= n == "START";
    }
    CHECK(sawGetAll && sawStart);

    rig.ctl->Disconnect();
    CHECK(rig.ctl->WaitFor([](const SessionStatus& x) { return x.state == SessionState::Idle; }, 3000));
    CHECK(!rig.ctl->GetStatus().codecConfirmed);
}

TEST(Session, SbcWhenSinkHasNoLdac)
{
    sim::SimSinkOptions so;
    so.ldac = false;
    Rig rig(so);
    rig.ctl->Connect(kAddr);
    REQUIRE(rig.WaitStreaming(30));
    std::this_thread::sleep_for(600ms);
    auto st = rig.ctl->GetStatus();
    auto ss = rig.sink.Stats();
    CHECK(st.activeCodec == CodecId::Sbc);
    CHECK(st.fallbackReason.find("does not advertise LDAC") != std::string::npos);
    CHECK(ss.sbcFrames > 100);
    CHECK_EQ(ss.malformedPackets, uint64_t(0));
    // Decoded audio must be the 1 kHz tone (no buffer corruption across the pipeline).
    REQUIRE(ss.decodedLeft.size() > 8192);
    double w = 2 * std::numbers::pi * 1000 / 48000.0, c = 2 * std::cos(w), s1 = 0, s2 = 0, tot = 0;
    size_t n = 0;
    for (size_t i = 4096; i < ss.decodedLeft.size(); ++i, ++n) {
        double v = ss.decodedLeft[i] / 32768.0;
        double s0 = v + c * s1 - s2;
        s2 = s1;
        s1 = s0;
        tot += v * v;
    }
    double ratio = (2 * (s1 * s1 + s2 * s2 - c * s1 * s2) / n) / tot;
    printf("      SBC session tone purity: %.4f\n", ratio);
    CHECK(ratio > 0.98);
}

TEST(Session, SbcByUserPreference)
{
    Settings s;
    s.codec = CodecPreference::Sbc;
    Rig rig({}, s);
    rig.ctl->Connect(kAddr);
    REQUIRE(rig.WaitStreaming(10));
    CHECK(rig.ctl->GetStatus().activeCodec == CodecId::Sbc);
    CHECK(rig.sink.Stats().configuredCodec == CodecId::Sbc);
}

TEST(Session, FallbackWhenSinkRejectsLdacConfig)
{
    sim::SimSinkOptions so;
    so.rejectLdacConfiguration = true;
    Rig rig(so);
    rig.ctl->Connect(kAddr);
    REQUIRE(rig.WaitStreaming(10));
    auto st = rig.ctl->GetStatus();
    CHECK(st.activeCodec == CodecId::Sbc);
    CHECK(st.fallbackReason.find("rejected LDAC") != std::string::npos);
}

TEST(Session, FallbackWhenLdacEncoderInitFails)
{
    EncoderFactory ef = [](CodecId c) -> std::unique_ptr<IEncoder> {
        auto e = CreateEncoder(c);
        if (c == CodecId::Ldac) return std::make_unique<FailingEncoder>(std::move(e), 1, -1);
        return e;
    };
    Rig rig({}, {}, ef);
    rig.ctl->Connect(kAddr);
    REQUIRE(rig.WaitStreaming(10));
    auto st = rig.ctl->GetStatus();
    CHECK(st.activeCodec == CodecId::Sbc);
    CHECK(st.fallbackReason.find("init failed") != std::string::npos);
    CHECK(rig.sink.Stats().configuredCodec == CodecId::Sbc);
}

TEST(Session, FallbackWhenLdacEncoderFailsMidStream)
{
    EncoderFactory ef = [](CodecId c) -> std::unique_ptr<IEncoder> {
        auto e = CreateEncoder(c);
        if (c == CodecId::Ldac) return std::make_unique<FailingEncoder>(std::move(e), 0, 200);
        return e;
    };
    Rig rig({}, {}, ef);
    rig.ctl->Connect(kAddr);
    REQUIRE(rig.ctl->WaitFor([](const SessionStatus& s) { return s.codecConfirmed && s.activeCodec == CodecId::Sbc && s.engine.packets > 20; }, 8000));
    auto st = rig.ctl->GetStatus();
    CHECK(st.fallbackReason.find("encoder error") != std::string::npos);
    auto ss = rig.sink.Stats();
    CHECK(ss.ldacFrames > 0);  // LDAC really streamed before the failure
    CHECK(ss.sbcFrames > 0);
    CHECK(ss.configuredCodec == CodecId::Sbc);
}

TEST(Session, GetCapabilitiesFallbackForAvdtp12Sink)
{
    sim::SimSinkOptions so;
    so.supportGetAllCapabilities = false;
    Rig rig(so);
    rig.ctl->Connect(kAddr);
    REQUIRE(rig.WaitStreaming(5));
    auto ss = rig.sink.Stats();
    bool sawGet = false;
    for (auto& n : ss.signals) sawGet |= n == "GET_CAPABILITIES";
    CHECK(sawGet);
    CHECK(rig.ctl->GetStatus().activeCodec == CodecId::Ldac);
    CHECK(!rig.ctl->GetStatus().sinkDelayReporting);  // only advertised via GET_ALL
}

TEST(Session, SignalTimeoutReportsError)
{
    sim::SimSinkOptions so;
    so.ignoreCommandsAfter = 1;  // answers DISCOVER only
    Rig rig(so);
    rig.ctl->Connect(kAddr);
    REQUIRE(rig.ctl->WaitFor([](const SessionStatus& s) { return s.state == SessionState::Error; }, 6000));
    auto st = rig.ctl->GetStatus();
    CHECK(!st.codecConfirmed);
    CHECK(st.lastError.find("Timeout") != std::string::npos || st.lastError.find("timed out") != std::string::npos);
}

TEST(Session, RemoteSuspendAndResume)
{
    Rig rig({});
    rig.ctl->Connect(kAddr);
    REQUIRE(rig.WaitStreaming(10));
    uint8_t seid = rig.sink.ConfiguredSourceSeid();
    rig.sink.SendCommand(avdtp::Signal::Suspend, {avdtp::SeidByte(seid)});
    REQUIRE(rig.ctl->WaitFor([](const SessionStatus& s) { return s.state == SessionState::Suspended; }, 3000));
    CHECK(!rig.ctl->GetStatus().codecConfirmed);
    rig.sink.SendCommand(avdtp::Signal::Start, {avdtp::SeidByte(seid)});
    uint64_t before = rig.ctl->GetStatus().engine.packets;
    REQUIRE(rig.ctl->WaitFor([&](const SessionStatus& s) { return s.codecConfirmed && s.engine.packets > 10; }, 3000));
    (void)before;
}

TEST(Session, LinkLossAndReconnect)
{
    sim::SimSinkOptions so;
    Settings s;
    Rig rig(so, s);
    // rebuild controller with reconnect enabled
    ControllerDeps d;
    d.connector = &rig.conn;
    d.signalTimeoutMs = 800;
    d.maxReconnectAttempts = 2;
    d.reconnectDelayMs = 200;
    d.sourceFactory = [](const Settings&) -> Result<std::unique_ptr<IPcmSource>> {
        return std::unique_ptr<IPcmSource>(std::make_unique<ToneSource>(48000, 2));
    };
    rig.ctl = std::make_unique<SessionController>(d, s);
    rig.ctl->Connect(kAddr);
    REQUIRE(rig.WaitStreaming(10));
    rig.sink.DropLink();
    REQUIRE(rig.ctl->WaitFor([](const SessionStatus& x) { return !x.codecConfirmed; }, 3000));
    REQUIRE(rig.ctl->WaitFor([](const SessionStatus& x) { return x.reconnects >= 1 && x.codecConfirmed; }, 8000));
    CHECK(rig.ctl->GetStatus().activeCodec == CodecId::Ldac);
}

TEST(Session, ConnectFailureIsReported)
{
    Rig rig({});
    rig.conn.FailNextConnects(1);
    rig.ctl->Connect(kAddr);
    REQUIRE(rig.ctl->WaitFor([](const SessionStatus& s) { return s.state == SessionState::Error; }, 3000));
    CHECK(rig.ctl->GetStatus().lastError.find("injected") != std::string::npos);
}

TEST(Session, LiveLdacQualityChange)
{
    Settings s;
    s.ldacQuality = LdacQuality::High;
    Rig rig({}, s);
    rig.ctl->Connect(kAddr);
    REQUIRE(rig.WaitStreaming(10));
    s.ldacQuality = LdacQuality::Mobile;
    rig.ctl->UpdateSettings(s);
    REQUIRE(rig.ctl->WaitFor([](const SessionStatus& x) { return x.engine.quality == "MQ"; }, 3000));
    CHECK(!rig.ctl->GetStatus().restartRequired);
    s.sampleRate = 96000;
    rig.ctl->UpdateSettings(s);
    CHECK(rig.ctl->WaitFor([](const SessionStatus& x) { return x.restartRequired; }, 3000));
}
