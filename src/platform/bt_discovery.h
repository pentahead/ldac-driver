// Device discovery via documented user-mode APIs:
//  - BluetoothFindFirstDevice/BluetoothEnumerateInstalledServices (bthprops)
//  - Winsock NS_BTH SDP query for the A2DP AudioSink record (0x110B)
//  - CfgMgr32 for the BTHENUM\{0000110B} devnode and the driver bound to it
// AVDTP codec capabilities are NOT obtainable here: they require an L2CAP channel (kernel driver).
#pragma once

#include <optional>
#include <span>
#include <string>
#include <vector>

#include "common/json.h"
#include "common/status.h"
#include "common/types.h"

namespace bta {

struct A2dpSinkDriverBinding {
    bool present = false;
    std::string instanceId;
    std::string service;      // e.g. "BthA2dp" (in-box) or "BtA2dpL2cap" (this project)
    uint32_t problemCode = 0;
    bool started = false;
};

struct BtDeviceInfo {
    BdAddr address;
    std::string name;
    uint32_t classOfDevice = 0;
    bool connected = false, remembered = false, authenticated = false;
    bool a2dpSinkService = false;  // AudioSink service enabled for this device
    A2dpSinkDriverBinding driver;

    bool IsAudioClass() const { return ((classOfDevice >> 8) & 0x1F) == 0x04; }
    Json ToJson() const;
};

struct A2dpSdpRecord {
    bool found = false;
    uint16_t l2capPsm = 0;
    uint16_t avdtpVersion = 0;   // e.g. 0x0103
    uint16_t a2dpVersion = 0;    // e.g. 0x0104
    uint16_t supportedFeatures = 0;
    std::string serviceName;
    Json ToJson() const;
};

Result<std::vector<BtDeviceInfo>> EnumeratePairedDevices();
A2dpSinkDriverBinding FindA2dpSinkDriver(BdAddr addr);
// Pages the device (it must be in range). Uses the SDP cache unless flush is set.
Result<A2dpSdpRecord> QueryA2dpSinkSdp(BdAddr addr, bool flushCache = true);
// Exposed for unit tests.
std::optional<A2dpSdpRecord> ParseA2dpSdpRecord(std::span<const uint8_t> record);
bool BluetoothRadioPresent(std::string* name = nullptr);

}  // namespace bta
