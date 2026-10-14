// Persistent user settings. Stored as JSON, validated and clamped on every load/update,
// written atomically (temp file + MoveFileEx) so a crash never leaves a torn file.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "common/json.h"
#include "common/status.h"
#include "common/types.h"

namespace bta {

struct SbcSettings {
    uint8_t maxBitpool = 53;   // 2..250; standard "high quality" is 53 at 44.1/48 kHz joint stereo
    uint8_t blocks = 16;       // 4, 8, 12, 16
    uint8_t subbands = 8;      // 4, 8
    bool loudness = true;      // allocation method: loudness (true) or SNR (false)
};

struct Settings {
    std::string deviceAddress;                  // last selected device ("" = none)
    CodecPreference codec = CodecPreference::Auto;
    LdacQuality ldacQuality = LdacQuality::Adaptive;
    uint32_t sampleRate = 0;                    // 0 = auto (source rate if sink supports it)
    uint8_t bitsPerSample = 0;                  // 0 = auto, 16, 24 (LDAC only; SBC is always 16)
    SbcSettings sbc;
    uint32_t sendAheadMs = 40;                  // media buffered ahead in the sink (20..300)
    std::string source = "loopback";           // "loopback" (WASAPI) | "tone" | "endpoint" (driver ring)
    std::string sourceEndpointId;               // WASAPI endpoint id ("" = default render device)
    std::string logLevel = "info";

    Json ToJson() const;
    // Applies known keys from j onto *this; unknown keys are ignored, invalid values rejected.
    Status MergeFrom(const Json& j, std::vector<std::string>* warnings = nullptr);
    void Clamp();
};

std::wstring DefaultSettingsPath();  // %ProgramData%\BtA2dpSrc\settings.json
Status LoadSettings(const std::wstring& path, Settings& out);
Status SaveSettings(const std::wstring& path, const Settings& s);

}  // namespace bta
