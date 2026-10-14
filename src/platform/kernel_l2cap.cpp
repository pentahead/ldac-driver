#include "platform/kernel_l2cap.h"

#include <windows.h>
#include <cfgmgr32.h>
#include <initguid.h>

#include <atomic>
#include <format>
#include <mutex>

#include "bta2dp_ioctl.h"
#include "common/log.h"

#pragma comment(lib, "cfgmgr32.lib")

namespace bta {

namespace {

constexpr const char* kLog = "kl2cap";

struct Handle {
    HANDLE h = INVALID_HANDLE_VALUE;
    Handle() = default;
    explicit Handle(HANDLE v) : h(v) {}
    ~Handle()
    {
        if (h != INVALID_HANDLE_VALUE && h) CloseHandle(h);
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    bool ok() const { return h != INVALID_HANDLE_VALUE && h != nullptr; }
};

std::string Win32Err(const char* what, DWORD e)
{
    char* msg = nullptr;
    FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, e, 0,
                   reinterpret_cast<char*>(&msg), 0, nullptr);
    std::string s = std::format("{} failed: {} ({})", what, e, msg ? msg : "");
    if (msg) LocalFree(msg);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    return s;
}

HANDLE OpenDevice(const std::wstring& path)
{
    return CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                       FILE_FLAG_OVERLAPPED, nullptr);
}

// Synchronous ioctl on an overlapped handle with an optional timeout (cancels on expiry).
Status Ioctl(HANDLE h, DWORD code, const void* in, DWORD inLen, void* out, DWORD outLen, DWORD* returned, DWORD timeoutMs)
{
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent) return {Err::Internal, "CreateEvent failed"};
    DWORD n = 0;
    BOOL ok = DeviceIoControl(h, code, const_cast<void*>(in), inLen, out, outLen, &n, &ov);
    DWORD e = ok ? ERROR_SUCCESS : GetLastError();
    if (!ok && e == ERROR_IO_PENDING) {
        DWORD w = WaitForSingleObject(ov.hEvent, timeoutMs);
        if (w == WAIT_TIMEOUT) {
            CancelIoEx(h, &ov);
            GetOverlappedResult(h, &ov, &n, TRUE);
            CloseHandle(ov.hEvent);
            return {Err::Timeout, "driver request timed out"};
        }
        ok = GetOverlappedResult(h, &ov, &n, FALSE);
        e = ok ? ERROR_SUCCESS : GetLastError();
    }
    CloseHandle(ov.hEvent);
    if (returned) *returned = n;
    if (ok) return Status::Ok();
    switch (e) {
    case ERROR_OPERATION_ABORTED: return {Err::Timeout, "request cancelled"};
    case ERROR_DEVICE_NOT_CONNECTED:
    case ERROR_NOT_CONNECTED:
    case ERROR_CONNECTION_ABORTED:
    case ERROR_GRACEFUL_DISCONNECT:
    case ERROR_FILE_NOT_FOUND:
    case ERROR_BAD_COMMAND: return {Err::Disconnected, Win32Err("DeviceIoControl", e)};
    case ERROR_SEM_TIMEOUT: return {Err::Timeout, Win32Err("DeviceIoControl", e)};
    default: return {Err::IoError, Win32Err("DeviceIoControl", e)};
    }
}

class KernelChannel : public IL2capChannel {
public:
    KernelChannel(HANDLE dev, const BTA2DP_CHANNEL_INFO& info) : dev_(dev), info_(info) {}
    ~KernelChannel() override { Close(); }

    Status Send(std::span<const uint8_t> sdu) override
    {
        if (closed_) return {Err::Disconnected, "channel closed"};
        if (sdu.size() > info_.OutMtu) return {Err::InvalidArgument, "SDU exceeds MTU"};
        BTA2DP_CHANNEL_REF ref{info_.ChannelId, 0};
        ++inFlight_;
        // METHOD_IN_DIRECT: data travels in the "output" buffer which the driver reads.
        Status st = Ioctl(dev_.h, IOCTL_BTA2DP_SEND, &ref, sizeof ref, const_cast<uint8_t*>(sdu.data()),
                          static_cast<DWORD>(sdu.size()), nullptr, 2000);
        --inFlight_;
        if (st.code() == Err::Disconnected) closed_ = true;
        return st;
    }

    Status Receive(std::vector<uint8_t>& sdu, uint32_t timeoutMs) override
    {
        if (closed_) return {Err::Disconnected, "channel closed"};
        BTA2DP_CHANNEL_REF ref{info_.ChannelId, 0};
        sdu.resize(info_.InMtu ? info_.InMtu : 1024);
        DWORD n = 0;
        Status st = Ioctl(dev_.h, IOCTL_BTA2DP_RECEIVE, &ref, sizeof ref, sdu.data(), static_cast<DWORD>(sdu.size()), &n, timeoutMs);
        if (!st) {
            if (st.code() == Err::Disconnected) closed_ = true;
            return st;
        }
        if (n == 0) {  // zero-length completion = remote closed the channel
            closed_ = true;
            return {Err::Disconnected, "channel closed by remote"};
        }
        sdu.resize(n);
        return Status::Ok();
    }

    uint16_t OutMtu() const override { return info_.OutMtu; }
    uint16_t InMtu() const override { return info_.InMtu; }
    bool IsOpen() const override { return !closed_; }
    void Close() override
    {
        bool expected = false;
        if (!closed_.compare_exchange_strong(expected, true)) return;
        BTA2DP_CHANNEL_REF ref{info_.ChannelId, 0};
        Ioctl(dev_.h, IOCTL_BTA2DP_CLOSE_CHANNEL, &ref, sizeof ref, nullptr, 0, nullptr, 2000);
        CancelIoEx(dev_.h, nullptr);
    }

    uint32_t PendingSends() const override
    {
        BTA2DP_CHANNEL_REF ref{info_.ChannelId, 0};
        BTA2DP_CHANNEL_INFO ci{};
        DWORD n = 0;
        if (closed_ || !Ioctl(dev_.h, IOCTL_BTA2DP_QUERY_CHANNEL, &ref, sizeof ref, &ci, sizeof ci, &n, 200)) return inFlight_;
        return ci.PendingSends;
    }

private:
    Handle dev_;  // each channel holds its own handle so cancellation is per channel
    BTA2DP_CHANNEL_INFO info_;
    std::atomic<bool> closed_{false};
    std::atomic<uint32_t> inFlight_{0};
};

}  // namespace

std::vector<L2capDriverInstance> EnumerateL2capDriverInstances(std::string* error)
{
    std::vector<L2capDriverInstance> out;
    GUID g = GUID_DEVINTERFACE_BTA2DP_L2CAP;
    ULONG len = 0;
    if (CM_Get_Device_Interface_List_SizeW(&len, &g, nullptr, CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS || len <= 1) {
        if (error) *error = "BtA2dpL2cap driver not installed or not bound to any device";
        return out;
    }
    std::vector<wchar_t> buf(len);
    if (CM_Get_Device_Interface_ListW(&g, nullptr, buf.data(), len, CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS) {
        if (error) *error = "CM_Get_Device_Interface_List failed";
        return out;
    }
    for (const wchar_t* p = buf.data(); *p; p += wcslen(p) + 1) {
        L2capDriverInstance inst;
        inst.interfacePath = p;
        Handle h(OpenDevice(p));
        if (!h.ok()) {
            if (error) *error = Win32Err("open driver interface", GetLastError());
            continue;
        }
        BTA2DP_DEVICE_INFO info{};
        DWORD n = 0;
        if (Ioctl(h.h, IOCTL_BTA2DP_GET_INFO, nullptr, 0, &info, sizeof info, &n, 2000) && n == sizeof info) {
            inst.remote = BdAddr{info.RemoteAddress};
            inst.version = info.Version;
            inst.remoteConnected = (info.Flags & BTA2DP_INFO_FLAG_REMOTE_CONNECTED) != 0;
            out.push_back(inst);
        }
    }
    return out;
}

Result<std::wstring> KernelL2capConnector::FindInterface(BdAddr addr) const
{
    std::string err;
    auto list = EnumerateL2capDriverInstances(&err);
    for (const auto& i : list) {
        if (i.remote == addr) {
            if (i.version != BTA2DP_IOCTL_VERSION)
                return Status{Err::NotSupported, std::format("driver contract v{} != expected v{}", i.version, BTA2DP_IOCTL_VERSION)};
            return i.interfacePath;
        }
    }
    if (list.empty()) return Status{Err::NotAvailable, err.empty() ? "BtA2dpL2cap driver not present" : err};
    return Status{Err::NotAvailable, "BtA2dpL2cap driver is not bound to " + addr.ToString() +
                                         " (use the installer to switch this device's A2DP driver)"};
}

Status KernelL2capConnector::CheckAvailable(BdAddr addr) const
{
    auto r = FindInterface(addr);
    return r ? Status::Ok() : r.status();
}

Result<std::unique_ptr<IL2capChannel>> KernelL2capConnector::Connect(const L2capConnectParams& p)
{
    auto path = FindInterface(p.addr);
    if (!path) return path.status();
    HANDLE h = OpenDevice(*path);
    if (h == INVALID_HANDLE_VALUE) return Status{Err::IoError, Win32Err("open driver", GetLastError())};
    BTA2DP_OPEN_REQUEST req{p.psm, p.inMtu, p.flushTimeoutMs, 0, p.timeoutMs};
    BTA2DP_CHANNEL_INFO info{};
    DWORD n = 0;
    Status st = Ioctl(h, IOCTL_BTA2DP_OPEN_CHANNEL, &req, sizeof req, &info, sizeof info, &n, p.timeoutMs + 2000);
    if (!st || n != sizeof info) {
        CloseHandle(h);
        return st ? Status{Err::ProtocolError, "short OPEN_CHANNEL reply"} : st;
    }
    BTA_LOGI(kLog, "L2CAP channel {} open psm=0x{:04X} outMtu={} inMtu={}", info.ChannelId, p.psm, info.OutMtu, info.InMtu);
    return std::unique_ptr<IL2capChannel>(std::make_unique<KernelChannel>(h, info));
}

Result<std::unique_ptr<IL2capChannel>> KernelL2capConnector::Accept(BdAddr addr, uint16_t psm, uint32_t timeoutMs)
{
    auto path = FindInterface(addr);
    if (!path) return path.status();
    HANDLE h = OpenDevice(*path);
    if (h == INVALID_HANDLE_VALUE) return Status{Err::IoError, Win32Err("open driver", GetLastError())};
    BTA2DP_ACCEPT_REQUEST req{psm, 1024, timeoutMs};
    BTA2DP_CHANNEL_INFO info{};
    DWORD n = 0;
    Status st = Ioctl(h, IOCTL_BTA2DP_ACCEPT_CHANNEL, &req, sizeof req, &info, sizeof info, &n, timeoutMs + 2000);
    if (!st || n != sizeof info) {
        CloseHandle(h);
        return st ? Status{Err::ProtocolError, "short ACCEPT_CHANNEL reply"} : st;
    }
    return std::unique_ptr<IL2capChannel>(std::make_unique<KernelChannel>(h, info));
}

}  // namespace bta
