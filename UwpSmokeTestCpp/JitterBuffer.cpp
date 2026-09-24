#include "pch.h"
#include "JitterBuffer.h"

#include <algorithm>
#include <cstring>

namespace rx
{
    JitterBuffer::JitterBuffer() : JitterBuffer(Config{})
    {
    }

    JitterBuffer::JitterBuffer(Config const& config)
    {
        Configure(config);
    }

    void JitterBuffer::Configure(Config const& config)
    {
        m_config = config;
        if (m_config.reorderWindow < 1)
        {
            m_config.reorderWindow = 1;
        }
        if (m_config.capacity <= static_cast<size_t>(m_config.reorderWindow))
        {
            m_config.capacity = static_cast<size_t>(m_config.reorderWindow) + 1;
        }
        m_slots.assign(m_config.capacity, Slot{});
        m_storage.assign(m_config.capacity * m_config.maxPacketBytes, 0);
        Reset();
    }

    void JitterBuffer::Reset()
    {
        for (auto& slot : m_slots)
        {
            slot.used = false;
        }
        m_buffered = 0;
        m_started = false;
        m_expected = 0;
        m_seen.fill(-1);
    }

    JitterBuffer::InsertResult JitterBuffer::Insert(uint8_t const* data, size_t size, RtpPacketView const& packet,
                                                    int64_t extSequence, Clock::time_point now, Sink& sink)
    {
        if (size > m_config.maxPacketBytes)
        {
            return InsertResult::TooLarge;
        }

        if (!m_started)
        {
            m_started = true;
            m_expected = extSequence;
        }

        int64_t const d = extSequence - m_expected;

        if (d < 0)
        {
            if (IsDuplicate(extSequence))
            {
                return InsertResult::Duplicate;
            }
            MarkSeen(extSequence);
            return InsertResult::Late;
        }

        if (d == 0)
        {
            bool const filledGap = m_buffered > 0;
            MarkSeen(extSequence);
            ++m_expected;
            sink.OnOrderedPacket(packet, extSequence, now);
            Drain(sink);
            if (m_buffered > 0)
            {
                RecomputeGapStart();
            }
            return filledGap ? InsertResult::DeliveredReordered : InsertResult::Delivered;
        }

        if (d > m_config.reorderWindow)
        {
            FlushAll(sink);
            int64_t const lost = extSequence - m_expected;
            if (lost > 0)
            {
                sink.OnPacketsLost(lost);
            }
            m_expected = extSequence + 1;
            MarkSeen(extSequence);
            sink.OnOrderedPacket(packet, extSequence, now);
            return InsertResult::Resynced;
        }

        size_t const index = IndexOf(extSequence);
        Slot& slot = m_slots[index];
        if (slot.used)
        {
            if (slot.extSequence == extSequence)
            {
                return InsertResult::Duplicate;
            }
            // Cannot happen while capacity > reorderWindow; treat defensively as a stale slot.
            slot.used = false;
            --m_buffered;
        }

        std::memcpy(m_storage.data() + index * m_config.maxPacketBytes, data, size);
        slot.used = true;
        slot.extSequence = extSequence;
        slot.arrival = now;
        slot.size = size;
        if (++m_buffered == 1)
        {
            m_gapSince = now;
        }

        if (m_buffered >= static_cast<size_t>(m_config.reorderWindow))
        {
            SkipToNextBuffered(sink);
        }
        return InsertResult::Buffered;
    }

    void JitterBuffer::Poll(Clock::time_point now, Sink& sink)
    {
        while (m_buffered > 0 && now - m_gapSince >= m_config.timeout)
        {
            SkipToNextBuffered(sink);
        }
    }

    void JitterBuffer::DeliverSlot(size_t index, Sink& sink)
    {
        Slot& slot = m_slots[index];
        slot.used = false;
        --m_buffered;
        MarkSeen(slot.extSequence);

        RtpPacketView packet;
        uint8_t const* data = m_storage.data() + index * m_config.maxPacketBytes;
        if (ParseRtpPacket(data, slot.size, packet) == RtpParseResult::Ok)
        {
            sink.OnOrderedPacket(packet, slot.extSequence, slot.arrival);
        }
    }

    void JitterBuffer::Drain(Sink& sink)
    {
        while (m_buffered > 0)
        {
            size_t const index = IndexOf(m_expected);
            Slot const& slot = m_slots[index];
            if (!slot.used || slot.extSequence != m_expected)
            {
                break;
            }
            DeliverSlot(index, sink);
            ++m_expected;
        }
    }

    void JitterBuffer::SkipToNextBuffered(Sink& sink)
    {
        if (m_buffered == 0)
        {
            return;
        }

        for (int64_t candidate = m_expected + 1; candidate <= m_expected + m_config.reorderWindow; ++candidate)
        {
            Slot const& slot = m_slots[IndexOf(candidate)];
            if (slot.used && slot.extSequence == candidate)
            {
                sink.OnPacketsLost(candidate - m_expected);
                m_expected = candidate;
                Drain(sink);
                if (m_buffered > 0)
                {
                    RecomputeGapStart();
                }
                return;
            }
        }

        // Buffered count and slots disagree; recover by discarding everything.
        for (auto& slot : m_slots)
        {
            slot.used = false;
        }
        m_buffered = 0;
    }

    void JitterBuffer::FlushAll(Sink& sink)
    {
        while (m_buffered > 0)
        {
            SkipToNextBuffered(sink);
        }
    }

    void JitterBuffer::RecomputeGapStart()
    {
        bool found = false;
        Clock::time_point oldest{};
        for (int64_t candidate = m_expected + 1; candidate <= m_expected + m_config.reorderWindow; ++candidate)
        {
            Slot const& slot = m_slots[IndexOf(candidate)];
            if (slot.used && slot.extSequence == candidate)
            {
                if (!found || slot.arrival < oldest)
                {
                    oldest = slot.arrival;
                    found = true;
                }
            }
        }
        if (found)
        {
            m_gapSince = oldest;
        }
    }

    bool JitterBuffer::IsDuplicate(int64_t extSequence) const
    {
        return m_seen[static_cast<size_t>(extSequence % static_cast<int64_t>(m_seen.size()))] == extSequence;
    }

    void JitterBuffer::MarkSeen(int64_t extSequence)
    {
        m_seen[static_cast<size_t>(extSequence % static_cast<int64_t>(m_seen.size()))] = extSequence;
    }
}
