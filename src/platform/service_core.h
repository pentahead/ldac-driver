// Service command layer: owns settings persistence, discovery, the session controller and the IPC
// server. Used by bta2dp-service.exe (real or --simulate) and by integration tests.
#pragma once

#include <memory>
#include <mutex>

#include "core/session_controller.h"
#include "platform/ipc.h"
#include "sim/sim_sink.h"
#include "transport/loopback.h"

namespace bta {

struct ServiceOptions {
    bool simulate = false;          // loopback transport + simulated LDAC sink (no hardware)
    sim::SimSinkOptions simSink;
    std::wstring settingsPath;      // empty = DefaultSettingsPath()
    std::wstring pipeName = kPipeName;
    bool startPipe = true;
    int maxReconnectAttempts = 3;
};

class ServiceCore {
public:
    explicit ServiceCore(ServiceOptions opts);
    ~ServiceCore();
    Status Start();
    void Stop();
    Json Handle(const Json& request);  // thread-safe
    SessionController& Controller() { return *controller_; }

    static constexpr const char* kSimAddress = "00:00:00:5A:51:4D";

private:
    Json Devices();
    Json Probe(const Json& req);
    Json SetSettings(const Json& req);
    Json Connect(const Json& req);
    Json Logs(const Json& req);
    Json Endpoints();
    Json SelfTest();
    Json Info();

    ServiceOptions opts_;
    std::wstring settingsPath_;
    std::unique_ptr<IL2capConnector> connector_;
    std::unique_ptr<sim::SimulatedSink> simSink_;
    std::unique_ptr<SessionController> controller_;
    std::unique_ptr<PipeServer> pipe_;
    std::mutex settingsMu_;
    Settings settings_;
};

Result<std::unique_ptr<IPcmSource>> CreateSourceFromSettings(const Settings& s);

}  // namespace bta
