#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>

namespace rx
{
    // Single-producer / single-consumer ring of interleaved float frames. The producer is the
    // audio socket thread and the consumer is the AudioGraph quantum thread; neither blocks.
    // Only the consumer moves the read index, so a producer-side flush is a request that the
    // consumer honours on its next quantum.
    class PcmRingBuffer
    {
    public:
        PcmRingBuffer(size_t capacityFrames, uint32_t channels) :
            m_capacity(capacityFrames),
            m_channels(channels),
            m_data(capacityFrames * channels, 0.0f)
        {
        }

        size_t CapacityFrames() const { return m_capacity; }
        uint32_t Channels() const { return m_channels; }

        size_t Available() const
        {
            return static_cast<size_t>(m_write.load(std::memory_order_acquire) - m_read.load(std::memory_order_acquire));
        }

        // Producer. Returns frames written; the remainder did not fit.
        size_t Write(float const* frames, size_t count)
        {
            uint64_t const write = m_write.load(std::memory_order_relaxed);
            uint64_t const read = m_read.load(std::memory_order_acquire);
            size_t const freeFrames = m_capacity - static_cast<size_t>(write - read);
            size_t const toWrite = std::min(count, freeFrames);
            size_t const start = static_cast<size_t>(write % m_capacity);
            size_t const first = std::min(toWrite, m_capacity - start);
            std::memcpy(&m_data[start * m_channels], frames, first * m_channels * sizeof(float));
            if (toWrite > first)
            {
                std::memcpy(&m_data[0], frames + first * m_channels, (toWrite - first) * m_channels * sizeof(float));
            }
            m_write.store(write + toWrite, std::memory_order_release);
            return toWrite;
        }

        // Consumer. Returns frames read.
        size_t Read(float* out, size_t count)
        {
            uint64_t const read = m_read.load(std::memory_order_relaxed);
            uint64_t const write = m_write.load(std::memory_order_acquire);
            size_t const available = static_cast<size_t>(write - read);
            size_t const toRead = std::min(count, available);
            size_t const start = static_cast<size_t>(read % m_capacity);
            size_t const first = std::min(toRead, m_capacity - start);
            std::memcpy(out, &m_data[start * m_channels], first * m_channels * sizeof(float));
            if (toRead > first)
            {
                std::memcpy(out + first * m_channels, &m_data[0], (toRead - first) * m_channels * sizeof(float));
            }
            m_read.store(read + toRead, std::memory_order_release);
            return toRead;
        }

        // Consumer. Drops up to `count` of the oldest frames.
        size_t Discard(size_t count)
        {
            uint64_t const read = m_read.load(std::memory_order_relaxed);
            uint64_t const write = m_write.load(std::memory_order_acquire);
            size_t const toDrop = std::min(count, static_cast<size_t>(write - read));
            m_read.store(read + toDrop, std::memory_order_release);
            return toDrop;
        }

        void RequestFlush() { m_flushRequested.store(true, std::memory_order_release); }
        bool ConsumeFlushRequest() { return m_flushRequested.exchange(false, std::memory_order_acq_rel); }

    private:
        size_t m_capacity;
        uint32_t m_channels;
        std::vector<float> m_data;
        std::atomic<uint64_t> m_write{ 0 };
        std::atomic<uint64_t> m_read{ 0 };
        std::atomic<bool> m_flushRequested{ false };
    };
}
