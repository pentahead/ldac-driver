// Lock-free single-producer/single-consumer ring of interleaved float frames.
// Overflow drops the *new* data and counts it, so the consumer never observes torn frames.
#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

namespace bta {

class PcmRing {
public:
    PcmRing(size_t capacityFrames, int channels);

    // Producer side. Returns frames written (< frames on overflow).
    size_t Write(const float* frames, size_t count);
    // Consumer side. Returns frames read.
    size_t Read(float* frames, size_t count);
    size_t Available() const;   // frames readable
    size_t FreeSpace() const;   // frames writable
    size_t Capacity() const { return cap_; }
    int Channels() const { return ch_; }
    uint64_t OverflowFrames() const { return overflow_.load(std::memory_order_relaxed); }
    // Consumer-side reset (drops buffered data). Only call while the producer is quiescent.
    void Clear();

private:
    size_t cap_;  // frames; one slot kept empty
    int ch_;
    std::vector<float> buf_;
    std::atomic<size_t> head_{0};  // write index (frames)
    std::atomic<size_t> tail_{0};  // read index (frames)
    std::atomic<uint64_t> overflow_{0};
};

}  // namespace bta
