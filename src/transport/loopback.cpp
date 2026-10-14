#include "transport/loopback.h"

#include <chrono>

namespace bta {

namespace {

struct Pipe {
    std::mutex mu;
    std::condition_variable cv;
    std::deque<std::vector<uint8_t>> q;
    bool closed = false;
};

class LoopbackChannel : public IL2capChannel {
public:
    LoopbackChannel(std::shared_ptr<Pipe> rx, std::shared_ptr<Pipe> tx, uint16_t inMtu, uint16_t outMtu)
        : rx_(std::move(rx)), tx_(std::move(tx)), inMtu_(inMtu), outMtu_(outMtu) {}
    ~LoopbackChannel() override { Close(); }

    Status Send(std::span<const uint8_t> sdu) override
    {
        if (sdu.size() > outMtu_) return {Err::InvalidArgument, "SDU exceeds peer MTU"};
        std::lock_guard lk(tx_->mu);
        if (tx_->closed) return {Err::Disconnected, "channel closed"};
        tx_->q.emplace_back(sdu.begin(), sdu.end());
        tx_->cv.notify_all();
        return Status::Ok();
    }

    Status Receive(std::vector<uint8_t>& sdu, uint32_t timeoutMs) override
    {
        std::unique_lock lk(rx_->mu);
        if (!rx_->cv.wait_for(lk, std::chrono::milliseconds(timeoutMs), [&] { return !rx_->q.empty() || rx_->closed; }))
            return {Err::Timeout, "receive timeout"};
        if (rx_->q.empty()) return {Err::Disconnected, "channel closed"};
        sdu = std::move(rx_->q.front());
        rx_->q.pop_front();
        return Status::Ok();
    }

    uint16_t OutMtu() const override { return outMtu_; }
    uint16_t InMtu() const override { return inMtu_; }
    bool IsOpen() const override
    {
        std::lock_guard lk(tx_->mu);
        return !tx_->closed;
    }
    void Close() override
    {
        for (auto* p : {rx_.get(), tx_.get()}) {
            std::lock_guard lk(p->mu);
            p->closed = true;
            p->cv.notify_all();
        }
    }

private:
    std::shared_ptr<Pipe> rx_, tx_;
    uint16_t inMtu_, outMtu_;
};

}  // namespace

std::pair<std::unique_ptr<IL2capChannel>, std::unique_ptr<IL2capChannel>> MakeLoopbackPair(uint16_t mtuA, uint16_t mtuB)
{
    auto ab = std::make_shared<Pipe>();
    auto ba = std::make_shared<Pipe>();
    // A receives what B sends (ba); A's outgoing MTU is B's incoming MTU.
    return {std::make_unique<LoopbackChannel>(ba, ab, mtuA, mtuB), std::make_unique<LoopbackChannel>(ab, ba, mtuB, mtuA)};
}

LoopbackConnector::LoopbackConnector(PeerHandler handler, uint16_t remoteMtu)
    : handler_(std::move(handler)), remoteMtu_(remoteMtu) {}

Result<std::unique_ptr<IL2capChannel>> LoopbackConnector::Connect(const L2capConnectParams& p)
{
    if (failConnects_ > 0) {
        --failConnects_;
        return Status{Err::IoError, "injected connect failure"};
    }
    auto [local, remote] = MakeLoopbackPair(p.inMtu, remoteMtu_);
    handler_(p.psm, std::move(remote));
    return std::move(local);
}

Result<std::unique_ptr<IL2capChannel>> LoopbackConnector::Accept(BdAddr, uint16_t, uint32_t timeoutMs)
{
    std::unique_lock lk(mu_);
    if (!cv_.wait_for(lk, std::chrono::milliseconds(timeoutMs), [&] { return !incoming_.empty(); }))
        return Status{Err::Timeout, "no incoming channel"};
    auto ch = std::move(incoming_.front());
    incoming_.pop_front();
    return std::move(ch);
}

std::unique_ptr<IL2capChannel> LoopbackConnector::PeerInitiate(uint16_t, uint16_t localInMtu)
{
    auto [local, remote] = MakeLoopbackPair(localInMtu, remoteMtu_);
    {
        std::lock_guard lk(mu_);
        incoming_.push_back(std::move(local));
    }
    cv_.notify_all();
    return std::move(remote);
}

}  // namespace bta
