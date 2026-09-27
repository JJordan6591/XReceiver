#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "MediaClock.h"
#include "RtpPacket.h"

namespace rx
{
    // Bounded RTP reorder buffer. In-order packets pass straight through; a gap holds later
    // packets for at most `timeout` before the missing ones are declared lost. Storage is a
    // fixed pool of slots allocated once, so an interrupted stream cannot grow memory.
    class JitterBuffer
    {
    public:
        struct Config
        {
            size_t capacity = 512;          // must exceed reorderWindow
            int64_t reorderWindow = 256;    // packets ahead of the expected sequence we will hold
            Clock::duration timeout = std::chrono::milliseconds(10);
            size_t maxPacketBytes = 2048;
        };

        struct Sink
        {
            virtual ~Sink() = default;
            virtual void OnOrderedPacket(RtpPacketView const& packet, int64_t extSequence, Clock::time_point arrival) = 0;
            virtual void OnPacketsLost(int64_t count) = 0;
        };

        enum class InsertResult
        {
            Delivered,
            DeliveredReordered,
            Buffered,
            Duplicate,
            Late,
            Resynced,
            TooLarge,
        };

        JitterBuffer();
        explicit JitterBuffer(Config const& config);

        void Configure(Config const& config);
        void Reset();

        // `packet` must describe `data`; it is used directly for the in-order fast path.
        InsertResult Insert(uint8_t const* data, size_t size, RtpPacketView const& packet, int64_t extSequence,
                            Clock::time_point now, Sink& sink);

        // Releases packets whose gap has exceeded the timeout.
        void Poll(Clock::time_point now, Sink& sink);

        size_t BufferedCount() const { return m_buffered; }

    private:
        struct Slot
        {
            bool used = false;
            int64_t extSequence = 0;
            Clock::time_point arrival{};
            size_t size = 0;
        };

        size_t IndexOf(int64_t extSequence) const { return static_cast<size_t>(extSequence % static_cast<int64_t>(m_slots.size())); }
        void DeliverSlot(size_t index, Sink& sink);
        void Drain(Sink& sink);
        void SkipToNextBuffered(Sink& sink);
        void FlushAll(Sink& sink);
        void RecomputeGapStart();
        bool IsDuplicate(int64_t extSequence) const;
        void MarkSeen(int64_t extSequence);

        Config m_config;
        std::vector<Slot> m_slots;
        std::vector<uint8_t> m_storage;
        size_t m_buffered = 0;
        bool m_started = false;
        int64_t m_expected = 0;
        Clock::time_point m_gapSince{};
        std::array<int64_t, 1024> m_seen{};
    };
}
