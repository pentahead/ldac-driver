#include "platform/service_core.h"

#include <chrono>
#include <format>

#include "bta2dp_ioctl.h"
#include "common/log.h"
#include "platform/bt_discovery.h"
#include "platform/kernel_l2cap.h"
#include "platform/wasapi_source.h"

namespace bta {

static constexpr const char* kLog = "service";

static Json Ok()
{
    Json j = Json::MakeObject();
    j["ok"] = true;
    return j;
}

static Json Fail(const std::string& msg)
{
    Json j = Json::MakeObject();
    j["ok"] = false;
    j["error"] = msg;
    return j;
}

static LogLevel ParseLevel(const std::string& l)
{
    if (l == "trace") return LogLevel::Trace;
    if (l == "debug") return LogLevel::Debug;
    if (l == "warn") return LogLevel::Warn;
    if (l == "error") return LogLevel::Error;
    return LogLevel::Info;
}

Result<std::unique_ptr<IPcmSource>> CreateSourceFromSettings(const Settings& s)
{
    if (s.source == "tone") return std::unique_ptr<IPcmSource>(std::make_unique<ToneSource>(48000, 2, 1000.0, 0.2));
    if (s.source == "loopback") return std::unique_ptr<IPcmSource>(std::make_unique<WasapiLoopbackSource>(s.sourceEndpointId));
    return Status{Err::NotAvailable, "source 'endpoint' requires the BtA2dpAudio virtual endpoint driver (not available in this build)"};
}

ServiceCore::ServiceCore(ServiceOptions opts) : opts_(std::move(opts))
{
    settingsPath_ = opts_.settingsPath.empty() ? DefaultSettingsPath() : opts_.settingsPath;
}

ServiceCore::~ServiceCore() { Stop(); }

Status ServiceCore::Start()
{
    Status ls = LoadSettings(settingsPath_, settings_);
    if (!ls && ls.code() != Err::NotAvailable) BTA_LOGW(kLog, "settings ignored ({}); using defaults", ls.ToString());
    Logger::Instance().SetMinLevel(ParseLevel(settings_.logLevel));

    if (opts_.simulate) {
        simSink_ = std::make_unique<sim::SimulatedSink>(opts_.simSink);
        connector_ = std::make_unique<LoopbackConnector>(
            [this](uint16_t psm, std::unique_ptr<IL2capChannel> ch) { simSink_->OnChannel(psm, std::move(ch)); }, static_cast<uint16_t>(1005));
        BTA_LOGW(kLog, "SIMULATION MODE: audio goes to an in-process simulated sink, not to Bluetooth hardware");
    } else {
        connector_ = std::make_unique<KernelL2capConnector>();
    }
    ControllerDeps d;
    d.connector = connector_.get();
    d.sourceFactory = CreateSourceFromSettings;
    d.maxReconnectAttempts = opts_.maxReconnectAttempts;
    d.engine.sendAheadMs = settings_.sendAheadMs;
    controller_ = std::make_unique<SessionController>(d, settings_);
    if (opts_.startPipe) {
        pipe_ = std::make_unique<PipeServer>([this](const Json& r) { return Handle(r); }, opts_.pipeName);
        if (Status st = pipe_->Start(); !st) return st;
    }
    BTA_LOGI(kLog, "service core started (settings {})", WideToUtf8(settingsPath_));
    return Status::Ok();
}

void ServiceCore::Stop()
{
    if (pipe_) pipe_->Stop();
    pipe_.reset();
    controller_.reset();
    connector_.reset();
    simSink_.reset();
}

Json ServiceCore::Handle(const Json& req)
{
    std::string cmd = req.Get("cmd").AsString();
    BTA_LOGD(kLog, "command '{}'", cmd);
    if (cmd == "status") {
        Json j = Ok();
        j["status"] = controller_->GetStatus().ToJson();
        j["simulate"] = opts_.simulate;
        return j;
    }
    if (cmd == "info") return Info();
    if (cmd == "devices") return Devices();
    if (cmd == "probe") return Probe(req);
    if (cmd == "get_settings") {
        std::lock_guard lk(settingsMu_);
        Json j = Ok();
        j["settings"] = settings_.ToJson();
        return j;
    }
    if (cmd == "set_settings") return SetSettings(req);
    if (cmd == "connect") return Connect(req);
    if (cmd == "disconnect") {
        controller_->Disconnect();
        return Ok();
    }
    if (cmd == "logs") return Logs(req);
    if (cmd == "endpoints") return Endpoints();
    if (cmd == "selftest") return SelfTest();
    return Fail("unknown command '" + cmd + "'");
}

Json ServiceCore::Info()
{
    Json j = Ok();
    j["version"] = "0.1.0";
    j["simulate"] = opts_.simulate;
    j["transport"] = connector_->Name();
    j["ioctlVersion"] = BTA2DP_IOCTL_VERSION;
    std::string why;
    Json enc = Json::MakeObject();
    enc["SBC"] = IsEncoderAvailable(CodecId::Sbc);
    enc["LDAC"] = IsEncoderAvailable(CodecId::Ldac, &why);
    j["encoders"] = enc;
    std::string radio;
    j["radioPresent"] = BluetoothRadioPresent(&radio);
    j["radioName"] = radio;
    std::string derr;
    auto inst = EnumerateL2capDriverInstances(&derr);
    Json di = Json::MakeArray();
    for (auto& i : inst) {
        Json e = Json::MakeObject();
        e["address"] = i.remote.ToString();
        e["version"] = i.version;
        e["connected"] = i.remoteConnected;
        di.Push(e);
    }
    j["driverInstances"] = di;
    if (inst.empty()) j["driverStatus"] = derr;
    return j;
}

Json ServiceCore::Devices()
{
    Json list = Json::MakeArray();
    if (opts_.simulate) {
        Json d = Json::MakeObject();
        d["address"] = kSimAddress;
        d["name"] = "Simulated LDAC Sink";
        d["connected"] = true;
        d["a2dpSink"] = true;
        d["audioClass"] = true;
        Json drv = Json::MakeObject();
        drv["present"] = true;
        drv["service"] = "simulated";
        d["driver"] = drv;
        d["transportReady"] = true;
        list.Push(d);
    }
    auto devs = EnumeratePairedDevices();
    if (devs) {
        for (auto& d : *devs) {
            Json j = d.ToJson();
            j["transportReady"] = !opts_.simulate && connector_->CheckAvailable(d.address).ok();
            list.Push(j);
        }
    }
    Json r = Ok();
    r["devices"] = list;
    if (!devs) r["warning"] = devs.status().ToString();
    return r;
}

Json ServiceCore::Probe(const Json& req)
{
    auto a = BdAddr::Parse(req.Get("address").AsString());
    if (!a) return Fail("address required");
    auto rec = QueryA2dpSinkSdp(*a);
    if (!rec) return Fail(rec.status().ToString());
    Json j = Ok();
    j["sdp"] = rec->ToJson();
    j["note"] = "Codec capabilities are reported after an AVDTP connection (status.sinkCodecs).";
    return j;
}

Json ServiceCore::SetSettings(const Json& req)
{
    const Json* s = req.Find("settings");
    if (!s) return Fail("settings object required");
    Settings next;
    std::vector<std::string> warnings;
    {
        std::lock_guard lk(settingsMu_);
        next = settings_;
    }
    Status st = next.MergeFrom(*s, &warnings);
    if (!st) return Fail(st.ToString());
    st = SaveSettings(settingsPath_, next);
    if (!st) return Fail("settings validated but not saved: " + st.ToString());
    {
        std::lock_guard lk(settingsMu_);
        settings_ = next;
    }
    Logger::Instance().SetMinLevel(ParseLevel(next.logLevel));
    controller_->UpdateSettings(next);
    Json j = Ok();
    j["settings"] = next.ToJson();
    Json w = Json::MakeArray();
    for (auto& x : warnings) w.Push(x);
    j["warnings"] = w;
    return j;
}

Json ServiceCore::Connect(const Json& req)
{
    std::string addr = req.Get("address").AsString();
    if (addr.empty()) {
        std::lock_guard lk(settingsMu_);
        addr = settings_.deviceAddress;
    }
    if (addr.empty() && opts_.simulate) addr = kSimAddress;
    auto a = BdAddr::Parse(addr);
    if (!a) return Fail("no device address given or configured");
    controller_->Connect(*a);
    Json j = Ok();
    j["address"] = a->ToString();
    return j;
}

Json ServiceCore::Logs(const Json& req)
{
    uint64_t after = static_cast<uint64_t>(req.Get("after").AsInt(0));
    auto entries = Logger::Instance().Snapshot(after, static_cast<size_t>(req.Get("max").AsInt(300)));
    Json arr = Json::MakeArray();
    for (auto& e : entries) {
        Json j = Json::MakeObject();
        j["seq"] = e.seq;
        j["t"] = e.unixMs;
        j["level"] = LogLevelName(e.level);
        j["component"] = e.component;
        j["message"] = e.message;
        arr.Push(j);
    }
    Json r = Ok();
    r["entries"] = arr;
    return r;
}

Json ServiceCore::Endpoints()
{
    auto eps = EnumerateRenderEndpoints();
    if (!eps) return Fail(eps.status().ToString());
    Json arr = Json::MakeArray();
    for (auto& e : *eps) {
        Json j = Json::MakeObject();
        j["id"] = e.id;
        j["name"] = e.name;
        j["default"] = e.isDefault;
        arr.Push(j);
    }
    Json r = Ok();
    r["endpoints"] = arr;
    return r;
}

// Encodes 0.5 s of a test tone with each available encoder and reports the results.
Json ServiceCore::SelfTest()
{
    Json results = Json::MakeArray();
    auto run = [&](CodecId id, a2dp::StreamConfig cfg) {
        Json r = Json::MakeObject();
        r["codec"] = CodecName(id);
        r["config"] = cfg.Describe();
        auto enc = CreateEncoder(id);
        Status st = enc ? enc->Open(cfg, 1005) : bta::Status{Err::NotAvailable, "no encoder"};
        if (!st) {
            r["ok"] = false;
            r["error"] = st.ToString();
            results.Push(r);
            return;
        }
        ToneSource tone(cfg.sampleRate, enc->Channels());
        std::vector<float> f(enc->InputFramesPerCall() * enc->Channels());
        std::vector<uint8_t> pcm(f.size() * BytesPerSample(enc->InputFormat()));
        std::vector<EncodedPayload> out;
        uint64_t frames = 0, bytes = 0;
        auto t0 = std::chrono::steady_clock::now();
        for (uint32_t done = 0; done < cfg.sampleRate / 2; done += enc->InputFramesPerCall()) {
            tone.Read(f.data(), enc->InputFramesPerCall());
            if (enc->InputFormat() == SampleFormat::S16) FloatToS16(f, reinterpret_cast<int16_t*>(pcm.data()));
            else FloatToS32(f, reinterpret_cast<int32_t*>(pcm.data()));
            if (!(st = enc->Encode(pcm.data(), out))) break;
        }
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        for (auto& p : out) {
            frames += p.frames;
            bytes += p.bytes.size();
        }
        r["ok"] = st.ok() && frames > 0;
        if (!st) r["error"] = st.ToString();
        r["packets"] = static_cast<uint64_t>(out.size());
        r["codecFrames"] = frames;
        r["kbps"] = bytes * 8 / 1000.0 / 0.5;
        r["nominalKbps"] = enc->NominalBitrate() / 1000.0;
        r["cpuMsPerSecondAudio"] = ms * 2;
        results.Push(r);
    };
    a2dp::NegotiationPrefs p;
    p.sampleRate = 48000;
    run(CodecId::Sbc, *a2dp::NegotiateSbc(a2dp::LocalSbcCaps(), p));
    p.ldacQuality = LdacQuality::High;
    auto lc = a2dp::NegotiateLdac(a2dp::LocalLdacCaps(), p);
    run(CodecId::Ldac, *lc);
    Json j = Ok();
    j["results"] = results;
    return j;
}

}  // namespace bta
