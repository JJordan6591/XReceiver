#include "pch.h"
#include "MediaClock.h"

#include <algorithm>
#include <cmath>

namespace rx
{
    TransitEstimator::TransitEstimator(Clock::duration window) :
        m_windowLength(window)
    {
    }

    void TransitEstimator::Reset()
    {
        m_window.clear();
        m_hasEpoch = false;
        m_hasLast = false;
        m_lastTransit = 0.0;
        m_jitterSeconds = 0.0;
    }

    void TransitEstimator::Add(Clock::time_point arrival, int64_t extRtpTimestamp, uint32_t clockRate)
    {
        if (clockRate == 0)
        {
            return;
        }

        if (!m_hasEpoch)
        {
            m_hasEpoch = true;
            m_epoch = arrival;
            m_baseTimestamp = extRtpTimestamp;
        }

        double const arrivalSeconds = std::chrono::duration<double>(arrival - m_epoch).count();
        double const mediaSeconds = static_cast<double>(extRtpTimestamp - m_baseTimestamp) / clockRate;
        double const transit = arrivalSeconds - mediaSeconds;

        if (m_hasLast)
        {
            double const d = std::fabs(transit - m_lastTransit);
            m_jitterSeconds += (d - m_jitterSeconds) / 16.0;
        }
        m_hasLast = true;
        m_lastTransit = transit;

        while (!m_window.empty() && m_window.back().transit >= transit)
        {
            m_window.pop_back();
        }
        m_window.push_back({ arrival, transit });

        while (m_window.size() > 1 && m_window.front().arrival + m_windowLength < arrival)
        {
            m_window.pop_front();
        }
    }

    double TransitEstimator::BaseTransitSeconds() const
    {
        return m_window.empty() ? 0.0 : m_window.front().transit;
    }

    Clock::time_point TransitEstimator::LocalTimeOf(int64_t extRtpTimestamp, uint32_t clockRate) const
    {
        if (!m_hasEpoch || clockRate == 0)
        {
            return Clock::now();
        }
        double const mediaSeconds = static_cast<double>(extRtpTimestamp - m_baseTimestamp) / clockRate;
        auto const offset = std::chrono::duration<double>(mediaSeconds + BaseTransitSeconds());
        return m_epoch + std::chrono::duration_cast<Clock::duration>(offset);
    }

    int32_t ComputeAudioTargetDelayMs(AvSyncInputs const& inputs)
    {
        double const videoLatency = inputs.videoPipelineLatencyMs + inputs.videoReceiveToSubmitMs;
        double delay = videoLatency - inputs.audioOutputLatencyMs + inputs.userOffsetMs;

        double const jitterFloor = std::max(inputs.minDelayMs, 2.0 * inputs.audioJitterMs + 10.0);
        delay = std::clamp(delay, jitterFloor, std::max(jitterFloor, inputs.maxDelayMs));
        return static_cast<int32_t>(std::lround(delay));
    }
}
