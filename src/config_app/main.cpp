// bta2dp-config.exe - configuration UI for the BtA2dpSrc service (Win32, no extra dependencies).
// Everything shown comes from the running service; the active codec is displayed only when the
// service reports it as confirmed (packets accepted on the media channel).
#include <windows.h>
#include <commctrl.h>
#include <windowsx.h>

#include <format>
#include <string>
#include <vector>

#include "common/json.h"
#include "common/types.h"
#include "platform/ipc.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

using namespace bta;

namespace {

enum Id : int {
    IDC_SVC = 100, IDC_REFRESH, IDC_DEVICES, IDC_CONNECT, IDC_DISCONNECT, IDC_PROBE,
    IDC_CODEC, IDC_LDACQ, IDC_RATE, IDC_BITS, IDC_BITPOOL, IDC_SENDAHEAD, IDC_SOURCE, IDC_ENDPOINT, IDC_APPLY,
    IDC_STATUS, IDC_LOG, IDC_TIMER = 1
};

HWND g_wnd, g_svc, g_devices, g_codec, g_ldacq, g_rate, g_bits, g_bitpool, g_sendAhead, g_source, g_endpoint, g_status, g_log;
HFONT g_font;
UINT g_dpi = 96;
int S(int v) { return MulDiv(v, static_cast<int>(g_dpi), 96); }  // layout is authored at 96 DPI
uint64_t g_logSeq = 0;
bool g_ldacEncoder = false;
std::vector<std::string> g_endpointIds;
std::string g_lastSinkCodecs;

std::wstring W(const std::string& s) { return Utf8ToWide(s); }

Json Cmd(const char* c)
{
    Json j = Json::MakeObject();
    j["cmd"] = c;
    return j;
}

Result<Json> Call(const Json& req, uint32_t timeout = 1500)
{
    auto r = PipeRequest(req, timeout);
    if (r && !r->Get("ok").AsBool()) return Status{Err::Rejected, r->Get("error").AsString()};
    return r;
}

HWND Make(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id, DWORD ex = 0)
{
    HWND hw = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h), g_wnd,
                              reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    SendMessageW(hw, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
    return hw;
}

void Label(const wchar_t* t, int x, int y, int w) { Make(L"STATIC", t, 0, x, y + 3, w, 20, 0); }

HWND Combo(int x, int y, int w, int id, std::initializer_list<const wchar_t*> items)
{
    HWND c = Make(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, x, y, w, 300, id);
    for (auto* i : items) ComboBox_AddString(c, i);
    ComboBox_SetCurSel(c, 0);
    return c;
}

void SetText(HWND h, const std::string& s) { SetWindowTextW(h, W(s).c_str()); }

std::string GetText(HWND h)
{
    wchar_t b[256];
    GetWindowTextW(h, b, 256);
    return WideToUtf8(b);
}

void AppendLog(const std::string& line)
{
    std::wstring w = W(line) + L"\r\n";
    int len = GetWindowTextLengthW(g_log);
    if (len > 60000) {
        Edit_SetSel(g_log, 0, 20000);
        Edit_ReplaceSel(g_log, L"");
        len = GetWindowTextLengthW(g_log);
    }
    Edit_SetSel(g_log, len, len);
    Edit_ReplaceSel(g_log, w.c_str());
}

void Error(const std::string& what, const Status& st)
{
    AppendLog("[ui] " + what + ": " + st.ToString());
    MessageBoxW(g_wnd, W(what + "\n\n" + st.message()).c_str(), L"BtA2dpSrc", MB_ICONWARNING);
}

std::string SelectedAddress()
{
    int i = ListView_GetNextItem(g_devices, -1, LVNI_SELECTED);
    if (i < 0) return {};
    wchar_t b[64];
    ListView_GetItemText(g_devices, i, 1, b, 64);
    return WideToUtf8(b);
}

void RefreshDevices()
{
    auto r = Call(Cmd("devices"), 8000);
    ListView_DeleteAllItems(g_devices);
    if (!r) return Error("Device discovery failed", r.status());
    int row = 0;
    for (auto& d : r->Get("devices").AsArray()) {
        if (!d.Get("a2dpSink").AsBool()) continue;
        std::wstring name = W(d.Get("name").AsString());
        LVITEMW it{LVIF_TEXT, row, 0};
        it.pszText = name.data();
        ListView_InsertItem(g_devices, &it);
        std::wstring addr = W(d.Get("address").AsString());
        std::wstring conn = d.Get("connected").AsBool() ? L"yes" : L"no";
        std::wstring drv = W(d.Get("driver").Get("present").AsBool() ? d.Get("driver").Get("service").AsString() : "(none)");
        std::wstring ready = d.Get("transportReady").AsBool() ? L"ready" : L"needs BtA2dpL2cap";
        ListView_SetItemText(g_devices, row, 1, addr.data());
        ListView_SetItemText(g_devices, row, 2, conn.data());
        ListView_SetItemText(g_devices, row, 3, drv.data());
        ListView_SetItemText(g_devices, row, 4, ready.data());
        ++row;
    }
    if (r->Has("warning")) AppendLog("[devices] " + r->Get("warning").AsString());
}

void SelectCombo(HWND c, const std::wstring& text)
{
    int i = ComboBox_FindStringExact(c, -1, text.c_str());
    ComboBox_SetCurSel(c, i >= 0 ? i : 0);
}

void LoadSettings()
{
    auto info = Call(Cmd("info"));
    if (info) {
        g_ldacEncoder = info->Get("encoders").Get("LDAC").AsBool();
        if (!g_ldacEncoder) AppendLog("[ui] LDAC encoder unavailable in this service build; LDAC option disabled");
    }
    auto eps = Call(Cmd("endpoints"));
    ComboBox_ResetContent(g_endpoint);
    g_endpointIds = {""};
    ComboBox_AddString(g_endpoint, L"(default playback device)");
    if (eps)
        for (auto& e : eps->Get("endpoints").AsArray()) {
            g_endpointIds.push_back(e.Get("id").AsString());
            ComboBox_AddString(g_endpoint, W(e.Get("name").AsString()).c_str());
        }
    auto r = Call(Cmd("get_settings"));
    if (!r) return;
    Json s = r->Get("settings");
    std::string codec = s.Get("codec").AsString();
    SelectCombo(g_codec, codec == "ldac" ? L"LDAC" : codec == "sbc" ? L"SBC" : L"Auto (LDAC if available)");
    std::string q = s.Get("ldacQuality").AsString();
    SelectCombo(g_ldacq, q == "hq" ? L"HQ (990 kbps)" : q == "sq" ? L"SQ (660 kbps)" : q == "mq" ? L"MQ (330 kbps)" : L"Adaptive (ABR)");
    int64_t rate = s.Get("sampleRate").AsInt();
    SelectCombo(g_rate, rate ? std::to_wstring(rate) : L"Auto");
    int64_t bits = s.Get("bitsPerSample").AsInt();
    SelectCombo(g_bits, bits ? std::to_wstring(bits) : L"Auto");
    SetWindowTextW(g_bitpool, std::to_wstring(s.Get("sbc").Get("maxBitpool").AsInt(53)).c_str());
    SetWindowTextW(g_sendAhead, std::to_wstring(s.Get("sendAheadMs").AsInt(40)).c_str());
    SelectCombo(g_source, W(s.Get("source").AsString()));
    std::string ep = s.Get("sourceEndpointId").AsString();
    for (size_t i = 0; i < g_endpointIds.size(); ++i)
        if (g_endpointIds[i] == ep) ComboBox_SetCurSel(g_endpoint, static_cast<int>(i));
}

void Apply()
{
    Json s = Json::MakeObject();
    int c = ComboBox_GetCurSel(g_codec);
    if (c == 1 && !g_ldacEncoder) return Error("LDAC cannot be selected", {Err::NotAvailable, "the service has no LDAC encoder"});
    s["codec"] = c == 1 ? "ldac" : c == 2 ? "sbc" : "auto";
    static const char* kQ[] = {"abr", "hq", "sq", "mq"};
    s["ldacQuality"] = kQ[std::max(0, ComboBox_GetCurSel(g_ldacq))];
    std::string rate = GetText(g_rate);
    s["sampleRate"] = rate == "Auto" ? 0 : std::stoi(rate);
    std::string bits = GetText(g_bits);
    s["bitsPerSample"] = bits == "Auto" ? 0 : std::stoi(bits);
    try {
        s["sbc"]["maxBitpool"] = std::stoi(GetText(g_bitpool));
        s["sendAheadMs"] = std::stoi(GetText(g_sendAhead));
    } catch (...) {
        return Error("Invalid number", {Err::InvalidArgument, "bitpool and send-ahead must be integers"});
    }
    s["source"] = GetText(g_source);
    int ep = ComboBox_GetCurSel(g_endpoint);
    s["sourceEndpointId"] = (ep > 0 && ep < static_cast<int>(g_endpointIds.size())) ? g_endpointIds[ep] : "";
    std::string addr = SelectedAddress();
    if (!addr.empty()) s["deviceAddress"] = addr;
    Json req = Cmd("set_settings");
    req["settings"] = s;
    auto r = Call(req, 3000);
    if (!r) return Error("Settings rejected", r.status());
    AppendLog("[ui] settings applied and saved");
    for (auto& w : r->Get("warnings").AsArray()) AppendLog("[ui] warning: " + w.AsString());
}

void Poll()
{
    auto st = Call(Cmd("status"), 800);
    if (!st) {
        SetText(g_svc, "Service: not reachable - " + st.status().message() +
                           "   (start with: sc start BtA2dpSrc, or bta2dp-service --console)");
        SetText(g_status, "No status (service not running).");
        return;
    }
    Json s = st->Get("status");
    SetText(g_svc, std::string("Service: running") + (st->Get("simulate").AsBool() ? "  [SIMULATION MODE - no Bluetooth hardware]" : ""));
    Json e = s.Get("engine");
    std::string sinkCodecs;
    for (auto& c : s.Get("sinkCodecs").AsArray())
        sinkCodecs += std::format("    SEP {}: {}{}\r\n", c.Get("seid").AsInt(), c.Get("description").AsString(),
                                  c.Get("usable").AsBool() ? "" : "  (not usable)");
    if (sinkCodecs.empty()) sinkCodecs = "    (known after connecting)\r\n";
    std::string active = s.Get("codecConfirmed").AsBool() ? s.Get("activeCodec").AsString() : "- (not confirmed)";
    std::string text = std::format(
        "State:            {}\r\nDevice:           {}\r\nTransport:        {}\r\nActive codec:     {}\r\n"
        "Configuration:    {}\r\nQuality:          {}\r\nBitrate:          {:.0f} kbps measured / {:.0f} kbps nominal\r\n"
        "Sink delay:       {:.1f} ms     Sink buffer: {:.0f} ms\r\nPackets:          {}   underruns: {}   silence frames: {}   send errors: {}\r\n"
        "Encode time:      avg {:.0f} us  max {:.0f} us\r\nFallback reason:  {}\r\nLast error:       {}\r\n{}Sink codecs:\r\n{}",
        s.Get("state").AsString(), s.Get("device").AsString(), s.Get("transport").AsString(), active,
        s.Get("configuration").AsString(), e.Get("quality").AsString(), e.Get("measuredKbps").AsNumber(),
        e.Get("nominalKbps").AsNumber(), s.Get("sinkDelayMs").AsNumber(), e.Get("sinkBufferMs").AsNumber(),
        e.Get("packets").AsInt(), e.Get("underrunEvents").AsInt(), e.Get("silenceFramesInserted").AsInt(),
        e.Get("sendErrors").AsInt(), e.Get("encodeAvgUs").AsNumber(), e.Get("encodeMaxUs").AsNumber(),
        s.Get("fallbackReason").AsString().empty() ? "-" : s.Get("fallbackReason").AsString(),
        s.Get("lastError").AsString().empty() ? "-" : s.Get("lastError").AsString(),
        s.Get("restartRequired").AsBool() ? "NOTE: reconnect to apply the changed settings\r\n" : "", sinkCodecs);
    SetText(g_status, text);

    Json lr = Cmd("logs");
    lr["after"] = g_logSeq;
    if (auto logs = Call(lr, 800))
        for (auto& l : logs->Get("entries").AsArray()) {
            g_logSeq = static_cast<uint64_t>(l.Get("seq").AsInt());
            AppendLog(std::format("{:<5} [{}] {}", l.Get("level").AsString(), l.Get("component").AsString(), l.Get("message").AsString()));
        }
}

LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        g_wnd = h;
        NONCLIENTMETRICSW m{sizeof m};
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof m, &m, 0);
        g_font = CreateFontIndirectW(&m.lfMessageFont);
        g_svc = Make(L"STATIC", L"Service: ...", 0, 12, 10, 760, 20, IDC_SVC);
        Make(L"BUTTON", L"Refresh", BS_PUSHBUTTON | WS_TABSTOP, 790, 6, 90, 26, IDC_REFRESH);

        g_devices = Make(WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_BORDER | WS_TABSTOP, 12, 40, 520, 200, IDC_DEVICES);
        ListView_SetExtendedListViewStyle(g_devices, LVS_EX_FULLROWSELECT);
        const wchar_t* cols[] = {L"Name", L"Address", L"Connected", L"A2DP driver", L"Transport"};
        int widths[] = {150, 120, 70, 80, 95};
        for (int i = 0; i < 5; ++i) {
            LVCOLUMNW c{LVCF_TEXT | LVCF_WIDTH, 0, S(widths[i]), const_cast<wchar_t*>(cols[i])};
            ListView_InsertColumn(g_devices, i, &c);
        }
        Make(L"BUTTON", L"Connect", BS_PUSHBUTTON | WS_TABSTOP, 12, 246, 100, 26, IDC_CONNECT);
        Make(L"BUTTON", L"Disconnect", BS_PUSHBUTTON | WS_TABSTOP, 118, 246, 100, 26, IDC_DISCONNECT);
        Make(L"BUTTON", L"Probe SDP", BS_PUSHBUTTON | WS_TABSTOP, 224, 246, 100, 26, IDC_PROBE);

        int x = 550, lw = 120, cx = 675, cw = 205, y = 40;
        Make(L"BUTTON", L"Codec settings", BS_GROUPBOX, x - 8, y - 8, 346, 270, 0);
        Label(L"Codec", x, y + 12, lw);
        g_codec = Combo(cx, y + 12, cw, IDC_CODEC, {L"Auto (LDAC if available)", L"LDAC", L"SBC"});
        Label(L"LDAC quality", x, y + 42, lw);
        g_ldacq = Combo(cx, y + 42, cw, IDC_LDACQ, {L"Adaptive (ABR)", L"HQ (990 kbps)", L"SQ (660 kbps)", L"MQ (330 kbps)"});
        Label(L"Sample rate", x, y + 72, lw);
        g_rate = Combo(cx, y + 72, cw, IDC_RATE, {L"Auto", L"44100", L"48000", L"88200", L"96000"});
        Label(L"Bit depth (LDAC)", x, y + 102, lw);
        g_bits = Combo(cx, y + 102, cw, IDC_BITS, {L"Auto", L"16", L"24"});
        Label(L"SBC max bitpool", x, y + 132, lw);
        g_bitpool = Make(L"EDIT", L"53", ES_NUMBER | WS_BORDER | WS_TABSTOP, cx, y + 132, 60, 22, IDC_BITPOOL);
        Label(L"Send-ahead (ms)", x, y + 160, lw);
        g_sendAhead = Make(L"EDIT", L"40", ES_NUMBER | WS_BORDER | WS_TABSTOP, cx, y + 160, 60, 22, IDC_SENDAHEAD);
        Label(L"Audio source", x, y + 188, lw);
        g_source = Combo(cx, y + 188, cw, IDC_SOURCE, {L"loopback", L"tone"});
        Label(L"Capture from", x, y + 216, lw);
        g_endpoint = Combo(cx, y + 216, cw, IDC_ENDPOINT, {L"(default playback device)"});
        Make(L"BUTTON", L"Apply && Save", BS_DEFPUSHBUTTON | WS_TABSTOP, 780, 246 + 30, 100, 26, IDC_APPLY);

        g_status = Make(L"EDIT", L"", ES_MULTILINE | ES_READONLY | WS_BORDER | WS_VSCROLL, 12, 312, 868, 230, IDC_STATUS);
        g_log = Make(L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_BORDER | WS_VSCROLL, 12, 550, 868, 150, IDC_LOG);
        HFONT mono = CreateFontW(-S(13), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");
        SendMessageW(g_status, WM_SETFONT, reinterpret_cast<WPARAM>(mono), TRUE);
        SendMessageW(g_log, WM_SETFONT, reinterpret_cast<WPARAM>(mono), TRUE);

        LoadSettings();
        RefreshDevices();
        Poll();
        SetTimer(h, IDC_TIMER, 1000, nullptr);
        return 0;
    }
    case WM_TIMER: Poll(); return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_REFRESH:
            LoadSettings();
            RefreshDevices();
            return 0;
        case IDC_APPLY: Apply(); return 0;
        case IDC_CONNECT: {
            Json r = Cmd("connect");
            std::string a = SelectedAddress();
            if (!a.empty()) r["address"] = a;
            auto res = Call(r);
            if (!res) Error("Connect failed", res.status());
            else AppendLog("[ui] connecting to " + res->Get("address").AsString());
            return 0;
        }
        case IDC_DISCONNECT:
            if (auto res = Call(Cmd("disconnect")); !res) Error("Disconnect failed", res.status());
            return 0;
        case IDC_PROBE: {
            std::string a = SelectedAddress();
            if (a.empty()) return MessageBoxW(h, L"Select a device first.", L"BtA2dpSrc", MB_ICONINFORMATION), 0;
            Json r = Cmd("probe");
            r["address"] = a;
            AppendLog("[ui] SDP query " + a + " (device must be in range)...");
            auto res = Call(r, 20000);
            if (!res) Error("SDP probe failed", res.status());
            else AppendLog("[sdp] " + res->Get("sdp").Dump());
            return 0;
        }
        }
        break;
    case WM_DESTROY:
        KillTimer(h, IDC_TIMER);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_SYSTEM_AWARE);
    g_dpi = GetDpiForSystem();
    INITCOMMONCONTROLSEX icc{sizeof icc, ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"BtA2dpSrcConfig";
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    RegisterClassW(&wc);
    HWND w = CreateWindowW(wc.lpszClassName, L"BtA2dpSrc - Bluetooth LDAC/SBC configuration",
                           WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, CW_USEDEFAULT, CW_USEDEFAULT, S(908), S(750),
                           nullptr, nullptr, inst, nullptr);
    ShowWindow(w, show);
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0)) {
        if (!IsDialogMessageW(w, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    return 0;
}
