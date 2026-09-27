#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "MediaClock.h"

namespace rx
{
    namespace h264
    {
        constexpr uint8_t kNalSliceNonIdr = 1;
        constexpr uint8_t kNalSlicePartitionC = 4;
        constexpr uint8_t kNalSliceIdr = 5;
        constexpr uint8_t kNalSei = 6;
        constexpr uint8_t kNalSps = 7;
        constexpr uint8_t kNalPps = 8;
        constexpr uint8_t kNalAud = 9;
        constexpr uint8_t kNalStapA = 24;
        constexpr uint8_t kNalFuA = 28;

        constexpr uint8_t kStartCode[4] = { 0, 0, 0, 1 };
    }

    struct NalRef
    {
        uint32_t offset = 0;    // offset of the NAL header byte inside AccessUnit::data
        uint32_t size = 0;      // NAL size including the header byte, excluding the start code
        uint8_t type = 0;
    };

    // One coded picture in Annex B byte-stream form (4-byte start codes), keyed by SSRC and
    // extended RTP timestamp.
    struct AccessUnit
    {
        std::vector<uint8_t> data;
        std::vector<NalRef> nals;
        uint32_t ssrc = 0;
        int64_t rtpTimestamp = 0;
        int64_t firstSequence = 0;
        int64_t lastSequence = 0;
        Clock::time_point firstPacketTime{};
        Clock::time_point completeTime{};

        bool isIdr = false;
        bool hasSps = false;
        bool hasPps = false;
        bool hasSlice = false;          // at least one complete slice NAL
        bool sawSlice = false;          // a slice (complete or partial) was seen
        bool allNonRef = true;          // every slice seen has nal_ref_idc == 0

        // Incomplete or damaged. A corrupt AU never carries data or NALs; only its
        // classification flags survive so the gate can decide whether references were lost.
        bool corrupt = false;
        bool lossConfined = true;       // when corrupt: all missing packets belonged to this AU
        bool discontinuity = false;     // first sample after a resync
    };

    using AccessUnitPtr = std::unique_ptr<AccessUnit>;
}
