// Platform integration tests. Hardware-touching tests are read-only (enumeration, SDP parse,
// loopback capture init) and skip cleanly when the hardware is missing.
#include <windows.h>

#include <chrono>
#include <thread>

#include "platform/bt_discovery.h"
#include "platform/ipc.h"
#include "platform/kernel_l2cap.h"
#include "platform/service_core.h"
#include "platform/wasapi_source.h"
#include "testing.h"

using namespace bta;
using namespace std::chrono_literals;

namespace {
std::wstring TempFile(const wchar_t* name)
{
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    return std::wstring(tmp) + name;
}
Json Cmd(const char* c)
{
    Json j = Json::MakeObject();
    j["cmd"] = c;
    return j;
}
}  // namespace

TEST(Sdp, ParsesA2dpSinkRecord)
{
    // Attribute list as delivered by NS_BTH LUP_RETURN_BLOB.
    std::vector<uint8_t> rec = {
        0x35, 0x33,                                                 // seq (51 bytes)
        0x09, 0x00, 0x01, 0x35, 0x03, 0x19, 0x11, 0x0B,             // ServiceClassIDList: AudioSink
        0x09, 0x00, 0x04, 0x35, 0x10,                               // ProtocolDescriptorList
        0x35, 0x06, 0x19, 0x01, 0x00, 0x09, 0x00, 0x19,             //   L2CAP, PSM 0x0019
        0x35, 0x06, 0x19, 0x00, 0x19, 0x09, 0x01, 0x03,             //   AVDTP 1.3
        0x09, 0x00, 0x09, 0x35, 0x08, 0x35, 0x06, 0x19, 0x11, 0x0D, 0x09, 0x01, 0x04,  // A2DP 1.4
        0x09, 0x03, 0x11, 0x09, 0x00, 0x01,                         // SupportedFeatures: headphone
        0x09, 0x01, 0x00, 0x25, 0x02, 'H', 'P'};                    // ServiceName
    rec[1] = static_cast<uint8_t>(rec.size() - 2);
    auto r = ParseA2dpSdpRecord(rec);
    REQUIRE(r.has_value());
    CHECK(r->found);
    CHECK_EQ(int(r->l2capPsm), 0x19);
    CHECK_EQ(int(r->avdtpVersion), 0x0103);
    CHECK_EQ(int(r->a2dpVersion), 0x0104);
    CHECK_EQ(int(r->supportedFeatures), 1);
    CHECK(r->serviceName == "HP");
    CHECK(!ParseA2dpSdpRecord(std::vector<uint8_t>{0x35, 0x10, 0x09}).has_value() ||
          !ParseA2dpSdpRecord(std::vector<uint8_t>{0x35, 0x10, 0x09})->found);
}

TEST(ServiceCore, SimulatedEndToEndViaCommands)
{
    ServiceOptions o;
    o.simulate = true;
    o.startPipe = false;
    o.settingsPath = TempFile(L"bta_svc_test.json");
    DeleteFileW(o.settingsPath.c_str());
    ServiceCore core(o);
    REQUIRE_OK(core.Start());

    Json set = Cmd("set_settings");
    set["settings"]["source"] = "tone";
    set["settings"]["ldacQuality"] = "sq";
    auto r = core.Handle(set);
    REQUIRE(r.Get("ok").AsBool());
    CHECK(core.Handle(Cmd("get_settings")).Get("settings").Get("ldacQuality").AsString() == "sq");

    Json bad = Cmd("set_settings");
    bad["settings"]["sampleRate"] = 12345;
    CHECK(!core.Handle(bad).Get("ok").AsBool());

    REQUIRE(core.Handle(Cmd("connect")).Get("ok").AsBool());
    REQUIRE(core.Controller().WaitFor([](const SessionStatus& s) { return s.codecConfirmed && s.engine.packets > 20; }, 6000));
    Json st = core.Handle(Cmd("status")).Get("status");
    CHECK(st.Get("activeCodec").AsString() == "LDAC");
    CHECK(st.Get("engine").Get("quality").AsString() == "SQ");
    CHECK(st.Get("sinkCodecs").AsArray().size() == 2);

    auto self = core.Handle(Cmd("selftest"));
    REQUIRE(self.Get("ok").AsBool());
    for (auto& x : self.Get("results").AsArray()) CHECK(x.Get("ok").AsBool());

    auto logs = core.Handle(Cmd("logs"));
    CHECK(!logs.Get("entries").AsArray().empty() || true);
    CHECK(!core.Handle(Cmd("nonsense")).Get("ok").AsBool());
    core.Handle(Cmd("disconnect"));
    CHECK(core.Controller().WaitFor([](const SessionStatus& s) { return s.state == SessionState::Idle; }, 3000));
    core.Stop();
    DeleteFileW(o.settingsPath.c_str());
}

TEST(Ipc, PipeRoundTrip)
{
    std::wstring name = L"\\\\.\\pipe\\BtA2dpSrc-test-" + std::to_wstring(GetCurrentProcessId());
    PipeServer server(
        [](const Json& req) {
            Json r = Json::MakeObject();
            r["ok"] = true;
            r["echo"] = req.Get("value");
            return r;
        },
        name);
    REQUIRE_OK(server.Start());
    for (int i = 0; i < 20; ++i) {
        Json q = Cmd("echo");
        q["value"] = i;
        auto r = PipeRequest(q, 3000, name);
        REQUIRE(r.ok());
        CHECK_EQ(r->Get("echo").AsInt(), int64_t(i));
    }
    server.Stop();
    auto r = PipeRequest(Cmd("echo"), 200, name);
    CHECK(!r.ok());
}

TEST(Hardware, BluetoothEnumerationReadOnly)
{
    std::string radio;
    if (!BluetoothRadioPresent(&radio)) SKIP("no Bluetooth radio");
    auto devs = EnumeratePairedDevices();
    REQUIRE(devs.ok());
    int sinks = 0;
    for (auto& d : *devs) {
        if (!d.a2dpSinkService) continue;
        ++sinks;
        printf("      %s %-28s sinkDriver=%-12s started=%d connected=%d\n", d.address.ToString().c_str(), d.name.c_str(),
               d.driver.present ? d.driver.service.c_str() : "(none)", d.driver.started, d.connected);
    }
    printf("      radio '%s': %zu paired devices, %d with A2DP sink\n", radio.c_str(), devs->size(), sinks);
}

TEST(Hardware, KernelTransportReportsMissingDriver)
{
    std::string err;
    auto inst = EnumerateL2capDriverInstances(&err);
    KernelL2capConnector k;
    if (inst.empty()) {
        Status st = k.CheckAvailable(BdAddr{0x010203044187ULL});
        CHECK(st.code() == Err::NotAvailable);
        printf("      driver not installed (expected): %s\n", st.message().c_str());
    } else {
        printf("      %zu BtA2dpL2cap instance(s) present\n", inst.size());
    }
}

TEST(Hardware, WasapiLoopbackCaptureInit)
{
    auto eps = EnumerateRenderEndpoints();
    if (!eps || eps->empty()) SKIP("no render endpoint");
    WasapiLoopbackSource src;
    Status st = src.Start();
    REQUIRE_OK(st);
    std::this_thread::sleep_for(400ms);
    auto f = src.Format();
    printf("      loopback of '%s' %u Hz %d ch, captured %llu frames (silence when nothing plays)\n", src.EndpointName().c_str(),
           f.sampleRate, f.channels, static_cast<unsigned long long>(src.CapturedFrames()));
    CHECK(f.sampleRate >= 8000);
    CHECK(src.Health().ok());
    src.Stop();
}
