// bta2dpctl - command-line client for the BtA2dpSrc service, plus offline diagnostics.
//
//   bta2dpctl status | info | devices | endpoints | logs | selftest
//   bta2dpctl probe <addr>          SDP query of the A2DP sink record (pages the device)
//   bta2dpctl connect [addr] | disconnect
//   bta2dpctl get | set key=value [key=value...]   e.g. set codec=ldac ldacQuality=hq sampleRate=96000
//   bta2dpctl local-devices         discovery without the service (read-only)
//   bta2dpctl simulate [seconds] [codec]   in-process end-to-end run against the simulated sink
#include <windows.h>

#include <cstdio>
#include <string>
#include <thread>

#include "common/log.h"
#include "platform/bt_discovery.h"
#include "platform/ipc.h"
#include "platform/service_core.h"

using namespace bta;

static int Print(const Result<Json>& r)
{
    if (!r) {
        fprintf(stderr, "error: %s\n", r.status().ToString().c_str());
        return 1;
    }
    printf("%s\n", r.value().Dump(true).c_str());
    return r.value().Get("ok").AsBool(true) ? 0 : 1;
}

static Json Cmd(const char* c)
{
    Json j = Json::MakeObject();
    j["cmd"] = c;
    return j;
}

static Json ParseValue(const std::string& v)
{
    if (auto j = Json::Parse(v); j && (j->IsNumber() || j->IsBool())) return *j;
    return Json(v);
}

static int Simulate(int seconds, const std::string& codec)
{
    Logger::Instance().SetMinLevel(LogLevel::Info);
    ServiceOptions o;
    o.simulate = true;
    o.startPipe = false;
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    o.settingsPath = std::wstring(tmp) + L"bta2dpctl-simulate.json";
    DeleteFileW(o.settingsPath.c_str());
    ServiceCore core(o);
    if (Status st = core.Start(); !st) return fprintf(stderr, "%s\n", st.ToString().c_str()), 1;
    Json set = Cmd("set_settings");
    set["settings"]["source"] = "tone";
    set["settings"]["codec"] = codec;
    Print(core.Handle(set));
    Print(core.Handle(Cmd("connect")));
    for (int i = 0; i < seconds; ++i) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        auto st = core.Handle(Cmd("status"))["status"];
        printf("t=%2ds state=%-10s active=%-5s confirmed=%d packets=%llu kbps=%.0f quality=%s sinkDelay=%.1fms\n", i + 1,
               st.Get("state").AsString().c_str(), st.Get("activeCodec").AsString().c_str(), st.Get("codecConfirmed").AsBool(),
               static_cast<unsigned long long>(st.Get("engine").Get("packets").AsInt()), st.Get("engine").Get("measuredKbps").AsNumber(),
               st.Get("engine").Get("quality").AsString().c_str(), st.Get("sinkDelayMs").AsNumber());
    }
    Print(core.Handle(Cmd("status")));
    core.Handle(Cmd("disconnect"));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    DeleteFileW(o.settingsPath.c_str());
    return 0;
}

int main(int argc, char** argv)
{
    SetConsoleOutputCP(CP_UTF8);
    Logger::Instance().EnableStderr(false);
    if (argc < 2) {
        fprintf(stderr,
                "usage: bta2dpctl status|info|devices|endpoints|logs|selftest|probe <addr>|connect [addr]|disconnect|\n"
                "                 get|set k=v...|local-devices|simulate [seconds] [auto|ldac|sbc]\n");
        return 2;
    }
    std::string c = argv[1];
    if (c == "simulate") return Simulate(argc > 2 ? atoi(argv[2]) : 5, argc > 3 ? argv[3] : "auto");
    if (c == "local-devices") {
        auto d = EnumeratePairedDevices();
        if (!d) return fprintf(stderr, "%s\n", d.status().ToString().c_str()), 1;
        Json arr = Json::MakeArray();
        for (auto& x : *d) arr.Push(x.ToJson());
        printf("%s\n", arr.Dump(true).c_str());
        return 0;
    }
    if (c == "probe") {
        if (argc < 3) return fprintf(stderr, "probe <addr>\n"), 2;
        Json j = Cmd("probe");
        j["address"] = argv[2];
        // Probe works without the service too (read-only SDP query).
        auto r = PipeRequest(j, 15000);
        if (!r && r.status().code() == Err::NotAvailable) {
            auto a = BdAddr::Parse(argv[2]);
            if (!a) return fprintf(stderr, "bad address\n"), 2;
            auto rec = QueryA2dpSinkSdp(*a);
            if (!rec) return fprintf(stderr, "%s\n", rec.status().ToString().c_str()), 1;
            printf("%s\n", rec->ToJson().Dump(true).c_str());
            return 0;
        }
        return Print(r);
    }
    if (c == "connect") {
        Json j = Cmd("connect");
        if (argc > 2) j["address"] = argv[2];
        return Print(PipeRequest(j));
    }
    if (c == "get") return Print(PipeRequest(Cmd("get_settings")));
    if (c == "set") {
        Json j = Cmd("set_settings");
        Json s = Json::MakeObject();
        for (int i = 2; i < argc; ++i) {
            std::string kv = argv[i];
            size_t eq = kv.find('=');
            if (eq == std::string::npos) return fprintf(stderr, "expected key=value: %s\n", kv.c_str()), 2;
            std::string k = kv.substr(0, eq), v = kv.substr(eq + 1);
            if (k.rfind("sbc.", 0) == 0) s["sbc"][k.substr(4)] = ParseValue(v);
            else s[k] = ParseValue(v);
        }
        j["settings"] = s;
        return Print(PipeRequest(j));
    }
    if (c == "status" || c == "info" || c == "devices" || c == "endpoints" || c == "logs" || c == "selftest" || c == "disconnect")
        return Print(PipeRequest(Cmd(c.c_str()), 15000));
    fprintf(stderr, "unknown command %s\n", c.c_str());
    return 2;
}
