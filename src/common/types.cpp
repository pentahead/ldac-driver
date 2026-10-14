#include "common/types.h"

#include <windows.h>

#include <cctype>
#include <cstdio>
#include <cstring>

namespace bta {

std::string BdAddr::ToString() const
{
    char buf[24];
    snprintf(buf, sizeof buf, "%02X:%02X:%02X:%02X:%02X:%02X", static_cast<unsigned>((value >> 40) & 0xFF),
             static_cast<unsigned>((value >> 32) & 0xFF), static_cast<unsigned>((value >> 24) & 0xFF),
             static_cast<unsigned>((value >> 16) & 0xFF), static_cast<unsigned>((value >> 8) & 0xFF),
             static_cast<unsigned>(value & 0xFF));
    return buf;
}

std::optional<BdAddr> BdAddr::Parse(const std::string& s)
{
    uint64_t v = 0;
    int digits = 0;
    for (char c : s) {
        if (c == ':' || c == '-') continue;
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return std::nullopt;
        v = (v << 4) | static_cast<uint64_t>(d);
        if (++digits > 12) return std::nullopt;
    }
    if (digits != 12) return std::nullopt;
    return BdAddr{v};
}

const char* CodecName(CodecId c)
{
    switch (c) {
    case CodecId::Sbc: return "SBC";
    case CodecId::Aac: return "AAC";
    case CodecId::Ldac: return "LDAC";
    case CodecId::AptX: return "aptX";
    case CodecId::AptXHd: return "aptX HD";
    case CodecId::AptXLl: return "aptX LL";
    case CodecId::AptXAdaptive: return "aptX Adaptive";
    case CodecId::OtherVendor: return "Vendor";
    case CodecId::None: return "None";
    }
    return "?";
}

static bool IEq(const std::string& a, const char* b)
{
    size_t n = strlen(b);
    if (a.size() != n) return false;
    for (size_t i = 0; i < n; ++i)
        if (tolower(static_cast<unsigned char>(a[i])) != tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}

std::optional<CodecId> CodecFromName(const std::string& s)
{
    for (int i = 0; i <= static_cast<int>(CodecId::None); ++i)
        if (IEq(s, CodecName(static_cast<CodecId>(i)))) return static_cast<CodecId>(i);
    return std::nullopt;
}

const char* CodecPreferenceName(CodecPreference p)
{
    switch (p) {
    case CodecPreference::Auto: return "auto";
    case CodecPreference::Ldac: return "ldac";
    case CodecPreference::Sbc: return "sbc";
    }
    return "auto";
}

std::optional<CodecPreference> CodecPreferenceFromName(const std::string& s)
{
    if (IEq(s, "auto")) return CodecPreference::Auto;
    if (IEq(s, "ldac")) return CodecPreference::Ldac;
    if (IEq(s, "sbc")) return CodecPreference::Sbc;
    return std::nullopt;
}

const char* LdacQualityName(LdacQuality q)
{
    switch (q) {
    case LdacQuality::High: return "hq";
    case LdacQuality::Standard: return "sq";
    case LdacQuality::Mobile: return "mq";
    case LdacQuality::Adaptive: return "abr";
    }
    return "abr";
}

std::optional<LdacQuality> LdacQualityFromName(const std::string& s)
{
    if (IEq(s, "hq")) return LdacQuality::High;
    if (IEq(s, "sq")) return LdacQuality::Standard;
    if (IEq(s, "mq")) return LdacQuality::Mobile;
    if (IEq(s, "abr")) return LdacQuality::Adaptive;
    return std::nullopt;
}

std::wstring Utf8ToWide(const std::string& s)
{
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string WideToUtf8(const std::wstring& w)
{
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

}  // namespace bta
