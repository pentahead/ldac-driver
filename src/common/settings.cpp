#include "common/settings.h"

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <fstream>
#include <sstream>

namespace bta {

Json Settings::ToJson() const
{
    Json j = Json::MakeObject();
    j["deviceAddress"] = deviceAddress;
    j["codec"] = CodecPreferenceName(codec);
    j["ldacQuality"] = LdacQualityName(ldacQuality);
    j["sampleRate"] = sampleRate;
    j["bitsPerSample"] = static_cast<int>(bitsPerSample);
    Json sb = Json::MakeObject();
    sb["maxBitpool"] = static_cast<int>(sbc.maxBitpool);
    sb["blocks"] = static_cast<int>(sbc.blocks);
    sb["subbands"] = static_cast<int>(sbc.subbands);
    sb["allocation"] = sbc.loudness ? "loudness" : "snr";
    j["sbc"] = sb;
    j["sendAheadMs"] = sendAheadMs;
    j["source"] = source;
    j["sourceEndpointId"] = sourceEndpointId;
    j["logLevel"] = logLevel;
    j["schemaVersion"] = 1;
    return j;
}

void Settings::Clamp()
{
    sbc.maxBitpool = std::clamp<uint8_t>(sbc.maxBitpool, 2, 250);
    if (sbc.blocks != 4 && sbc.blocks != 8 && sbc.blocks != 12 && sbc.blocks != 16) sbc.blocks = 16;
    if (sbc.subbands != 4 && sbc.subbands != 8) sbc.subbands = 8;
    sendAheadMs = std::clamp<uint32_t>(sendAheadMs, 20, 300);
    if (bitsPerSample != 0 && bitsPerSample != 16 && bitsPerSample != 24) bitsPerSample = 0;
}

Status Settings::MergeFrom(const Json& j, std::vector<std::string>* warnings)
{
    if (!j.IsObject()) return {Err::InvalidArgument, "settings must be a JSON object"};
    Settings next = *this;
    auto warn = [&](const std::string& w) {
        if (warnings) warnings->push_back(w);
    };

    if (auto* v = j.Find("deviceAddress")) {
        std::string a = v->AsString();
        if (!a.empty() && !BdAddr::Parse(a)) return {Err::InvalidArgument, "deviceAddress is not a BD_ADDR"};
        next.deviceAddress = a.empty() ? "" : BdAddr::Parse(a)->ToString();
    }
    if (auto* v = j.Find("codec")) {
        auto c = CodecPreferenceFromName(v->AsString());
        if (!c) return {Err::InvalidArgument, "codec must be auto|ldac|sbc"};
        next.codec = *c;
    }
    if (auto* v = j.Find("ldacQuality")) {
        auto q = LdacQualityFromName(v->AsString());
        if (!q) return {Err::InvalidArgument, "ldacQuality must be hq|sq|mq|abr"};
        next.ldacQuality = *q;
    }
    if (auto* v = j.Find("sampleRate")) {
        int64_t r = v->AsInt(-1);
        if (r != 0 && r != 44100 && r != 48000 && r != 88200 && r != 96000 && r != 32000 && r != 16000)
            return {Err::InvalidArgument, "sampleRate must be 0 (auto) or 16000/32000/44100/48000/88200/96000"};
        next.sampleRate = static_cast<uint32_t>(r);
    }
    if (auto* v = j.Find("bitsPerSample")) {
        int64_t b = v->AsInt(-1);
        if (b != 0 && b != 16 && b != 24) return {Err::InvalidArgument, "bitsPerSample must be 0, 16 or 24"};
        next.bitsPerSample = static_cast<uint8_t>(b);
    }
    if (auto* sb = j.Find("sbc")) {
        if (auto* v = sb->Find("maxBitpool")) {
            int64_t bp = v->AsInt(-1);
            if (bp < 2 || bp > 250) return {Err::InvalidArgument, "sbc.maxBitpool must be 2..250"};
            next.sbc.maxBitpool = static_cast<uint8_t>(bp);
        }
        if (auto* v = sb->Find("blocks")) {
            int64_t b = v->AsInt(-1);
            if (b != 4 && b != 8 && b != 12 && b != 16) return {Err::InvalidArgument, "sbc.blocks must be 4/8/12/16"};
            next.sbc.blocks = static_cast<uint8_t>(b);
        }
        if (auto* v = sb->Find("subbands")) {
            int64_t s = v->AsInt(-1);
            if (s != 4 && s != 8) return {Err::InvalidArgument, "sbc.subbands must be 4 or 8"};
            next.sbc.subbands = static_cast<uint8_t>(s);
        }
        if (auto* v = sb->Find("allocation")) {
            std::string a = v->AsString();
            if (a != "loudness" && a != "snr") return {Err::InvalidArgument, "sbc.allocation must be loudness|snr"};
            next.sbc.loudness = (a == "loudness");
        }
    }
    if (auto* v = j.Find("sendAheadMs")) {
        int64_t ms = v->AsInt(-1);
        if (ms < 20 || ms > 300) return {Err::InvalidArgument, "sendAheadMs must be 20..300"};
        next.sendAheadMs = static_cast<uint32_t>(ms);
    }
    if (auto* v = j.Find("source")) {
        std::string s = v->AsString();
        if (s != "loopback" && s != "tone" && s != "endpoint")
            return {Err::InvalidArgument, "source must be loopback|tone|endpoint"};
        next.source = s;
    }
    if (auto* v = j.Find("sourceEndpointId")) next.sourceEndpointId = v->AsString();
    if (auto* v = j.Find("logLevel")) {
        std::string l = v->AsString();
        if (l != "trace" && l != "debug" && l != "info" && l != "warn" && l != "error")
            warn("ignored invalid logLevel '" + l + "'");
        else
            next.logLevel = l;
    }
    next.Clamp();
    *this = next;
    return Status::Ok();
}

std::wstring DefaultSettingsPath()
{
    PWSTR pd = nullptr;
    std::wstring base = L"C:\\ProgramData";
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &pd))) base = pd;
    CoTaskMemFree(pd);
    return base + L"\\BtA2dpSrc\\settings.json";
}

Status LoadSettings(const std::wstring& path, Settings& out)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return {Err::NotAvailable, "settings file not found"};
    std::stringstream ss;
    ss << f.rdbuf();
    std::string err;
    auto j = Json::Parse(ss.str(), &err);
    if (!j) return {Err::ProtocolError, "settings parse error: " + err};
    Settings s;
    Status st = s.MergeFrom(*j);
    if (!st) return st;
    out = s;
    return Status::Ok();
}

Status SaveSettings(const std::wstring& path, const Settings& s)
{
    std::wstring dir = path.substr(0, path.find_last_of(L"\\/"));
    if (!dir.empty()) SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    std::wstring tmp = path + L".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return {Err::IoError, "cannot write " + WideToUtf8(tmp)};
        std::string text = s.ToJson().Dump(true);
        f.write(text.data(), static_cast<std::streamsize>(text.size()));
        f.flush();
        if (!f) return {Err::IoError, "write failed"};
    }
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return {Err::IoError, "atomic replace failed, error " + std::to_string(GetLastError())};
    return Status::Ok();
}

}  // namespace bta
