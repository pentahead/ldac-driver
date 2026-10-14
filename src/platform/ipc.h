// Newline-delimited JSON request/response over a named pipe (\\.\pipe\BtA2dpSrc).
// Requests: {"cmd": "...", ...}  Responses: {"ok": true|false, "error": "...", ...}
#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include "common/json.h"
#include "common/status.h"

namespace bta {

constexpr const wchar_t* kPipeName = L"\\\\.\\pipe\\BtA2dpSrc";

class PipeServer {
public:
    using Handler = std::function<Json(const Json& request)>;
    // SDDL: SYSTEM + Administrators full; interactive users read/write (config app runs unelevated).
    explicit PipeServer(Handler h, std::wstring name = kPipeName);
    ~PipeServer();
    Status Start();
    void Stop();

private:
    void AcceptLoop();
    void ServeClient(void* pipe);

    Handler handler_;
    std::wstring name_;
    std::thread acceptThread_;
    std::atomic<bool> stop_{false};
    void* stopEvent_ = nullptr;
    void* firstPipe_ = nullptr;  // created synchronously in Start() so clients never race the server
    void* CreateInstance();
    std::atomic<int> activeClients_{0};
};

// Blocking single request. Connects, sends, waits for one response line.
Result<Json> PipeRequest(const Json& request, uint32_t timeoutMs = 5000, const std::wstring& name = kPipeName);

}  // namespace bta
