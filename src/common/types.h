// Shared value types used across modules and the IPC protocol.
#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace bta {

// Bluetooth device address, stored as the 48-bit integer Windows uses (BTH_ADDR).
struct BdAddr {
    uint64_t value = 0;

    bool operator==(const BdAddr&) const = default;
    bool IsZero() const { return value == 0; }
    std::string ToString() const;                       // "AA:BB:CC:DD:EE:FF"
    static std::optional<BdAddr> Parse(const std::string& s);  // accepts ':' '-' or bare hex
};

enum class CodecId : uint8_t {
    Sbc,
    Aac,
    Ldac,
    AptX,
    AptXHd,
    AptXLl,
    AptXAdaptive,
    OtherVendor,
    None,
};

const char* CodecName(CodecId c);
std::optional<CodecId> CodecFromName(const std::string& s);

// User-facing codec preference.
enum class CodecPreference : uint8_t { Auto, Ldac, Sbc };
const char* CodecPreferenceName(CodecPreference p);
std::optional<CodecPreference> CodecPreferenceFromName(const std::string& s);

// LDAC encode quality (maps to libldac EQMID; Abr uses the adaptive controller).
enum class LdacQuality : uint8_t { High = 0, Standard = 1, Mobile = 2, Adaptive = 3 };
const char* LdacQualityName(LdacQuality q);
std::optional<LdacQuality> LdacQualityFromName(const std::string& s);

std::wstring Utf8ToWide(const std::string& s);
std::string WideToUtf8(const std::wstring& s);

}  // namespace bta
