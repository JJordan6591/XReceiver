#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "MediaClock.h"

namespace rx
{
    // Audio target delay shared by the UI thread (writer) and the audio quantum (reader). Set()
    // is ordinary movement that drift correction follows slowly. Step() is a manual A/V offset
    // change: it also advances a generation, so the audio thread applies it on its next quantum.
    // Target and generation share one lock-free word, so a reader never sees one without the other.
    class AudioTargetDelay
    {
    public:
        static constexpr int32_t kMaxMs = 1000;    // the ring holds one second

        struct Sample
        {
            int32_t targetMs = 0;
            uint32_t generation = 0;
        };

        explicit AudioTargetDelay(int32_t targetMs) : m_packed(Pack({ Clamp(targetMs), 0 })) {}

        void Set(int32_t ms) { Store(ms, false); }
        void Step(int32_t ms) { Store(ms, true); }
        Sample Load() const { return Unpack(m_packed.load(std::memory_order_acquire)); }

    private:
        static int32_t Clamp(int32_t ms) { return std::clamp(ms, 0, kMaxMs); }
        static uint64_t Pack(Sample s) { return (uint64_t{ s.generation } << 32) | static_cast<uint32_t>(s.targetMs); }
        static Sample Unpack(uint64_t v) { return { static_cast<int32_t>(static_cast<uint32_t>(v)), static_cast<uint32_t>(v >> 32) }; }

        void Store(int32_t ms, bool step)
        {
            uint64_t current = m_packed.load(std::memory_order_relaxed);
            for (;;)
            {
                Sample next = Unpack(current);
                next.targetMs = Clamp(ms);
                next.generation += step ? 1u : 0u;
                if (m_packed.compare_exchange_weak(current, Pack(next), std::memory_order_release, std::memory_order_relaxed))
                {
                    return;
                }
            }
        }

        std::atomic<uint64_t> m_packed;
    };

    // Audio-thread half of a manual step. A later target (audio later) is reached by inserting
    // that much silence before playout continues; an earlier target (audio earlier) by discarding
    // that much buffered input at once. Steps that arrive before the previous one finishes are
    // combined, so the result always matches the latest target. No allocation, no locks.
    class ManualDelayStep
    {
    public:
        // Forgets pending work, e.g. when playout restarts and primes to the current target.
        void Reset()
        {
            m_hasApplied = false;
            m_pendingHoldMs = 0.0;
        }

        // Once per quantum before the fill is measured. Returns input frames to discard now.
        // While priming, playout waits for the new target by itself, so nothing is scheduled.
        size_t Update(AudioTargetDelay::Sample sample, bool primed, double inputFramesPerMs)
        {
            bool const stepped = m_hasApplied && sample.generation != m_generation;
            double const deltaMs = stepped ? static_cast<double>(sample.targetMs) - m_appliedMs : 0.0;
            m_hasApplied = true;
            m_generation = sample.generation;
            m_appliedMs = sample.targetMs;
            if (!primed)
            {
                m_pendingHoldMs = 0.0;
                return 0;
            }

            double const limit = static_cast<double>(AudioTargetDelay::kMaxMs);
            m_pendingHoldMs = std::clamp(m_pendingHoldMs + deltaMs, -limit, limit);
            if (m_pendingHoldMs >= 0.0)
            {
                return 0;
            }
            size_t const discard = static_cast<size_t>(std::llround(-m_pendingHoldMs * inputFramesPerMs));
            m_pendingHoldMs = 0.0;
            return discard;
        }

        // Output frames of silence to emit at the start of this quantum, at most `frames`.
        size_t TakeHold(size_t frames, double outputFramesPerMs)
        {
            if (m_pendingHoldMs <= 0.0 || outputFramesPerMs <= 0.0)
            {
                return 0;
            }
            size_t const wanted = static_cast<size_t>(std::llround(m_pendingHoldMs * outputFramesPerMs));
            size_t const hold = std::min(frames, wanted);
            m_pendingHoldMs = hold == wanted ? 0.0 : m_pendingHoldMs - static_cast<double>(hold) / outputFramesPerMs;
            return hold;
        }

        // Silence still owed. The drift loop adds it to the measured fill so a step in progress
        // does not look like clock drift.
        double PendingHoldMs() const { return m_pendingHoldMs; }

    private:
        bool m_hasApplied = false;
        uint32_t m_generation = 0;
        int32_t m_appliedMs = 0;
        double m_pendingHoldMs = 0.0;
    };

    // Slow clock-drift trim of the resampling ratio. A buffer error beyond the deadband for more
    // than a second nudges the ratio by at most 0.5 %; the trim releases once the error settles.
    class DriftTrim
    {
    public:
        static constexpr double kDeadbandMs = 10.0;
        static constexpr double kReleaseMs = 3.0;
        static constexpr double kMaxCorrection = 0.005;    // 0.5 %
        static constexpr double kCorrectionPerMs = 0.0001; // 10 ms error -> 0.1 %

        void Reset()
        {
            m_errorEmaMs = 0.0;
            m_errorSustained = false;
            m_correction = 0.0;
        }

        // Once per quantum with the fill error in milliseconds. Returns the ratio correction.
        double Update(double errorMs, Clock::time_point now)
        {
            m_errorEmaMs += 0.02 * (errorMs - m_errorEmaMs);
            if (std::fabs(m_errorEmaMs) > kDeadbandMs)
            {
                if (!m_errorSustained)
                {
                    m_errorSustained = true;
                    m_errorSince = now;
                }
                else if (now - m_errorSince > std::chrono::seconds(1))
                {
                    m_correction = std::clamp(m_errorEmaMs * kCorrectionPerMs, -kMaxCorrection, kMaxCorrection);
                }
            }
            else if (std::fabs(m_errorEmaMs) < kReleaseMs)
            {
                m_errorSustained = false;
                m_correction = 0.0;
            }
            return m_correction;
        }

        double Correction() const { return m_correction; }

    private:
        double m_errorEmaMs = 0.0;
        bool m_errorSustained = false;
        Clock::time_point m_errorSince{};
        double m_correction = 0.0;
    };
}
