#include "platform/wasapi_source.h"

#include <windows.h>
#include <initguid.h>
#include <audioclient.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <propkeydef.h>
#include <functiondiscoverykeys_devpkey.h>

#include <format>

#include "codec/pcm.h"
#include "common/types.h"
#include "common/log.h"

namespace bta {

namespace {
constexpr const char* kLog = "wasapi";

template <typename T>
struct ComPtr {
    T* p = nullptr;
    ~ComPtr()
    {
        if (p) p->Release();
    }
    T** operator&() { return &p; }
    T* operator->() { return p; }
    explicit operator bool() const { return p != nullptr; }
};

struct CoInit {
    HRESULT hr;
    CoInit() : hr(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
    ~CoInit()
    {
        if (SUCCEEDED(hr)) CoUninitialize();
    }
};

std::string FriendlyName(IMMDevice* dev)
{
    ComPtr<IPropertyStore> ps;
    if (FAILED(dev->OpenPropertyStore(STGM_READ, &ps))) return {};
    PROPVARIANT v;
    PropVariantInit(&v);
    std::string name;
    if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR) name = WideToUtf8(v.pwszVal);
    PropVariantClear(&v);
    return name;
}

std::string HrStr(HRESULT hr) { return std::format("HRESULT 0x{:08X}", static_cast<uint32_t>(hr)); }
}  // namespace

Result<std::vector<AudioEndpointInfo>> EnumerateRenderEndpoints()
{
    CoInit com;
    ComPtr<IMMDeviceEnumerator> en;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en));
    if (FAILED(hr)) return Status{Err::NotAvailable, "MMDeviceEnumerator: " + HrStr(hr)};
    std::string defId;
    {
        ComPtr<IMMDevice> def;
        if (SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &def))) {
            LPWSTR id = nullptr;
            if (SUCCEEDED(def->GetId(&id))) defId = WideToUtf8(id);
            CoTaskMemFree(id);
        }
    }
    ComPtr<IMMDeviceCollection> col;
    hr = en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col);
    if (FAILED(hr)) return Status{Err::IoError, "EnumAudioEndpoints: " + HrStr(hr)};
    UINT n = 0;
    col->GetCount(&n);
    std::vector<AudioEndpointInfo> out;
    for (UINT i = 0; i < n; ++i) {
        ComPtr<IMMDevice> dev;
        if (FAILED(col->Item(i, &dev))) continue;
        LPWSTR id = nullptr;
        dev->GetId(&id);
        AudioEndpointInfo e;
        e.id = id ? WideToUtf8(id) : "";
        CoTaskMemFree(id);
        e.name = FriendlyName(dev.p);
        e.isDefault = e.id == defId;
        out.push_back(e);
    }
    return out;
}

WasapiLoopbackSource::WasapiLoopbackSource(std::string endpointId) : endpointId_(std::move(endpointId)) {}

WasapiLoopbackSource::~WasapiLoopbackSource() { Stop(); }

Status WasapiLoopbackSource::Health() const
{
    int e = error_.load();
    if (e == 0) return Status::Ok();
    return {Err::Disconnected, "audio capture failed: " + HrStr(static_cast<HRESULT>(e)) +
                                   (e == static_cast<int>(AUDCLNT_E_DEVICE_INVALIDATED) ? " (endpoint removed or format changed)" : "")};
}

Status WasapiLoopbackSource::Start()
{
    Stop();
    error_ = 0;
    // Initialise synchronously on a COM MTA thread so format errors surface to the caller.
    std::atomic<int> initDone{0};
    Status initStatus;
    stop_ = false;
    th_ = std::thread([&, this] {
        CoInit com;
        auto fail = [&](Status s) {
            initStatus = std::move(s);
            initDone = 2;
            initDone.notify_all();
        };
        ComPtr<IMMDeviceEnumerator> en;
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en));
        if (FAILED(hr)) return fail({Err::NotAvailable, "MMDeviceEnumerator: " + HrStr(hr)});
        ComPtr<IMMDevice> dev;
        hr = endpointId_.empty() ? en->GetDefaultAudioEndpoint(eRender, eConsole, &dev)
                                 : en->GetDevice(Utf8ToWide(endpointId_).c_str(), &dev);
        if (FAILED(hr)) return fail({Err::NotAvailable, "render endpoint not found: " + HrStr(hr)});
        endpointName_ = FriendlyName(dev.p);
        IAudioClient* client = nullptr;
        hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client));
        if (FAILED(hr)) return fail({Err::IoError, "IAudioClient activate: " + HrStr(hr)});
        client_ = client;
        WAVEFORMATEX* wf = nullptr;
        hr = client->GetMixFormat(&wf);
        if (FAILED(hr)) return fail({Err::IoError, "GetMixFormat: " + HrStr(hr)});
        fmt_.sampleRate = wf->nSamplesPerSec;
        fmt_.channels = wf->nChannels;
        bytesPerFrame_ = wf->nBlockAlign;
        bool isFloat = wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
        if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
            auto* ext = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(wf);
            isFloat = IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) != FALSE;
        }
        if (isFloat && wf->wBitsPerSample == 32) sampleKind_ = 0;
        else if (wf->wBitsPerSample == 16) sampleKind_ = 1;
        else if (wf->wBitsPerSample == 24) sampleKind_ = 2;
        else if (wf->wBitsPerSample == 32) sampleKind_ = 3;
        else {
            CoTaskMemFree(wf);
            return fail({Err::NotSupported, std::format("unsupported mix format ({} bits)", wf->wBitsPerSample)});
        }
        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 2000000 /*200 ms*/, 0, wf, nullptr);
        CoTaskMemFree(wf);
        if (FAILED(hr)) return fail({Err::IoError, "IAudioClient::Initialize(loopback): " + HrStr(hr)});
        IAudioCaptureClient* cap = nullptr;
        hr = client->GetService(IID_PPV_ARGS(&cap));
        if (FAILED(hr)) return fail({Err::IoError, "GetService(IAudioCaptureClient): " + HrStr(hr)});
        capture_ = cap;
        ring_ = std::make_unique<PcmRing>(fmt_.sampleRate, fmt_.channels);  // 1 s
        hr = client->Start();
        if (FAILED(hr)) return fail({Err::IoError, "IAudioClient::Start: " + HrStr(hr)});
        BTA_LOGI(kLog, "loopback capture of '{}' {} Hz {} ch (kind {})", endpointName_, fmt_.sampleRate, fmt_.channels, sampleKind_);
        initDone = 1;
        initDone.notify_all();
        Run();
        client->Stop();
    });
    initDone.wait(0);
    if (initDone == 2) {
        th_.join();
        Stop();
        return initStatus;
    }
    return Status::Ok();
}

void WasapiLoopbackSource::Run()
{
    auto* cap = static_cast<IAudioCaptureClient*>(capture_);
    std::vector<float> conv;
    std::vector<float> silence;
    while (!stop_) {
        Sleep(5);
        UINT32 packet = 0;
        HRESULT hr = cap->GetNextPacketSize(&packet);
        while (SUCCEEDED(hr) && packet > 0) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            hr = cap->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
            if (FAILED(hr)) break;
            size_t samples = static_cast<size_t>(frames) * fmt_.channels;
            conv.resize(samples);
            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                std::fill(conv.begin(), conv.end(), 0.0f);
            } else {
                switch (sampleKind_) {
                case 0: memcpy(conv.data(), data, samples * sizeof(float)); break;
                case 1: S16ToFloat(std::span(reinterpret_cast<const int16_t*>(data), samples), conv.data()); break;
                case 2: S24PackedToFloat(data, samples, conv.data()); break;
                case 3: S32ToFloat(std::span(reinterpret_cast<const int32_t*>(data), samples), conv.data()); break;
                }
            }
            ring_->Write(conv.data(), frames);
            captured_ += frames;
            cap->ReleaseBuffer(frames);
            hr = cap->GetNextPacketSize(&packet);
        }
        if (FAILED(hr)) {
            BTA_LOGE(kLog, "capture error {}", HrStr(hr));
            error_ = static_cast<int>(hr);
            break;
        }
    }
}

void WasapiLoopbackSource::Stop()
{
    stop_ = true;
    if (th_.joinable()) th_.join();
    if (capture_) static_cast<IAudioCaptureClient*>(capture_)->Release();
    if (client_) static_cast<IAudioClient*>(client_)->Release();
    capture_ = nullptr;
    client_ = nullptr;
}

}  // namespace bta
