// WASAPI loopback capture of a render endpoint as a real-time PCM source.
// This is the user-mode audio path that works without a custom audio driver: whatever plays on the
// chosen endpoint is streamed to the headset. (A dedicated virtual endpoint needs the kernel audio
// driver; see docs/ARCHITECTURE.md.)
#pragma once

#include <atomic>
#include <thread>
#include <vector>

#include "common/json.h"
#include "pipeline/source.h"

namespace bta {

struct AudioEndpointInfo {
    std::string id;
    std::string name;
    bool isDefault = false;
};
Result<std::vector<AudioEndpointInfo>> EnumerateRenderEndpoints();

class WasapiLoopbackSource : public IPcmSource {
public:
    explicit WasapiLoopbackSource(std::string endpointId = {});  // empty = default render endpoint
    ~WasapiLoopbackSource() override;

    std::string Name() const override { return "wasapi-loopback"; }
    Status Start() override;  // initialises the client and starts capture
    void Stop() override;
    PcmFormatDesc Format() const override { return fmt_; }
    bool IsRealtime() const override { return true; }
    size_t Available() const override { return ring_ ? ring_->Available() : 0; }
    size_t Read(float* dst, size_t frames) override { return ring_ ? ring_->Read(dst, frames) : 0; }
    Status Health() const override;
    std::string EndpointName() const { return endpointName_; }
    uint64_t CapturedFrames() const { return captured_; }

private:
    void Run();

    std::string endpointId_, endpointName_;
    PcmFormatDesc fmt_;
    std::unique_ptr<PcmRing> ring_;
    std::thread th_;
    std::atomic<bool> stop_{false};
    std::atomic<uint64_t> captured_{0};
    mutable std::atomic<int> error_{0};  // HRESULT of a fatal capture error
    void* client_ = nullptr;   // IAudioClient*
    void* capture_ = nullptr;  // IAudioCaptureClient*
    int sampleKind_ = 0;       // 0 float32, 1 s16, 2 s24 packed, 3 s32
    int bytesPerFrame_ = 0;
};

}  // namespace bta
