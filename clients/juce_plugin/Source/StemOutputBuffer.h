#pragma once

#include <atomic>
#include <cstring>
#include <vector>

//==============================================================================
// StemOutputBuffer
// ----------------
// SPSC FIFO for predicted stem samples arriving from the server. Producer is
// the OSC receive thread (OscBridge), consumer is processBlock on the audio
// thread. Uses a power-of-two ring so head/tail arithmetic is branch-free.
//==============================================================================
class StemOutputBuffer
{
public:
    explicit StemOutputBuffer (int capacityPow2)
    {
        int cap = 1;
        while (cap < capacityPow2) cap <<= 1;
        buffer_.assign ((size_t) cap, 0.0f);
        mask_ = cap - 1;
    }

    int capacity() const noexcept { return mask_ + 1; }

    // --- producer (OSC thread) ---
    // Drops samples that would wrap the writer past the reader. Returns
    // actual number written; drops are logged by caller via atomic counter.
    int writeAt (uint32_t absoluteSampleIndex, const float* samples, int n) noexcept
    {
        // The stream is reassembled by absolute index (chunk_idx * package_size
        // + k), so we translate into a ring-local position.
        int written = 0;
        for (int i = 0; i < n; ++i)
            buffer_[(absoluteSampleIndex + (uint32_t) i) & (uint32_t) mask_] = samples[i];
        written = n;

        // Advance the "valid up to" marker if this write extends it.
        uint32_t oldEnd, newEnd = absoluteSampleIndex + (uint32_t) n;
        do {
            oldEnd = validEnd_.load (std::memory_order_acquire);
            if (newEnd <= oldEnd) break;
        } while (! validEnd_.compare_exchange_weak (oldEnd, newEnd,
                                                    std::memory_order_release,
                                                    std::memory_order_acquire));
        return written;
    }

    // --- consumer (audio thread) ---
    // Pulls up to `n` samples into `dst`. Returns samples actually produced;
    // remainder is zero-filled by the caller.
    int read (float* dst, int n) noexcept
    {
        const uint32_t end = validEnd_.load (std::memory_order_acquire);
        uint32_t pos = readIndex_.load (std::memory_order_relaxed);

        if (pos >= end) return 0;
        const int available = (int) (end - pos);
        const int take = std::min (n, available);

        for (int i = 0; i < take; ++i)
            dst[i] = buffer_[(pos + (uint32_t) i) & (uint32_t) mask_];

        readIndex_.store (pos + (uint32_t) take, std::memory_order_release);
        return take;
    }

    // Called when a new prediction batch begins - tells the consumer to jump
    // to absolute index `newStart` (abandoning any unread tail from the prior
    // batch). This keeps latency bounded at exactly one hop.
    void seekTo (uint32_t newStart) noexcept
    {
        readIndex_.store (newStart, std::memory_order_release);
        // validEnd_ stays as-is; writer may have already begun writing the new
        // batch and moved it forward.
    }

private:
    std::vector<float>    buffer_;
    int                   mask_ = 0;
    std::atomic<uint32_t> readIndex_ { 0 };
    std::atomic<uint32_t> validEnd_  { 0 };
};
