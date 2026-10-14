// In-process L2CAP channel pairs for tests and the simulator.
#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>

#include "transport/l2cap.h"

namespace bta {

// Creates two connected channel ends. SDUs larger than the receiver's MTU are rejected.
std::pair<std::unique_ptr<IL2capChannel>, std::unique_ptr<IL2capChannel>> MakeLoopbackPair(uint16_t mtuA,
                                                                                          uint16_t mtuB);

// Connector that hands the remote end of every new channel to a peer callback
// (the simulated sink). Outgoing (local) MTU and the remote MTU are configurable.
class LoopbackConnector : public IL2capConnector {
public:
    using PeerHandler = std::function<void(uint16_t psm, std::unique_ptr<IL2capChannel> remoteEnd)>;

    explicit LoopbackConnector(PeerHandler handler, uint16_t remoteMtu = 895);

    std::string Name() const override { return "loopback"; }
    Status CheckAvailable(BdAddr) const override { return Status::Ok(); }
    Result<std::unique_ptr<IL2capChannel>> Connect(const L2capConnectParams& p) override;
    Result<std::unique_ptr<IL2capChannel>> Accept(BdAddr addr, uint16_t psm, uint32_t timeoutMs) override;

    // Peer side: open a channel towards the local side (delivered via Accept()).
    std::unique_ptr<IL2capChannel> PeerInitiate(uint16_t psm, uint16_t localInMtu = 1024);
    // Fault injection: fail the next N Connect() calls.
    void FailNextConnects(int n) { failConnects_ = n; }

private:
    PeerHandler handler_;
    uint16_t remoteMtu_;
    int failConnects_ = 0;
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<std::unique_ptr<IL2capChannel>> incoming_;
};

}  // namespace bta
