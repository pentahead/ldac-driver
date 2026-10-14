// bta2dp-switch.exe - per-device A2DP sink driver switcher (documented SetupAPI / newdev APIs).
//
//   bta2dp-switch list                                   A2DP sink devnodes and their current driver
//   bta2dp-switch drivers <addr>                         compatible drivers in the driver store
//   bta2dp-switch use <addr> <inf-name|inf-path> [--dry-run]
//                                                        install a specific driver on ONE device (admin);
//                                                        an INF path is used directly without staging
//   bta2dp-switch restore <addr> [--dry-run]             return the device to its journaled previous driver
//                                                        (default: microsoft_bluetooth_a2dp_src.inf)
//
// Only the selected device is changed. The previous driver is journaled in
// %ProgramData%\BtA2dpSrc\driver-journal.json before any change so it can always be restored.
#include <windows.h>
#include <cfgmgr32.h>
#include <newdev.h>
#include <setupapi.h>
#include <shlobj.h>

#include <cstdio>
#include <cwctype>
#include <format>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "common/json.h"
#include "common/types.h"

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "newdev.lib")

using namespace bta;

namespace {

constexpr wchar_t kSinkPrefix[] = L"BTHENUM\\{0000110B-0000-1000-8000-00805F9B34FB}";
constexpr char kInboxInf[] = "microsoft_bluetooth_a2dp_src.inf";

struct SinkDev {
    std::wstring instanceId;
    std::string address;
    std::string name;
    std::string inf, service, provider;
};

std::wstring Upper(std::wstring s)
{
    for (auto& c : s) c = static_cast<wchar_t>(towupper(c));
    return s;
}

std::string RegProp(HDEVINFO devs, SP_DEVINFO_DATA* did, DWORD prop)
{
    wchar_t buf[512];
    DWORD type = 0;
    if (!SetupDiGetDeviceRegistryPropertyW(devs, did, prop, &type, reinterpret_cast<PBYTE>(buf), sizeof buf, nullptr)) return {};
    return WideToUtf8(buf);
}

std::string DriverKeyValue(HDEVINFO devs, SP_DEVINFO_DATA* did, const wchar_t* value)
{
    HKEY k = SetupDiOpenDevRegKey(devs, did, DICS_FLAG_GLOBAL, 0, DIREG_DRV, KEY_READ);
    if (k == INVALID_HANDLE_VALUE) return {};
    wchar_t buf[512];
    DWORD len = sizeof buf, type = 0;
    std::string out;
    if (RegQueryValueExW(k, value, nullptr, &type, reinterpret_cast<BYTE*>(buf), &len) == ERROR_SUCCESS && type == REG_SZ)
        out = WideToUtf8(buf);
    RegCloseKey(k);
    return out;
}

std::string AddressFromInstance(const std::wstring& id)
{
    size_t us = id.rfind(L'_');
    size_t amp = id.rfind(L'&', us);
    if (us == std::wstring::npos || amp == std::wstring::npos || us - amp - 1 != 12) return {};
    auto a = BdAddr::Parse(WideToUtf8(id.substr(amp + 1, 12)));
    return a ? a->ToString() : std::string{};
}

// Calls fn(devs, did, sink) for each present A2DP sink devnode.
template <typename Fn>
bool ForEachSink(Fn fn)
{
    HDEVINFO devs = SetupDiGetClassDevsW(nullptr, L"BTHENUM", nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (devs == INVALID_HANDLE_VALUE) return false;
    SP_DEVINFO_DATA did{sizeof did};
    for (DWORD i = 0; SetupDiEnumDeviceInfo(devs, i, &did); ++i) {
        wchar_t id[MAX_DEVICE_ID_LEN];
        if (!SetupDiGetDeviceInstanceIdW(devs, &did, id, MAX_DEVICE_ID_LEN, nullptr)) continue;
        if (Upper(id).rfind(kSinkPrefix, 0) != 0) continue;
        SinkDev s;
        s.instanceId = id;
        s.address = AddressFromInstance(Upper(id));
        s.name = RegProp(devs, &did, SPDRP_FRIENDLYNAME);
        if (s.name.empty()) s.name = RegProp(devs, &did, SPDRP_DEVICEDESC);
        s.service = RegProp(devs, &did, SPDRP_SERVICE);
        s.inf = DriverKeyValue(devs, &did, L"InfPath");
        s.provider = DriverKeyValue(devs, &did, L"ProviderName");
        if (fn(devs, &did, s)) break;
    }
    SetupDiDestroyDeviceInfoList(devs);
    return true;
}

std::wstring JournalPath()
{
    PWSTR pd = nullptr;
    std::wstring base = L"C:\\ProgramData";
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &pd))) base = pd;
    CoTaskMemFree(pd);
    std::wstring dir = base + L"\\BtA2dpSrc";
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    return dir + L"\\driver-journal.json";
}

Json LoadJournal()
{
    std::ifstream f(JournalPath(), std::ios::binary);
    if (!f) return Json::MakeObject();
    std::stringstream ss;
    ss << f.rdbuf();
    auto j = Json::Parse(ss.str());
    return j && j->IsObject() ? *j : Json::MakeObject();
}

bool SaveJournal(const Json& j)
{
    std::wstring p = JournalPath(), tmp = p + L".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << j.Dump(true);
    }
    return MoveFileExW(tmp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
}

std::string FileName(const std::string& path)
{
    size_t p = path.find_last_of("\\/");
    return p == std::string::npos ? path : path.substr(p + 1);
}

bool IEquals(const std::string& a, const std::string& b)
{
    return a.size() == b.size() && _stricmp(a.c_str(), b.c_str()) == 0;
}

struct DriverChoice {
    SP_DRVINFO_DATA_W info{};
    std::string inf, desc, provider;
    DWORD rank = 0;
};

// Enumerates compatible drivers for the device: from the driver store, or (infPath non-empty) from one
// specific INF file only (DI_ENUMSINGLEINF) so the package never needs to be pre-staged.
std::vector<DriverChoice> CompatibleDrivers(HDEVINFO devs, SP_DEVINFO_DATA* did, const std::wstring& infPath = {})
{
    std::vector<DriverChoice> out;
    if (!infPath.empty()) {
        SP_DEVINSTALL_PARAMS_W ip{sizeof ip};
        if (!SetupDiGetDeviceInstallParamsW(devs, did, &ip)) return out;
        ip.Flags |= DI_ENUMSINGLEINF;
        wcsncpy_s(ip.DriverPath, infPath.c_str(), _TRUNCATE);
        if (!SetupDiSetDeviceInstallParamsW(devs, did, &ip)) return out;
    }
    if (!SetupDiBuildDriverInfoList(devs, did, SPDIT_COMPATDRIVER)) return out;
    SP_DRVINFO_DATA_W dd{sizeof dd};
    for (DWORD i = 0; SetupDiEnumDriverInfoW(devs, did, SPDIT_COMPATDRIVER, i, &dd); ++i) {
        DriverChoice c;
        c.info = dd;
        c.desc = WideToUtf8(dd.Description);
        c.provider = WideToUtf8(dd.ProviderName);
        std::vector<BYTE> buf(4096);
        auto* det = reinterpret_cast<SP_DRVINFO_DETAIL_DATA_W*>(buf.data());
        det->cbSize = sizeof(SP_DRVINFO_DETAIL_DATA_W);
        DWORD need = 0;
        if (SetupDiGetDriverInfoDetailW(devs, did, &dd, det, static_cast<DWORD>(buf.size()), &need) ||
            GetLastError() == ERROR_INSUFFICIENT_BUFFER)
            c.inf = FileName(WideToUtf8(det->InfFileName));
        SP_DRVINSTALL_PARAMS ip{sizeof ip};
        if (SetupDiGetDriverInstallParamsW(devs, did, &dd, &ip)) c.rank = ip.Rank;
        out.push_back(c);
        dd = SP_DRVINFO_DATA_W{sizeof dd};
    }
    return out;
}

int CmdList()
{
    printf("%-18s %-28s %-10s %-36s %s\n", "ADDRESS", "NAME", "SERVICE", "INF", "PROVIDER");
    ForEachSink([](HDEVINFO, SP_DEVINFO_DATA*, const SinkDev& s) {
        printf("%-18s %-28.28s %-10s %-36s %s\n", s.address.c_str(), s.name.c_str(), s.service.c_str(), s.inf.c_str(), s.provider.c_str());
        return false;
    });
    return 0;
}

int CmdDrivers(const std::string& addr)
{
    bool found = false;
    ForEachSink([&](HDEVINFO devs, SP_DEVINFO_DATA* did, const SinkDev& s) {
        if (!IEquals(s.address, addr)) return false;
        found = true;
        printf("Device %s (%s), current: %s [%s]\n", s.address.c_str(), s.name.c_str(), s.inf.c_str(), s.service.c_str());
        for (auto& c : CompatibleDrivers(devs, did))
            printf("  rank 0x%08lX  %-36s %-40s %s\n", c.rank, c.inf.c_str(), c.desc.c_str(), c.provider.c_str());
        return true;
    });
    if (!found) fprintf(stderr, "no A2DP sink devnode for %s\n", addr.c_str());
    return found ? 0 : 1;
}

int CmdUse(const std::string& addr, const std::string& infName, bool dryRun, bool isRestore)
{
    int rc = 1;
    bool found = false;
    ForEachSink([&](HDEVINFO devs, SP_DEVINFO_DATA* did, const SinkDev& s) {
        if (!IEquals(s.address, addr)) return false;
        found = true;
        // An argument that names an existing file is used directly (DI_ENUMSINGLEINF); otherwise it is an
        // INF name already in the driver store (e.g. microsoft_bluetooth_a2dp_src.inf or oemNN.inf).
        std::wstring wInf = Utf8ToWide(infName);
        bool isPath = GetFileAttributesW(wInf.c_str()) != INVALID_FILE_ATTRIBUTES;
        wchar_t full[MAX_PATH] = L"";
        if (isPath) GetFullPathNameW(wInf.c_str(), MAX_PATH, full, nullptr);
        auto drivers = CompatibleDrivers(devs, did, isPath ? std::wstring(full) : std::wstring());
        const DriverChoice* pick = nullptr;
        for (auto& c : drivers)
            if (isPath ? IEquals(c.inf, FileName(infName)) : IEquals(c.inf, infName)) pick = &c;
        if (!pick) {
            fprintf(stderr, "driver '%s' is not a compatible driver for %s. Stage it first (pnputil /add-driver <inf>) and check 'drivers'.\n",
                    infName.c_str(), addr.c_str());
            return true;
        }
        printf("%s %s: %s [%s] -> %s (%s, %s)\n", dryRun ? "[dry-run]" : "Switching", s.address.c_str(), s.inf.c_str(),
               s.service.c_str(), pick->inf.c_str(), pick->desc.c_str(), pick->provider.c_str());
        if (dryRun) {
            rc = 0;
            return true;
        }
        if (!isRestore) {
            Json j = LoadJournal();
            if (!j.Has(s.address)) {  // keep the ORIGINAL driver if switched repeatedly
                Json e = Json::MakeObject();
                e["previousInf"] = s.inf;
                e["previousService"] = s.service;
                e["instanceId"] = WideToUtf8(s.instanceId);
                j[s.address] = e;
                if (!SaveJournal(j)) {
                    fprintf(stderr, "cannot write journal; aborting without changes\n");
                    return true;
                }
            }
        }
        if (!SetupDiSetSelectedDriverW(devs, did, const_cast<SP_DRVINFO_DATA_W*>(&pick->info))) {
            fprintf(stderr, "SetupDiSetSelectedDriver failed (%lu)\n", GetLastError());
            return true;
        }
        BOOL reboot = FALSE;
        SP_DRVINFO_DATA_W drv = pick->info;
        if (!DiInstallDevice(nullptr, devs, did, &drv, DIIDFLAG_NOFINISHINSTALLUI, &reboot)) {
            DWORD e = GetLastError();
            fprintf(stderr, "DiInstallDevice failed (0x%08lX)%s\n", e, e == ERROR_ACCESS_DENIED ? " - run elevated" : "");
            return true;
        }
        printf("Done.%s\n", reboot ? " A reboot is required to complete the change." : "");
        if (isRestore) {
            Json j = LoadJournal();
            Json::Object o = j.AsObject();
            o.erase(s.address);
            SaveJournal(Json(o));
        }
        rc = 0;
        return true;
    });
    if (!found) fprintf(stderr, "no A2DP sink devnode for %s\n", addr.c_str());
    return rc;
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: bta2dp-switch list | drivers <addr> | use <addr> <inf> [--dry-run] | restore <addr> [--dry-run]\n");
        return 2;
    }
    std::string cmd = argv[1];
    bool dry = false;
    for (int i = 2; i < argc; ++i)
        if (std::string(argv[i]) == "--dry-run") dry = true;
    auto addrArg = [&]() -> std::string {
        if (argc < 3) return {};
        auto a = BdAddr::Parse(argv[2]);
        return a ? a->ToString() : std::string{};
    };
    if (cmd == "list") return CmdList();
    if (cmd == "drivers") {
        std::string a = addrArg();
        if (a.empty()) return fprintf(stderr, "address required\n"), 2;
        return CmdDrivers(a);
    }
    if (cmd == "use") {
        std::string a = addrArg();
        if (a.empty() || argc < 4) return fprintf(stderr, "use <addr> <inf-name>\n"), 2;
        return CmdUse(a, argv[3], dry, false);
    }
    if (cmd == "restore") {
        std::string a = addrArg();
        if (a.empty()) return fprintf(stderr, "address required\n"), 2;
        Json j = LoadJournal();
        std::string inf = j.Get(a).Get("previousInf").AsString();
        if (inf.empty()) inf = kInboxInf;
        printf("Restoring %s to %s\n", a.c_str(), inf.c_str());
        return CmdUse(a, inf, dry, true);
    }
    fprintf(stderr, "unknown command\n");
    return 2;
}
