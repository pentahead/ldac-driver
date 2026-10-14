#include "platform/bt_discovery.h"

#include <winsock2.h>
#include <ws2bth.h>
#include <windows.h>
#include <bluetoothapis.h>
#include <cfgmgr32.h>
#include <initguid.h>
#include <devpkey.h>

#include <cwctype>
#include <format>

#include "common/log.h"

#pragma comment(lib, "bthprops.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "cfgmgr32.lib")

namespace bta {

namespace {

// {0000110B-0000-1000-8000-00805F9B34FB} AudioSink
constexpr GUID kAudioSinkUuid = {0x0000110B, 0x0000, 0x1000, {0x80, 0x00, 0x00, 0x80, 0x5F, 0x9B, 0x34, 0xFB}};

// ---- SDP data element parsing (Core spec Vol 3 Part B sec. 3) ----------------------------------
struct Elem {
    uint8_t type = 0;  // 0 nil,1 uint,2 int,3 uuid,4 string,5 bool,6 seq,7 alt,8 url
    std::span<const uint8_t> data;
};

std::optional<Elem> ReadElem(std::span<const uint8_t> in, size_t& off)
{
    if (off >= in.size()) return std::nullopt;
    uint8_t h = in[off++];
    Elem e;
    e.type = h >> 3;
    uint8_t si = h & 7;
    size_t len;
    if (e.type == 0) {
        len = 0;
    } else if (si <= 4) {
        len = size_t(1) << si;
    } else {
        size_t lb = si == 5 ? 1 : si == 6 ? 2 : 4;
        if (off + lb > in.size()) return std::nullopt;
        len = 0;
        for (size_t i = 0; i < lb; ++i) len = (len << 8) | in[off + i];
        off += lb;
    }
    if (off + len > in.size()) return std::nullopt;
    e.data = in.subspan(off, len);
    off += len;
    return e;
}

uint64_t UintOf(const Elem& e)
{
    uint64_t v = 0;
    for (uint8_t b : e.data) v = (v << 8) | b;
    return v;
}

// Returns the 16-bit form of a UUID element (16/32-bit, or 128-bit with Bluetooth base).
uint32_t Uuid16(const Elem& e)
{
    if (e.type != 3) return 0;
    if (e.data.size() == 2 || e.data.size() == 4) return static_cast<uint32_t>(UintOf(e));
    if (e.data.size() == 16) return (e.data[0] << 24) | (e.data[1] << 16) | (e.data[2] << 8) | e.data[3];
    return 0;
}

std::vector<Elem> Children(const Elem& seq)
{
    std::vector<Elem> out;
    size_t off = 0;
    while (off < seq.data.size()) {
        auto c = ReadElem(seq.data, off);
        if (!c) break;
        out.push_back(*c);
    }
    return out;
}

std::string ParentHexAddr(const std::wstring& instanceId)
{
    // ...\7&3916E4D8&B&81F17DCEB4E5_C00000000 -> 81F17DCEB4E5
    size_t us = instanceId.rfind(L'_');
    size_t amp = instanceId.rfind(L'&', us);
    if (us == std::wstring::npos || amp == std::wstring::npos || us - amp - 1 != 12) return {};
    return WideToUtf8(instanceId.substr(amp + 1, 12));
}

std::wstring GetStringProp(DEVINST dn, const DEVPROPKEY& key)
{
    DEVPROPTYPE type;
    wchar_t buf[512];
    ULONG len = sizeof buf;
    if (CM_Get_DevNode_PropertyW(dn, &key, &type, reinterpret_cast<PBYTE>(buf), &len, 0) != CR_SUCCESS ||
        type != DEVPROP_TYPE_STRING)
        return {};
    return buf;
}

}  // namespace

std::optional<A2dpSdpRecord> ParseA2dpSdpRecord(std::span<const uint8_t> record)
{
    size_t off = 0;
    auto top = ReadElem(record, off);
    if (!top || top->type != 6) return std::nullopt;
    auto items = Children(*top);
    A2dpSdpRecord r;
    for (size_t i = 0; i + 1 < items.size(); i += 2) {
        if (items[i].type != 1) return std::nullopt;
        uint16_t attr = static_cast<uint16_t>(UintOf(items[i]));
        const Elem& v = items[i + 1];
        switch (attr) {
        case 0x0001:  // ServiceClassIDList
            for (auto& c : Children(v))
                if (Uuid16(c) == 0x110B) r.found = true;
            break;
        case 0x0004:  // ProtocolDescriptorList
            for (auto& proto : Children(v)) {
                auto parts = Children(proto);
                if (parts.size() < 2) continue;
                uint32_t u = Uuid16(parts[0]);
                if (u == 0x0100) r.l2capPsm = static_cast<uint16_t>(UintOf(parts[1]));
                if (u == 0x0019) r.avdtpVersion = static_cast<uint16_t>(UintOf(parts[1]));
            }
            break;
        case 0x0009:  // BluetoothProfileDescriptorList
            for (auto& prof : Children(v)) {
                auto parts = Children(prof);
                if (parts.size() >= 2 && Uuid16(parts[0]) == 0x110D) r.a2dpVersion = static_cast<uint16_t>(UintOf(parts[1]));
            }
            break;
        case 0x0100:  // ServiceName (primary language base)
            if (v.type == 4) r.serviceName.assign(v.data.begin(), v.data.end());
            break;
        case 0x0311:
            r.supportedFeatures = static_cast<uint16_t>(UintOf(v));
            break;
        default: break;
        }
    }
    return r;
}

Json BtDeviceInfo::ToJson() const
{
    Json j = Json::MakeObject();
    j["address"] = address.ToString();
    j["name"] = name;
    j["classOfDevice"] = std::format("0x{:06X}", classOfDevice);
    j["audioClass"] = IsAudioClass();
    j["connected"] = connected;
    j["remembered"] = remembered;
    j["authenticated"] = authenticated;
    j["a2dpSink"] = a2dpSinkService;
    Json d = Json::MakeObject();
    d["present"] = driver.present;
    d["service"] = driver.service;
    d["instanceId"] = driver.instanceId;
    d["started"] = driver.started;
    d["problemCode"] = driver.problemCode;
    j["driver"] = d;
    return j;
}

Json A2dpSdpRecord::ToJson() const
{
    Json j = Json::MakeObject();
    j["found"] = found;
    j["l2capPsm"] = std::format("0x{:04X}", l2capPsm);
    j["avdtpVersion"] = std::format("{}.{}", avdtpVersion >> 8, avdtpVersion & 0xFF);
    j["a2dpVersion"] = std::format("{}.{}", a2dpVersion >> 8, a2dpVersion & 0xFF);
    j["supportedFeatures"] = std::format("0x{:04X}", supportedFeatures);
    j["serviceName"] = serviceName;
    return j;
}

bool BluetoothRadioPresent(std::string* name)
{
    BLUETOOTH_FIND_RADIO_PARAMS p{sizeof p};
    HANDLE radio = nullptr;
    HBLUETOOTH_RADIO_FIND f = BluetoothFindFirstRadio(&p, &radio);
    if (!f) return false;
    if (name) {
        BLUETOOTH_RADIO_INFO ri{sizeof ri};
        if (BluetoothGetRadioInfo(radio, &ri) == ERROR_SUCCESS) *name = WideToUtf8(ri.szName);
    }
    CloseHandle(radio);
    BluetoothFindRadioClose(f);
    return true;
}

A2dpSinkDriverBinding FindA2dpSinkDriver(BdAddr addr)
{
    A2dpSinkDriverBinding b;
    std::string want = std::format("{:012X}", addr.value);
    ULONG len = 0;
    const wchar_t* filter = L"BTHENUM";
    if (CM_Get_Device_ID_List_SizeW(&len, filter, CM_GETIDLIST_FILTER_ENUMERATOR | CM_GETIDLIST_FILTER_PRESENT) != CR_SUCCESS)
        return b;
    std::vector<wchar_t> buf(len);
    if (CM_Get_Device_ID_ListW(filter, buf.data(), len, CM_GETIDLIST_FILTER_ENUMERATOR | CM_GETIDLIST_FILTER_PRESENT) != CR_SUCCESS)
        return b;
    for (const wchar_t* p = buf.data(); *p; p += wcslen(p) + 1) {
        std::wstring id = p;
        std::wstring upper = id;
        for (auto& c : upper) c = static_cast<wchar_t>(towupper(c));
        if (upper.rfind(L"BTHENUM\\{0000110B-0000-1000-8000-00805F9B34FB}", 0) != 0) continue;
        if (ParentHexAddr(upper) != want) continue;
        DEVINST dn;
        if (CM_Locate_DevNodeW(&dn, const_cast<wchar_t*>(id.c_str()), CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) continue;
        b.present = true;
        b.instanceId = WideToUtf8(id);
        b.service = WideToUtf8(GetStringProp(dn, DEVPKEY_Device_Service));
        ULONG status = 0, problem = 0;
        if (CM_Get_DevNode_Status(&status, &problem, dn, 0) == CR_SUCCESS) {
            b.started = (status & DN_STARTED) != 0;
            b.problemCode = (status & DN_HAS_PROBLEM) ? problem : 0;
        }
        break;
    }
    return b;
}

Result<std::vector<BtDeviceInfo>> EnumeratePairedDevices()
{
    std::vector<BtDeviceInfo> out;
    BLUETOOTH_DEVICE_SEARCH_PARAMS sp{sizeof sp};
    sp.fReturnAuthenticated = TRUE;
    sp.fReturnRemembered = TRUE;
    sp.fReturnConnected = TRUE;
    sp.fReturnUnknown = FALSE;
    sp.fIssueInquiry = FALSE;  // paired/known devices only; no radio inquiry
    BLUETOOTH_DEVICE_INFO di{sizeof di};
    HBLUETOOTH_DEVICE_FIND f = BluetoothFindFirstDevice(&sp, &di);
    if (!f) {
        DWORD e = GetLastError();
        if (e == ERROR_NO_MORE_ITEMS) return out;
        return Status{Err::NotAvailable, std::format("BluetoothFindFirstDevice failed ({}); is a Bluetooth radio present?", e)};
    }
    do {
        BtDeviceInfo d;
        d.address = BdAddr{di.Address.ullLong};
        d.name = WideToUtf8(di.szName);
        d.classOfDevice = di.ulClassofDevice;
        d.connected = di.fConnected != FALSE;
        d.remembered = di.fRemembered != FALSE;
        d.authenticated = di.fAuthenticated != FALSE;
        GUID guids[64];
        DWORD n = 64;
        if (BluetoothEnumerateInstalledServices(nullptr, &di, &n, guids) == ERROR_SUCCESS)
            for (DWORD i = 0; i < n; ++i)
                if (IsEqualGUID(guids[i], kAudioSinkUuid)) d.a2dpSinkService = true;
        d.driver = FindA2dpSinkDriver(d.address);
        if (d.driver.present) d.a2dpSinkService = true;
        out.push_back(std::move(d));
        di = BLUETOOTH_DEVICE_INFO{sizeof di};
    } while (BluetoothFindNextDevice(f, &di));
    BluetoothFindDeviceClose(f);
    return out;
}

Result<A2dpSdpRecord> QueryA2dpSinkSdp(BdAddr addr, bool flushCache)
{
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return Status{Err::Internal, "WSAStartup failed"};
    struct WsaCleanup {
        ~WsaCleanup() { WSACleanup(); }
    } cleanup;

    std::wstring ctx = Utf8ToWide("(" + addr.ToString() + ")");
    WSAQUERYSETW qs{};
    qs.dwSize = sizeof qs;
    qs.dwNameSpace = NS_BTH;
    GUID svc = kAudioSinkUuid;
    qs.lpServiceClassId = &svc;
    qs.lpszContext = ctx.data();
    DWORD flags = LUP_RETURN_BLOB | (flushCache ? LUP_FLUSHCACHE : 0);
    HANDLE h = nullptr;
    if (WSALookupServiceBeginW(&qs, flags, &h) != 0) {
        int e = WSAGetLastError();
        return Status{e == WSASERVICE_NOT_FOUND ? Err::NotAvailable : Err::IoError,
                      std::format("SDP lookup failed (WSA error {}); device out of range or not paired?", e)};
    }
    std::vector<uint8_t> buf(8192);
    DWORD len = static_cast<DWORD>(buf.size());
    A2dpSdpRecord rec;
    if (WSALookupServiceNextW(h, flags, &len, reinterpret_cast<WSAQUERYSETW*>(buf.data())) == 0) {
        auto* res = reinterpret_cast<WSAQUERYSETW*>(buf.data());
        if (res->lpBlob && res->lpBlob->cbSize) {
            auto parsed = ParseA2dpSdpRecord(std::span(res->lpBlob->pBlobData, res->lpBlob->cbSize));
            if (parsed) rec = *parsed;
        }
    } else {
        int e = WSAGetLastError();
        WSALookupServiceEnd(h);
        if (e == WSA_E_NO_MORE || e == WSAENOMORE) return Status{Err::NotAvailable, "device has no A2DP sink SDP record"};
        return Status{Err::IoError, std::format("WSALookupServiceNext failed ({})", e)};
    }
    WSALookupServiceEnd(h);
    if (!rec.found) return Status{Err::NotAvailable, "SDP record did not contain AudioSink"};
    return rec;
}

}  // namespace bta
