#include "platform/ipc.h"

#include <windows.h>
#include <sddl.h>

#include <format>

#include "common/log.h"

#pragma comment(lib, "advapi32.lib")

namespace bta {

namespace {
constexpr const char* kLog = "ipc";
constexpr size_t kMaxRequest = 64 * 1024;

bool WriteAll(HANDLE h, const std::string& s, OVERLAPPED* ov)
{
    DWORD n = 0;
    ResetEvent(ov->hEvent);
    if (!WriteFile(h, s.data(), static_cast<DWORD>(s.size()), &n, ov)) {
        if (GetLastError() != ERROR_IO_PENDING) return false;
        if (!GetOverlappedResult(h, ov, &n, TRUE)) return false;
    }
    return n == s.size();
}
}  // namespace

PipeServer::PipeServer(Handler h, std::wstring name) : handler_(std::move(h)), name_(std::move(name)) {}

PipeServer::~PipeServer() { Stop(); }

void* PipeServer::CreateInstance()
{
    PSECURITY_DESCRIPTOR sd = nullptr;
    ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;IU)", SDDL_REVISION_1, &sd, nullptr);
    SECURITY_ATTRIBUTES sa{sizeof sa, sd, FALSE};
    HANDLE pipe = CreateNamedPipeW(name_.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                                   PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                   PIPE_UNLIMITED_INSTANCES, 64 * 1024, 64 * 1024, 0, sd ? &sa : nullptr);
    if (sd) LocalFree(sd);
    return pipe == INVALID_HANDLE_VALUE ? nullptr : pipe;
}

Status PipeServer::Start()
{
    stop_ = false;
    firstPipe_ = CreateInstance();
    if (!firstPipe_) return {Err::IoError, std::format("CreateNamedPipe failed ({})", GetLastError())};
    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    acceptThread_ = std::thread([this] { AcceptLoop(); });
    return Status::Ok();
}

void PipeServer::Stop()
{
    if (!stopEvent_) return;
    stop_ = true;
    SetEvent(stopEvent_);
    if (acceptThread_.joinable()) acceptThread_.join();
    // Client threads observe stopEvent_ and exit; wait for them to drain.
    for (int i = 0; i < 300 && activeClients_.load() > 0; ++i) Sleep(10);
    CloseHandle(stopEvent_);
    stopEvent_ = nullptr;
}

void PipeServer::AcceptLoop()
{
    while (!stop_) {
        HANDLE pipe = firstPipe_ ? static_cast<HANDLE>(firstPipe_) : static_cast<HANDLE>(CreateInstance());
        firstPipe_ = nullptr;
        if (!pipe) {
            BTA_LOGE(kLog, "CreateNamedPipe failed: {}", GetLastError());
            WaitForSingleObject(stopEvent_, 1000);
            continue;
        }
        OVERLAPPED ov{};
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        BOOL connected = ConnectNamedPipe(pipe, &ov);
        DWORD e = connected ? ERROR_PIPE_CONNECTED : GetLastError();
        if (e == ERROR_IO_PENDING) {
            HANDLE w[2] = {ov.hEvent, stopEvent_};
            DWORD r = WaitForMultipleObjects(2, w, FALSE, INFINITE);
            if (r != WAIT_OBJECT_0) {
                CancelIoEx(pipe, &ov);
                CloseHandle(ov.hEvent);
                CloseHandle(pipe);
                break;
            }
            DWORD n;
            e = GetOverlappedResult(pipe, &ov, &n, FALSE) ? ERROR_PIPE_CONNECTED : GetLastError();
        }
        CloseHandle(ov.hEvent);
        if (e != ERROR_PIPE_CONNECTED) {
            CloseHandle(pipe);
            continue;
        }
        ++activeClients_;
        std::thread([this, pipe] {
            ServeClient(pipe);
            --activeClients_;
        }).detach();
    }
}

void PipeServer::ServeClient(void* p)
{
    HANDLE pipe = static_cast<HANDLE>(p);
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::string buf;
    char chunk[4096];
    while (!stop_) {
        DWORD n = 0;
        ResetEvent(ov.hEvent);
        BOOL ok = ReadFile(pipe, chunk, sizeof chunk, &n, &ov);
        if (!ok && GetLastError() == ERROR_IO_PENDING) {
            HANDLE w[2] = {ov.hEvent, stopEvent_};
            if (WaitForMultipleObjects(2, w, FALSE, INFINITE) != WAIT_OBJECT_0) {
                CancelIoEx(pipe, &ov);
                break;
            }
            ok = GetOverlappedResult(pipe, &ov, &n, FALSE);
        }
        if (!ok || n == 0) break;
        buf.append(chunk, n);
        if (buf.size() > kMaxRequest) break;  // abusive client
        size_t nl;
        while ((nl = buf.find('\n')) != std::string::npos) {
            std::string line = buf.substr(0, nl);
            buf.erase(0, nl + 1);
            std::string err;
            Json resp;
            auto req = Json::Parse(line, &err);
            if (!req || !req->IsObject()) {
                resp["ok"] = false;
                resp["error"] = "invalid JSON request: " + err;
            } else {
                try {
                    resp = handler_(*req);
                } catch (const std::exception& ex) {
                    resp = Json::MakeObject();
                    resp["ok"] = false;
                    resp["error"] = std::string("internal error: ") + ex.what();
                }
            }
            if (!WriteAll(pipe, resp.Dump() + "\n", &ov)) goto out;
        }
    }
out:
    CloseHandle(ov.hEvent);
    FlushFileBuffers(pipe);
    DisconnectNamedPipe(pipe);
    CloseHandle(pipe);
}

Result<Json> PipeRequest(const Json& request, uint32_t timeoutMs, const std::wstring& name)
{
    if (!WaitNamedPipeW(name.c_str(), timeoutMs)) {
        DWORD e = GetLastError();
        if (e == ERROR_FILE_NOT_FOUND)
            return Status{Err::NotAvailable, "BtA2dpSrc service is not running (pipe not found)"};
        return Status{Err::Timeout, std::format("pipe busy ({})", e)};
    }
    HANDLE h = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (h == INVALID_HANDLE_VALUE) return Status{Err::IoError, std::format("open pipe failed ({})", GetLastError())};
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    auto cleanup = [&] {
        CloseHandle(ov.hEvent);
        CloseHandle(h);
    };
    if (!WriteAll(h, request.Dump() + "\n", &ov)) {
        cleanup();
        return Status{Err::IoError, "pipe write failed"};
    }
    std::string buf;
    char chunk[8192];
    ULONGLONG deadline = GetTickCount64() + timeoutMs;
    while (buf.find('\n') == std::string::npos) {
        DWORD n = 0;
        ResetEvent(ov.hEvent);
        BOOL ok = ReadFile(h, chunk, sizeof chunk, &n, &ov);
        if (!ok && GetLastError() == ERROR_IO_PENDING) {
            ULONGLONG now = GetTickCount64();
            DWORD wait = now >= deadline ? 0 : static_cast<DWORD>(deadline - now);
            if (WaitForSingleObject(ov.hEvent, wait) != WAIT_OBJECT_0) {
                CancelIoEx(h, &ov);
                cleanup();
                return Status{Err::Timeout, "service did not respond"};
            }
            ok = GetOverlappedResult(h, &ov, &n, FALSE);
        }
        if (!ok || n == 0) {
            cleanup();
            return Status{Err::Disconnected, "pipe closed"};
        }
        buf.append(chunk, n);
    }
    cleanup();
    std::string err;
    auto j = Json::Parse(buf.substr(0, buf.find('\n')), &err);
    if (!j) return Status{Err::ProtocolError, "bad response: " + err};
    return *j;
}

}  // namespace bta
