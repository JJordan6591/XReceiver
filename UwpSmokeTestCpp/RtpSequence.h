#pragma once

#include <cstddef>
#include <cstdint>

#include "MediaClock.h"
#include "RtpPacket.h"

namespace rx
{
    // Extends 16-bit RTP sequence numbers to 64 bits. The initial offset keeps values positive
    // even when early packets arrive slightly out of order.
    class SequenceUnwrapper
    {
    public:
        void Reset() { m_initialized = false; }

        int64_t Unwrap(uint16_t sequence)
        {
            if (!m_initialized)
            {
                m_initialized = true;
                m_highest = (int64_t{ 1 } << 32) + sequence;
                return m_highest;
            }
            int16_t const diff = static_cast<int16_t>(static_cast<uint16_t>(sequence - static_cast<uint16_t>(m_highest)));
            int64_t const extended = m_highest + diff;
            if (extended > m_highest)
            {
                m_highest = extended;
            }
            return extended;
        }

    private:
        bool m_initialized = false;
        int64_t m_highest = 0;
    };

    // Extends 32-bit RTP timestamps to 64 bits.
    class TimestampUnwrapper
    {
    public:
        void Reset() { m_initialized = false; }

        int64_t Unwrap(uint32_t timestamp)
        {
            if (!m_initialized)
            {
                m_initialized = true;
                m_highest = (int64_t{ 1 } << 40) + timestamp;
                return m_highest;
            }
            int32_t const diff = static_cast<int32_t>(timestamp - static_cast<uint32_t>(m_highest));
            int64_t const extended = m_highest + diff;
            if (extended > m_highest)
            {
                m_highest = extended;
            }
            return extended;
        }

    private:
        bool m_initialized = false;
        int64_t m_highest = 0;
    };

    enum class TrackDecision
    {
        Accept,
        Ignore,
        NewStream,
    };

    // Locks onto one SSRC and detects stream restarts (SSRC change or large sequence jump),
    // using RFC 3550-style probation so a single stray packet cannot reset the pipeline.
    class RtpStreamTracker
    {
    public:
        static constexpr int kMaxSequenceJump = 3000;

        void Reset()
        {
            m_locked = false;
            m_hasCandidate = false;
            m_hasBadSequence = false;
        }

        bool Locked() const { return m_locked; }
        uint32_t Ssrc() const { return m_ssrc; }

        TrackDecision Check(RtpPacketView const& packet, Clock::time_point now, Clock::duration takeoverAfterSilence)
        {
            if (!m_locked)
            {
                Lock(packet, now);
                return TrackDecision::NewStream;
            }

            if (packet.ssrc != m_ssrc)
            {
                if (now - m_lastAccepted > takeoverAfterSilence)
                {
                    Lock(packet, now);
                    return TrackDecision::NewStream;
                }
                if (m_hasCandidate && packet.ssrc == m_candidateSsrc &&
                    packet.sequence == static_cast<uint16_t>(m_candidateSequence + 1))
                {
                    m_candidateSequence = packet.sequence;
                    if (++m_candidateCount >= 2)
                    {
                        Lock(packet, now);
                        return TrackDecision::NewStream;
                    }
                }
                else
                {
                    m_hasCandidate = true;
                    m_candidateSsrc = packet.ssrc;
                    m_candidateSequence = packet.sequence;
                    m_candidateCount = 1;
                }
                return TrackDecision::Ignore;
            }

            int const d = static_cast<int16_t>(static_cast<uint16_t>(packet.sequence - m_highestSequence));
            if (d > kMaxSequenceJump || d < -kMaxSequenceJump)
            {
                if (m_hasBadSequence && packet.sequence == static_cast<uint16_t>(m_badSequence + 1))
                {
                    Lock(packet, now);
                    return TrackDecision::NewStream;
                }
                m_hasBadSequence = true;
                m_badSequence = packet.sequence;
                return TrackDecision::Ignore;
            }

            m_hasBadSequence = false;
            m_hasCandidate = false;
            if (d > 0)
            {
                m_highestSequence = packet.sequence;
            }
            m_lastAccepted = now;
            return TrackDecision::Accept;
        }

    private:
        void Lock(RtpPacketView const& packet, Clock::time_point now)
        {
            m_locked = true;
            m_ssrc = packet.ssrc;
            m_highestSequence = packet.sequence;
            m_lastAccepted = now;
            m_hasCandidate = false;
            m_hasBadSequence = false;
        }

        bool m_locked = false;
        uint32_t m_ssrc = 0;
        uint16_t m_highestSequence = 0;
        Clock::time_point m_lastAccepted{};

        bool m_hasCandidate = false;
        uint32_t m_candidateSsrc = 0;
        uint16_t m_candidateSequence = 0;
        int m_candidateCount = 0;

        bool m_hasBadSequence = false;
        uint16_t m_badSequence = 0;
    };

    enum class RtpAdmission
    {
        Accept,             // the locked stream
        NewStream,          // first packet of a new or restarted stream
        Invalid,            // not a well-formed RTP packet
        WrongPayloadType,
        TooLarge,           // beyond the receive buffer limit
        TooShort,           // payload smaller than one media unit
        Ignored,            // another sender, or an unconfirmed restart
    };

    struct RtpAdmissionRules
    {
        int32_t payloadType = 96;
        size_t maxPacketBytes = 2048;
        size_t minPayloadBytes = 1;
        Clock::duration takeoverAfterSilence = std::chrono::seconds(1);
    };

    // Every check a datagram must pass before it counts as sender activity. The stream tracker
    // only ever sees well-formed packets of the expected payload type and size, so malformed or
    // mistyped input can never lock, confirm or take over a stream.
    inline RtpAdmission AdmitRtpPacket(uint8_t const* data, size_t size, RtpAdmissionRules const& rules,
                                       RtpStreamTracker& tracker, Clock::time_point now, RtpPacketView& packet)
    {
        if (ParseRtpPacket(data, size, packet) != RtpParseResult::Ok)
        {
            return RtpAdmission::Invalid;
        }
        if (packet.payloadType != rules.payloadType)
        {
            return RtpAdmission::WrongPayloadType;
        }
        if (size > rules.maxPacketBytes)
        {
            return RtpAdmission::TooLarge;
        }
        if (packet.payloadSize < rules.minPayloadBytes)
        {
            return RtpAdmission::TooShort;
        }
        switch (tracker.Check(packet, now, rules.takeoverAfterSilence))
        {
        case TrackDecision::Accept:
            return RtpAdmission::Accept;
        case TrackDecision::NewStream:
            return RtpAdmission::NewStream;
        case TrackDecision::Ignore:
            break;
        }
        return RtpAdmission::Ignored;
    }

    inline bool IsAdmitted(RtpAdmission admission)
    {
        return admission == RtpAdmission::Accept || admission == RtpAdmission::NewStream;
    }
}
