// IL2capConnector backed by BtA2dpL2cap.sys (see include/bta2dp_ioctl.h).
#pragma once

#include <string>
#include <vector>

#include "transport/l2cap.h"

namespace bta {

struct L2capDriverInstance {
    std::wstring interfacePath;
    BdAddr remote;
    uint32_t version = 0;
    bool remoteConnected = false;
};

// Enumerates present driver instances (one per bound sink). Empty if the driver is not installed.
std::vector<L2capDriverInstance> EnumerateL2capDriverInstances(std::string* error = nullptr);

class KernelL2capConnector : public IL2capConnector {
public:
    std::string Name() const override { return "kernel (BtA2dpL2cap.sys)"; }
    Status CheckAvailable(BdAddr addr) const override;
    Result<std::unique_ptr<IL2capChannel>> Connect(const L2capConnectParams& p) override;
    Result<std::unique_ptr<IL2capChannel>> Accept(BdAddr addr, uint16_t psm, uint32_t timeoutMs) override;

private:
    Result<std::wstring> FindInterface(BdAddr addr) const;
};

}  // namespace bta
