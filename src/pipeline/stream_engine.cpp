#include "pipeline/stream_engine.h"

#include <windows.h>
#include <avrt.h>

#include <algorithm>
#include <chrono>
#include <deque>

#include "common/log.h"

namespace bta {

namespace {
constexpr const char* kLog = "engine";

class Clock {
public:
    Clock()
    {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        freq_ = static_cast<double>(f.QuadPart);
    }
    double NowSec() const
    {
        LARGE_INTEGER c;
        QueryPerformanceCounter(&c);
        return c.QuadPart / freq_;
    }

private:
    double freq_;
};
}  // namespace

Status StreamEngine::Start(std::unique_ptr<IEncoder> encoder, IPcmSource* source, IL2capChannel* media,
                           const a2dp::StreamConfig& cfg, FatalHandler onFatal)
{
    if (running_) return {Err::StateError, "engine already running"};
    if (!encoder || !source || !media) return {Err::InvalidArgument, "missing encoder/source/media"};
    if (th_.joinable()) th_.join();
    enc_ = std::move(encoder);
    src_ = source;
    media_ = media;
    cfg_ = cfg;
    onFatal_ = std::move(onFatal);
    {
        std::lock_guard lk(statsMu_);
        stats_ = {};
        stats_.codec = enc_->Codec();
        stats_.running = true;
        stats_.nominalBitrate = enc_->NominalBitrate();
        stats_.quality = enc_->QualityLabel();
    }
    stop_ = false;
    running_ = true;
    th_ = std::thread([this] { Run(); });
    return Status::Ok();
}

void StreamEngine::Stop()
{
    stop_ = true;
    if (th_.joinable() && th_.get_id() != std::this_thread::get_id()) th_.join();
    running_ = false;
    std::lock_guard lk(statsMu_);
    stats_.running = false;
}

std::unique_ptr<IEncoder> StreamEngine::TakeEncoder()
{
    Stop();
    return std::move(enc_);
}

EngineStats StreamEngine::Stats() const
{
    std::lock_guard lk(statsMu_);
    return stats_;
}

Status StreamEngine::SetQuality(int level)
{
    std::lock_guard lk(encMu_);
    if (!enc_) return {Err::StateError, "no encoder"};
    Status st = enc_->SetQuality(level);
    if (st) {
        std::lock_guard sl(statsMu_);
        stats_.quality = enc_->QualityLabel();
        stats_.nominalBitrate = enc_->NominalBitrate();
    }
    return st;
}

void StreamEngine::Fail(const Status& st)
{
    BTA_LOGE(kLog, "stream stopped: {}", st.ToString());
    running_ = false;
    {
        std::lock_guard lk(statsMu_);
        stats_.running = false;
    }
    if (onFatal_) onFatal_(st);
}

void StreamEngine::Run()
{
    HANDLE mmcss = nullptr;
    HANDLE timer = nullptr;
    if (opts_.realtimeThread) {
        DWORD idx = 0;
        mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &idx);
        if (!mmcss) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
        timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    }

    const PcmFormatDesc sf = src_->Format();
    const uint32_t rate = cfg_.sampleRate;
    const int outCh = enc_->Channels();
    const uint32_t perCall = enc_->InputFramesPerCall();
    const SampleFormat fmt = enc_->InputFormat();
    Resampler rs(sf.sampleRate, rate, outCh);
    a2dp::RtpPacketizer rtp;

    std::vector<float> readBuf, remapped, staged;  // staged: interleaved at stream rate, outCh
    size_t stagedPos = 0;                        // consumed frames in staged
    std::vector<float> frameF(perCall * outCh);
    std::vector<uint8_t> frameI(perCall * outCh * BytesPerSample(fmt));
    std::vector<EncodedPayload> payloads;

    const double sendAhead = opts_.sendAheadMs / 1000.0 * rate;
    const double lowWater = opts_.lowWaterMs / 1000.0 * rate;
    const size_t maxBacklog = static_cast<size_t>(opts_.maxBacklogMs / 1000.0 * rate);

    Clock clock;
    double start = clock.NowSec();
    double primeStart = start;
    bool primed = !src_->IsRealtime();
    uint64_t sentFrames = 0;
    double encodeTotalUs = 0;
    uint64_t encodeCount = 0;
    double lastFeedback = start;
    std::deque<std::pair<double, size_t>> rateWindow;  // (time, bytes)

    BTA_LOGI(kLog, "started: {} source={} {} Hz/{}ch -> {} Hz/{}ch, {} frames/call, send-ahead {} ms", cfg_.Describe(),
             src_->Name(), sf.sampleRate, sf.channels, rate, outCh, perCall, opts_.sendAheadMs);

    auto stagedFrames = [&] { return staged.size() / outCh - stagedPos; };
    auto pullSource = [&](size_t maxFrames) {
        size_t avail = std::min(src_->Available(), maxFrames);
        while (avail > 0) {
            size_t n = std::min<size_t>(avail, 2048);
            readBuf.resize(n * sf.channels);
            n = src_->Read(readBuf.data(), n);
            if (n == 0) break;
            readBuf.resize(n * sf.channels);
            RemapChannels(readBuf, sf.channels, remapped, outCh);
            rs.Process(remapped, staged);
            avail -= n;
        }
    };

    // Sends encoded payloads; returns non-Ok only for fatal transport errors.
    auto sendPayloads = [&](double now) -> Status {
        for (const auto& p : payloads) {
            auto pkt = rtp.Build(p.bytes, p.frames, p.pcmFrames);
            Status ss = media_->Send(pkt);
            std::lock_guard lk(statsMu_);
            if (!ss) {
                stats_.sendErrors++;
                if (ss.code() == Err::Disconnected) return ss;
                continue;
            }
            stats_.packets++;
            stats_.bytes += pkt.size();
            stats_.codecFrames += p.frames;
            rateWindow.emplace_back(now, pkt.size());
        }
        return Status::Ok();
    };

    bool fatal = false;
    while (!stop_ && !fatal) {
        if (timer) {
            LARGE_INTEGER due;
            due.QuadPart = -static_cast<LONGLONG>(opts_.tickMs) * 10000;
            SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
            WaitForSingleObject(timer, 100);
        } else {
            Sleep(opts_.tickMs);
        }
        if (Status h = src_->Health(); !h) {
            Fail(h);
            fatal = true;
            break;
        }

        const double now = clock.NowSec();
        // Compact the staging buffer.
        if (stagedPos > 0 && stagedPos * 2 > staged.size() / outCh) {
            staged.erase(staged.begin(), staged.begin() + static_cast<ptrdiff_t>(stagedPos * outCh));
            stagedPos = 0;
        }

        if (src_->IsRealtime()) {
            pullSource(SIZE_MAX);
            if (stagedFrames() > maxBacklog + static_cast<size_t>(sendAhead)) {
                size_t drop = stagedFrames() - static_cast<size_t>(sendAhead);
                stagedPos += drop;
                std::lock_guard lk(statsMu_);
                stats_.droppedSourceFrames += drop;
            }
            if (!primed) {
                if (stagedFrames() >= static_cast<size_t>(sendAhead) || (now - primeStart) * 1000 > opts_.primeTimeoutMs) {
                    primed = true;
                    start = now;
                    sentFrames = 0;
                    BTA_LOGD(kLog, "primed with {} frames", stagedFrames());
                } else {
                    continue;
                }
            }
        }

        double elapsed = (now - start) * rate;
        double ahead = static_cast<double>(sentFrames) - elapsed;
        if (ahead < -sendAhead * 4) {  // stalled (debugger, suspend): resynchronise
            BTA_LOGW(kLog, "pipeline stalled {:.0f} ms; resynchronising", -ahead * 1000 / rate);
            start = now;
            sentFrames = 0;
            ahead = 0;
        }

        while (!stop_ && ahead < sendAhead) {
            if (!src_->IsRealtime() && stagedFrames() < perCall) pullSource(perCall * 4);
            size_t have = stagedFrames();
            if (have >= perCall) {
                std::copy_n(staged.begin() + static_cast<ptrdiff_t>(stagedPos * outCh), perCall * outCh, frameF.begin());
                stagedPos += perCall;
            } else if (ahead < lowWater) {
                // Underrun: use what we have and pad with silence to keep the sink clock fed.
                std::fill(frameF.begin(), frameF.end(), 0.0f);
                std::copy_n(staged.begin() + static_cast<ptrdiff_t>(stagedPos * outCh), have * outCh, frameF.begin());
                stagedPos += have;
                std::lock_guard lk(statsMu_);
                stats_.silenceFramesInserted += perCall - have;
                if (have == 0) stats_.underrunEvents++;
            } else {
                break;  // wait for more source data
            }

            if (fmt == SampleFormat::S16)
                FloatToS16(frameF, reinterpret_cast<int16_t*>(frameI.data()));
            else
                FloatToS32(frameF, reinterpret_cast<int32_t*>(frameI.data()));

            payloads.clear();
            double t0 = clock.NowSec();
            Status st;
            {
                std::lock_guard lk(encMu_);
                st = enc_->Encode(frameI.data(), payloads);
            }
            double us = (clock.NowSec() - t0) * 1e6;
            encodeTotalUs += us;
            ++encodeCount;
            if (!st) {
                Fail(st);
                fatal = true;
                break;
            }
            if (Status ss = sendPayloads(now); !ss) {
                Fail(ss);
                fatal = true;
                break;
            }
            sentFrames += perCall;
            ahead += perCall;
            {
                std::lock_guard lk(statsMu_);
                stats_.pcmFramesSent = sentFrames;
                stats_.encodeMaxUs = std::max(stats_.encodeMaxUs, us);
            }
        }

        if ((now - lastFeedback) * 1000 >= opts_.feedbackIntervalMs) {
            uint32_t depth = media_->PendingSends();
            {
                std::lock_guard lk(encMu_);
                enc_->OnTransmitFeedback(depth);
            }
            lastFeedback = now;
            while (!rateWindow.empty() && now - rateWindow.front().first > 1.0) rateWindow.pop_front();
            size_t bytes = 0;
            for (auto& [t, b] : rateWindow) bytes += b;
            std::lock_guard lk(statsMu_);
            stats_.queueDepth = depth;
            stats_.measuredKbps = bytes * 8 / 1000.0 / std::min(1.0, std::max(0.05, now - start));
            stats_.encodeAvgUs = encodeCount ? encodeTotalUs / encodeCount : 0;
            stats_.sinkBufferMs = std::max(0.0, ahead) * 1000.0 / rate;
            stats_.quality = enc_->QualityLabel();
            stats_.nominalBitrate = enc_->NominalBitrate();
            stats_.runtimeMs = static_cast<uint64_t>((now - start) * 1000);
        }
    }
    if (!fatal && media_->IsOpen()) {
        // Drain a partially filled payload so no encoded audio is lost on a clean stop.
        payloads.clear();
        {
            std::lock_guard lk(encMu_);
            enc_->Flush(payloads);
        }
        sendPayloads(clock.NowSec());
    }
    if (timer) CloseHandle(timer);
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    running_ = false;
    std::lock_guard lk(statsMu_);
    stats_.running = false;
    BTA_LOGI(kLog, "stopped: {} packets, {} bytes, {} silence frames", stats_.packets, stats_.bytes, stats_.silenceFramesInserted);
}

}  // namespace bta
