// L2CAP channel abstraction. AVDTP signalling and media run over two channels to PSM 0x19.
// Implementations: LoopbackConnector (tests/simulation) and KernelL2capConnector (BtA2dpL2cap.sys).
#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "common/status.h"
#include "common/types.h"

namespace bta {

class IL2capChannel {
public:
    virtual ~IL2capChannel() = default;
    // Sends one L2CAP SDU (must be <= OutMtu()).
    virtual Status Send(std::span<const uint8_t> sdu) = 0;
    // Waits up to timeoutMs for one SDU. Returns Err::Timeout if none, Err::Disconnected if closed.
    virtual Status Receive(std::vector<uint8_t>& sdu, uint32_t timeoutMs) = 0;
    virtual uint16_t OutMtu() const = 0;
    virtual uint16_t InMtu() const = 0;
    virtual bool IsOpen() const = 0;
    virtual void Close() = 0;
    // Outbound SDUs accepted but not yet completed by the controller (0 if unknown).
    virtual uint32_t PendingSends() const { return 0; }
};

struct L2capConnectParams {
    BdAddr addr;
    uint16_t psm = 0x0019;
    uint16_t inMtu = 1024;      // MTU we accept
    uint16_t flushTimeoutMs = 0;  // 0 = infinite (reliable); media channels may set a finite value
    uint32_t timeoutMs = 10000;
};

class IL2capConnector {
public:
    virtual ~IL2capConnector() = default;
    virtual std::string Name() const = 0;
    // Ok if the transport can be used at all (e.g. driver installed and bound to the device).
    virtual Status CheckAvailable(BdAddr addr) const = 0;
    virtual Result<std::unique_ptr<IL2capChannel>> Connect(const L2capConnectParams& p) = 0;
    // Waits for a remote-initiated channel on psm (sink-initiated media channel).
    virtual Result<std::unique_ptr<IL2capChannel>> Accept(BdAddr addr, uint16_t psm, uint32_t timeoutMs) = 0;
};

}  // namespace bta
