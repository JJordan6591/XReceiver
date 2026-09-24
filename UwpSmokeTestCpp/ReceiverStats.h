#pragma once

#include <array>
#include <atomic>
#include <cstdint>

namespace rx
{
    enum class Stat : size_t
    {
        // Video RTP
        VideoPackets,
        VideoBytes,
        VideoLost,
        VideoSequenceGaps,
        VideoDuplicate,
        VideoReordered,
        VideoLate,
        VideoInvalid,
        VideoWrongPayloadType,
        VideoForeignSsrc,
        VideoOutOfWindow,
        VideoTooLarge,
        VideoSocketErrors,
        VideoSsrc,
        VideoLastSequence,
        VideoLastTimestamp,
        VideoStreamRestarts,
        VideoJitterUs,

        // H.264
        UnsupportedNal,
        MalformedPayload,
        FuaErrors,
        StapaErrors,
        AccessUnitsComplete,
        AccessUnitsIncomplete,
        AccessUnitsMissingMarker,
        AccessUnitsDiscarded,
        IdrCount,
        IdrIntervalMs,
        Width,
        Height,
        Profile,
        Level,
        BitstreamRestriction,
        MaxDecFrameBuffering,
        PocType,
        HevcDetected,

        // Frame delivery
        FramesSubmitted,
        SubmittedBytes,
        DropStale,
        DropQueueFull,          // rejected by the playing-phase hard cap (back-pressure)
        DropStartupFull,        // rejected by the startup buffer cap
        IdrWaitsNetwork,        // IDR waits caused by damaged network input
        IdrWaitsBackpressure,   // IDR waits caused by a reference frame rejected for capacity
        DeliveryPhase,          // 0 none, 1 opening, 2 starting, 3 playing
        StartupPeakFrames,      // largest queue while starting (current source)
        DropAwaitingIdr,
        DropNonRef,
        DropIncomplete,
        PtsDiscontinuities,
        SampleErrors,
        FrameQueueDepth,
        SamplesInFlight,        // submitted minus Processed callbacks (diagnostic only)
        SampleRequests,
        SampleDeferrals,
        SamplesProcessed,
        SamplesRendered,
        PendingRequests,
        OverlappingRequests,
        EndOfStreamCompletions,
        LastRequestAgeMs,
        SampleLagUs,
        PtsLeadUs,
        ReceiveToSubmitP50Us,
        ReceiveToSubmitP95Us,
        SourceBuilds,
        MediaFailures,
        RealTimePlayback,
        WaitingForKeyframe,

        // Audio RTP
        AudioPackets,
        AudioBytes,
        AudioLost,
        AudioDuplicate,
        AudioReordered,
        AudioLate,
        AudioInvalid,
        AudioWrongPayloadType,
        AudioForeignSsrc,
        AudioSocketErrors,
        AudioSsrc,
        AudioStreamRestarts,
        AudioJitterUs,
        AudioPartialFrames,
        AudioDiscontinuities,

        // Audio output
        AudioRunning,
        AudioUnderruns,
        AudioConcealedFrames,
        AudioOverflowFrames,
        AudioHardCapDropFrames,
        AudioFillUs,
        AudioTargetDelayMs,
        AudioDriftPpm,
        AudioOutputLatencyUs,
        AudioGraphRate,
        AudioQuantumSamples,
        AudioResamplerActive,
        AudioCallbackErrors,
        AudioGraphErrors,

        Count
    };

    class ReceiverStats
    {
    public:
        using Snapshot = std::array<int64_t, static_cast<size_t>(Stat::Count)>;

        ReceiverStats() { Reset(); }

        void Reset()
        {
            for (auto& v : m_values)
            {
                v.store(0, std::memory_order_relaxed);
            }
        }

        void Add(Stat s, int64_t delta = 1) { m_values[Index(s)].fetch_add(delta, std::memory_order_relaxed); }
        void Set(Stat s, int64_t value) { m_values[Index(s)].store(value, std::memory_order_relaxed); }
        int64_t Get(Stat s) const { return m_values[Index(s)].load(std::memory_order_relaxed); }

        Snapshot Take() const
        {
            Snapshot snapshot{};
            for (size_t i = 0; i < snapshot.size(); ++i)
            {
                snapshot[i] = m_values[i].load(std::memory_order_relaxed);
            }
            return snapshot;
        }

        static constexpr size_t Index(Stat s) { return static_cast<size_t>(s); }

    private:
        std::array<std::atomic<int64_t>, static_cast<size_t>(Stat::Count)> m_values;
    };

    inline int64_t At(ReceiverStats::Snapshot const& snapshot, Stat s)
    {
        return snapshot[ReceiverStats::Index(s)];
    }
}
