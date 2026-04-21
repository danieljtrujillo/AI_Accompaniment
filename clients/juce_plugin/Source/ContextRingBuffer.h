#pragma once

#include <atomic>
#include <cstring>
#include <vector>

//==============================================================================
// ContextRingBuffer
// -----------------
// Lock-free SPSC mono sample accumulator used by the audio thread to feed the
// context window. The audio thread calls push() on every block; a background
// thread calls snapshot() every hop to copy out the most recent T samples.
//
// Writer (audio) is wait-free. Reader (network) uses a memcpy snapshot and
// is allowed to contend with the writer - worst case it reads torn samples
// at one block boundary, which is inaudible in a 6-second window.
//==============================================================================
class ContextRingBuffer
{
public:
    explicit ContextRingBuffer (int capacity)
        : buffer_ ((size_t) capacity, 0.0f), capacity_ (capacity) {}

    int capacity() const noexcept { return capacity_; }

    // --- audio thread ---
    void push (const float* samples, int n) noexcept
    {
        int w = writeIndex_.load (std::memory_order_relaxed);
        const int cap = capacity_;

        const int first = std::min (n, cap - w);
        std::memcpy (buffer_.data() + w, samples, (size_t) first * sizeof (float));

        const int second = n - first;
        if (second > 0)
            std::memcpy (buffer_.data(), samples + first, (size_t) second * sizeof (float));

        w = (w + n) % cap;
        writeIndex_.store (w, std::memory_order_release);
        totalWritten_.fetch_add ((uint64_t) n, std::memory_order_release);
    }

    // --- network thread ---
    // Copies the most recent `capacity_` samples in chronological order into
    // `out`, which must be sized >= capacity_. Returns the total number of
    // samples ever written (so the caller can tell whether the ring has
    // filled at least once).
    uint64_t snapshot (float* out) const noexcept
    {
        const int w = writeIndex_.load (std::memory_order_acquire);
        const int cap = capacity_;

        // The oldest sample in the ring sits at index w.
        const int first = cap - w;
        std::memcpy (out,            buffer_.data() + w, (size_t) first  * sizeof (float));
        std::memcpy (out + first,    buffer_.data(),     (size_t) w      * sizeof (float));

        return totalWritten_.load (std::memory_order_acquire);
    }

private:
    std::vector<float>   buffer_;
    int                  capacity_;
    std::atomic<int>     writeIndex_   { 0 };
    std::atomic<uint64_t> totalWritten_ { 0 };
};
