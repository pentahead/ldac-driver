#include "pipeline/pcm_ring.h"

#include <algorithm>
#include <cstring>

namespace bta {

PcmRing::PcmRing(size_t capacityFrames, int channels)
    : cap_(capacityFrames + 1), ch_(channels), buf_((capacityFrames + 1) * static_cast<size_t>(channels)) {}

size_t PcmRing::Available() const
{
    size_t h = head_.load(std::memory_order_acquire), t = tail_.load(std::memory_order_acquire);
    return (h + cap_ - t) % cap_;
}

size_t PcmRing::FreeSpace() const { return cap_ - 1 - Available(); }

size_t PcmRing::Write(const float* in, size_t count)
{
    size_t h = head_.load(std::memory_order_relaxed);
    size_t t = tail_.load(std::memory_order_acquire);
    size_t free = cap_ - 1 - (h + cap_ - t) % cap_;
    size_t n = std::min(count, free);
    if (n < count) overflow_.fetch_add(count - n, std::memory_order_relaxed);
    size_t first = std::min(n, cap_ - h);
    memcpy(&buf_[h * ch_], in, first * ch_ * sizeof(float));
    if (n > first) memcpy(&buf_[0], in + first * ch_, (n - first) * ch_ * sizeof(float));
    head_.store((h + n) % cap_, std::memory_order_release);
    return n;
}

size_t PcmRing::Read(float* out, size_t count)
{
    size_t t = tail_.load(std::memory_order_relaxed);
    size_t h = head_.load(std::memory_order_acquire);
    size_t avail = (h + cap_ - t) % cap_;
    size_t n = std::min(count, avail);
    size_t first = std::min(n, cap_ - t);
    memcpy(out, &buf_[t * ch_], first * ch_ * sizeof(float));
    if (n > first) memcpy(out + first * ch_, &buf_[0], (n - first) * ch_ * sizeof(float));
    tail_.store((t + n) % cap_, std::memory_order_release);
    return n;
}

void PcmRing::Clear() { tail_.store(head_.load(std::memory_order_acquire), std::memory_order_release); }

}  // namespace bta
