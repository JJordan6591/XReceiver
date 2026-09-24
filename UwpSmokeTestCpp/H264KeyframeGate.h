#pragma once

#include <vector>

#include "H264AccessUnit.h"
#include "H264Bitstream.h"
#include "MediaClock.h"
#include "ReceiverSettings.h"

namespace rx
{
    // Decides which access units may reach the decoder. Only complete AUs are ever submitted.
    // Caches the latest valid SPS/PPS and prepends them to IDRs that lack them.
    //
    // AwaitingIDR is entered on a new stream or decoder, on an SPS format change, and (Strict)
    // whenever a reference picture is lost. While awaiting, every non-IDR AU is discarded until
    // a complete IDR arrives; there is no timeout that resumes on non-IDR pictures.
    class H264KeyframeGate
    {
    public:
        enum class Decision
        {
            Submit,
            DropNoSlice,
            DropIncomplete,
            DropNonRef,
            DropAwaitingIdr,
        };

        struct Result
        {
            Decision decision = Decision::DropNoSlice;
            bool formatChanged = false;
            bool resynced = false;
            bool enteredWaiting = false;
            bool referenceLost = false;
        };

        struct Config
        {
            LossPolicy policy = LossPolicy::Strict;
        };

        void Configure(Config const& config) { m_config = config; }
        void SetPolicy(LossPolicy policy) { m_config.policy = policy; }

        // New stream: forget parameter sets and require an IDR on a fresh decoder.
        void Reset();

        // Strict loss recovery: discard until the next complete IDR, keeping the decoder.
        void EnterWaiting();

        // A reference picture never reached the decoder (damaged or dropped before submit).
        // Strict awaits an IDR; Tolerant keeps decoding complete pictures.
        void OnReferenceLost();

        // The decoder was replaced; the next sample must be an IDR regardless of policy.
        void MarkUnprimed();

        Result Process(AccessUnit& au);

        bool IsWaiting() const { return m_waiting; }
        bool HasParameterSets() const { return !m_sps.empty() && !m_pps.empty(); }
        SpsInfo const& Sps() const { return m_spsInfo; }

    private:
        void CacheParameterSets(AccessUnit const& au, Result& result);
        void InjectParameterSets(AccessUnit& au) const;

        Config m_config;
        bool m_waiting = true;
        std::vector<uint8_t> m_sps;
        std::vector<uint8_t> m_pps;
        SpsInfo m_spsInfo;
    };
}
