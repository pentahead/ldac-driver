// bta2dp-service.exe - BtA2dpSrc codec/session service.
//
//   bta2dp-service --console [--simulate] [--verbose]   run in the foreground (development)
//   bta2dp-service install | uninstall                  register/remove the Windows service (admin)
//   (no arguments)                                      started by the SCM
#include <windows.h>
#include <shlobj.h>

#include <cstdio>
#include <memory>
#include <string>

#include "common/log.h"
#include "platform/service_core.h"

using namespace bta;

namespace {

constexpr wchar_t kServiceName[] = L"BtA2dpSrc";
constexpr wchar_t kDisplayName[] = L"BtA2dpSrc Bluetooth A2DP Source (LDAC/SBC)";

SERVICE_STATUS_HANDLE g_statusHandle = nullptr;
SERVICE_STATUS g_status{};
HANDLE g_stopEvent = nullptr;
bool g_simulate = false;

void ReportStatus(DWORD state, DWORD exitCode = NO_ERROR, DWORD waitHint = 0)
{
    static DWORD checkpoint = 1;
    g_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_status.dwCurrentState = state;
    g_status.dwWin32ExitCode = exitCode;
    g_status.dwWaitHint = waitHint;
    g_status.dwControlsAccepted = state == SERVICE_START_PENDING ? 0 : SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
    g_status.dwCheckPoint = (state == SERVICE_RUNNING || state == SERVICE_STOPPED) ? 0 : checkpoint++;
    SetServiceStatus(g_statusHandle, &g_status);
}

DWORD WINAPI CtrlHandler(DWORD ctrl, DWORD, void*, void*)
{
    if (ctrl == SERVICE_CONTROL_STOP || ctrl == SERVICE_CONTROL_SHUTDOWN) {
        ReportStatus(SERVICE_STOP_PENDING, NO_ERROR, 5000);
        SetEvent(g_stopEvent);
        return NO_ERROR;
    }
    return ctrl == SERVICE_CONTROL_INTERROGATE ? NO_ERROR : ERROR_CALL_NOT_IMPLEMENTED;
}

std::wstring LogPath()
{
    PWSTR pd = nullptr;
    std::wstring base = L"C:\\ProgramData";
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &pd))) base = pd;
    CoTaskMemFree(pd);
    std::wstring dir = base + L"\\BtA2dpSrc";
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    return dir + L"\\service.log";
}

int RunCore(bool console)
{
    ServiceOptions o;
    o.simulate = g_simulate;
    ServiceCore core(o);
    Status st = core.Start();
    if (!st) {
        BTA_LOGE("service", "start failed: {}", st.ToString());
        return 1;
    }
    if (!console) ReportStatus(SERVICE_RUNNING);
    WaitForSingleObject(g_stopEvent, INFINITE);
    BTA_LOGI("service", "stopping");
    core.Stop();
    return 0;
}

void WINAPI ServiceMain(DWORD, LPWSTR*)
{
    g_statusHandle = RegisterServiceCtrlHandlerExW(kServiceName, CtrlHandler, nullptr);
    if (!g_statusHandle) return;
    ReportStatus(SERVICE_START_PENDING, NO_ERROR, 5000);
    Logger::Instance().EnableStderr(false);
    Logger::Instance().OpenFile(LogPath());
    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    int rc = RunCore(false);
    ReportStatus(SERVICE_STOPPED, rc ? ERROR_SERVICE_SPECIFIC_ERROR : NO_ERROR);
}

BOOL WINAPI ConsoleCtrl(DWORD)
{
    SetEvent(g_stopEvent);
    return TRUE;
}

int Install()
{
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring cmd = L"\"" + std::wstring(path) + L"\"";
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
    if (!scm) {
        fprintf(stderr, "OpenSCManager failed (%lu). Run from an elevated prompt.\n", GetLastError());
        return 1;
    }
    // Manual start: the service is never started automatically by installation.
    SC_HANDLE svc = CreateServiceW(scm, kServiceName, kDisplayName, SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS,
                                   SERVICE_DEMAND_START, SERVICE_ERROR_NORMAL, cmd.c_str(), nullptr, nullptr, nullptr,
                                   nullptr /*LocalSystem*/, nullptr);
    if (!svc) {
        fprintf(stderr, "CreateService failed (%lu)\n", GetLastError());
        CloseServiceHandle(scm);
        return 1;
    }
    SERVICE_DESCRIPTIONW desc{const_cast<wchar_t*>(L"Streams audio to Bluetooth headphones using LDAC or SBC (BtA2dpSrc).")};
    ChangeServiceConfig2W(svc, SERVICE_CONFIG_DESCRIPTION, &desc);
    // Restart on failure (3 times, 5 s apart).
    SC_ACTION actions[3] = {{SC_ACTION_RESTART, 5000}, {SC_ACTION_RESTART, 5000}, {SC_ACTION_NONE, 0}};
    SERVICE_FAILURE_ACTIONSW fa{86400, nullptr, nullptr, 3, actions};
    ChangeServiceConfig2W(svc, SERVICE_CONFIG_FAILURE_ACTIONS, &fa);
    printf("Service installed (manual start). Start with: sc start %ls\n", kServiceName);
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return 0;
}

int Uninstall()
{
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return 1;
    SC_HANDLE svc = OpenServiceW(scm, kServiceName, SERVICE_STOP | DELETE | SERVICE_QUERY_STATUS);
    if (!svc) {
        fprintf(stderr, "service not installed\n");
        CloseServiceHandle(scm);
        return 0;
    }
    SERVICE_STATUS ss;
    ControlService(svc, SERVICE_CONTROL_STOP, &ss);
    for (int i = 0; i < 50 && QueryServiceStatus(svc, &ss) && ss.dwCurrentState != SERVICE_STOPPED; ++i) Sleep(100);
    BOOL ok = DeleteService(svc);
    printf(ok ? "Service removed.\n" : "DeleteService failed (%lu)\n", GetLastError());
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return ok ? 0 : 1;
}

}  // namespace

int wmain(int argc, wchar_t** argv)
{
    bool console = false, verbose = false;
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"install") return Install();
        if (a == L"uninstall") return Uninstall();
        if (a == L"--console") console = true;
        else if (a == L"--simulate") g_simulate = true;
        else if (a == L"--verbose") verbose = true;
        else {
            fprintf(stderr, "usage: bta2dp-service [--console [--simulate] [--verbose]] | install | uninstall\n");
            return 2;
        }
    }
    if (console) {
        g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        SetConsoleCtrlHandler(ConsoleCtrl, TRUE);
        if (verbose) Logger::Instance().SetMinLevel(LogLevel::Debug);
        Logger::Instance().OpenFile(LogPath());
        printf("BtA2dpSrc service (console%s). Ctrl+C to stop.\n", g_simulate ? ", SIMULATION" : "");
        return RunCore(true);
    }
    SERVICE_TABLE_ENTRYW table[] = {{const_cast<wchar_t*>(kServiceName), ServiceMain}, {nullptr, nullptr}};
    if (!StartServiceCtrlDispatcherW(table)) {
        fprintf(stderr, "Not started by the SCM. For development run: bta2dp-service --console [--simulate]\n");
        return 1;
    }
    return 0;
}
