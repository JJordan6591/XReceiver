#pragma once

#include <chrono>
#include <cstdint>
#include <deque>

namespace rx
{
    using Clock = std::chrono::steady_clock;

    inline double ToMs(Clock::duration d)
    {
        return std::chrono::duration<double, std::milli>(d).count();
    }

    inline Clock::duration FromMs(int64_t ms)
    {
        return std::chrono::duration_cast<Clock::duration>(std::chrono::milliseconds(ms));
    }

    // Tracks (local arrival - RTP media time) per packet. The sliding-window minimum is the
    // "fastest path" transit, i.e. the RTP-to-local mapping with network queuing removed.
    class TransitEstimator
    {
    public:
        explicit TransitEstimator(Clock::duration window = std::chrono::seconds(2));

        void Reset();
        void Add(Clock::time_point arrival, int64_t extRtpTimestamp, uint32_t clockRate);

        bool HasValue() const { return !m_window.empty(); }
        double BaseTransitSeconds() const;
        double JitterMs() const { return m_jitterSeconds * 1000.0; }

        // Estimated local capture time of a packet, using the minimum-transit mapping.
        Clock::time_point LocalTimeOf(int64_t extRtpTimestamp, uint32_t clockRate) const;

    private:
        struct Sample
        {
            Clock::time_point arrival;
            double transit;
        };

        Clock::duration m_windowLength;
        std::deque<Sample> m_window;
        bool m_hasEpoch = false;
        Clock::time_point m_epoch{};
        int64_t m_baseTimestamp = 0;
        bool m_hasLast = false;
        double m_lastTransit = 0.0;
        double m_jitterSeconds = 0.0;
    };

    struct AvSyncInputs
    {
        double videoPipelineLatencyMs = 60.0;   // configured estimate of decode + render
        double videoReceiveToSubmitMs = 0.0;    // measured p50
        double audioOutputLatencyMs = 0.0;      // AudioGraph latency
        double audioJitterMs = 0.0;             // RFC 3550 jitter of the audio stream
        double userOffsetMs = 0.0;              // positive delays audio further
        double minDelayMs = 20.0;
        double maxDelayMs = 300.0;
    };

    // Audio is delayed to line up with video, which is presented as fast as possible.
    int32_t ComputeAudioTargetDelayMs(AvSyncInputs const& inputs);
}
