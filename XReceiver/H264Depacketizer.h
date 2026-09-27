#pragma once

#include <cstdint>
#include <functional>

#include "H264AccessUnit.h"
#include "MediaClock.h"

namespace rx
{
    // RFC 6184 non-interleaved mode depacketizer (single NAL unit, STAP-A, FU-A) with
    // access-unit assembly. Packets are grouped by SSRC and RTP timestamp. An access unit is
    // complete only when its marker packet arrives, every sequence number from its first to
    // its last packet was seen, and every NAL (including fragmented ones) is whole. Anything
    // else is emitted as corrupt with its payload discarded.
    class H264Depacketizer
    {
    public:
        struct Limits
        {
            size_t maxNalBytes = 2 * 1024 * 1024;
            size_t maxAuBytes = 4 * 1024 * 1024;
            // A real IDR at MTU size stays well under these. They stop fragment/aggregation storms.
            size_t maxFuFragments = 8192;
            size_t maxStapNals = 32;
            size_t maxNals = 256;
        };

        struct Counters
        {
            uint64_t completeAccessUnits = 0;
            uint64_t incompleteAccessUnits = 0;
            uint64_t sequenceGaps = 0;          // gap events seen in the ordered packet stream
            uint64_t missingPackets = 0;        // sequence numbers skipped by those gaps
            uint64_t staleOrDuplicate = 0;      // packets at or behind the last sequence
            uint64_t missingMarker = 0;         // timestamp or SSRC changed before a marker
            uint64_t fuaErrors = 0;             // missing start, missing end, bad header
            uint64_t stapaErrors = 0;
            uint64_t emptyPayload = 0;
            uint64_t unsupportedNal = 0;
            uint64_t oversize = 0;
            uint64_t forbiddenBit = 0;
        };

        struct PacketInfo
        {
            uint32_t ssrc = 0;
            int64_t extSequence = 0;
            int64_t extTimestamp = 0;
            bool marker = false;
            Clock::time_point arrival{};
        };

        using Callback = std::function<void(AccessUnitPtr)>;

        explicit H264Depacketizer(Callback callback);
        H264Depacketizer(Callback callback, Limits limits);

        // Forgets sequence history; the next access unit is treated as joining mid-stream.
        void Reset();

        // Packets must be delivered in sequence order (the jitter buffer does this); any
        // discontinuity in extSequence is treated as loss.
        void OnPacket(uint8_t const* payload, size_t size, PacketInfo const& info);

        Counters const& GetCounters() const { return m_counters; }

    private:
        void BeginAccessUnit(PacketInfo const& info, bool headMissing, bool headUnverified);
        void FinishAccessUnit(Clock::time_point now);
        void MarkCorrupt(bool confined);
        void AppendNal(uint8_t const* nal, size_t size);
        void NoteSliceStart(uint8_t header, uint8_t const* sliceData, size_t sliceSize);
        void Classify(NalRef const& ref);
        void HandleStapA(uint8_t const* payload, size_t size);
        void HandleFuA(uint8_t const* payload, size_t size);
        void CompleteFragment();
        void AbortFragment();
        bool Fits(size_t additionalBytes);

        Callback m_callback;
        Limits m_limits;
        Counters m_counters;
        AccessUnitPtr m_au;
        bool m_overflow = false;

        bool m_hasLastSequence = false;
        int64_t m_lastSequence = 0;

        // Set when the current AU's leading packets cannot be proven present (first AU after a
        // reset). Such an AU is accepted only if its first slice starts at macroblock 0.
        bool m_headUnverified = false;
        bool m_firstSliceSeen = false;
        bool m_firstSliceAtMb0 = false;

        bool m_fuActive = false;
        size_t m_fuFragments = 0;
        size_t m_fuStartOffset = 0;
        uint8_t m_fuType = 0;
        uint8_t m_fuNri = 0;
    };
}
