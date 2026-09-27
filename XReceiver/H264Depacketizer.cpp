#include "pch.h"
#include "H264Depacketizer.h"

#include "Checked.h"

namespace rx
{
    namespace
    {
        bool IsSliceType(uint8_t type)
        {
            return type >= h264::kNalSliceNonIdr && type <= h264::kNalSliceIdr;
        }

        // Types that may appear as a single NAL packet, inside a STAP-A, or inside an FU-A.
        bool IsNalUnitType(uint8_t type)
        {
            return type >= 1 && type <= 23;
        }
    }

    H264Depacketizer::H264Depacketizer(Callback callback) : H264Depacketizer(std::move(callback), Limits{})
    {
    }

    H264Depacketizer::H264Depacketizer(Callback callback, Limits limits) :
        m_callback(std::move(callback)),
        m_limits(limits)
    {
    }

    void H264Depacketizer::Reset()
    {
        m_au.reset();
        m_overflow = false;
        m_hasLastSequence = false;
        m_lastSequence = 0;
        m_headUnverified = false;
        m_firstSliceSeen = false;
        m_firstSliceAtMb0 = false;
        m_fuActive = false;
        m_fuFragments = 0;
        m_fuStartOffset = 0;
    }

    void H264Depacketizer::OnPacket(uint8_t const* payload, size_t size, PacketInfo const& info)
    {
        if (m_hasLastSequence && info.extSequence <= m_lastSequence)
        {
            // A duplicate, or a packet the jitter buffer already gave up on. The AU it belonged
            // to has been resolved without it and must not be reopened.
            ++m_counters.staleOrDuplicate;
            return;
        }

        bool const firstPacket = !m_hasLastSequence;
        bool const gap = m_hasLastSequence && info.extSequence != m_lastSequence + 1;
        if (gap)
        {
            ++m_counters.sequenceGaps;
            m_counters.missingPackets = SaturatingAdd(m_counters.missingPackets,
                static_cast<uint64_t>(info.extSequence - m_lastSequence - 1));
        }
        m_hasLastSequence = true;
        m_lastSequence = info.extSequence;

        if (m_au && (m_au->ssrc != info.ssrc || m_au->rtpTimestamp != info.extTimestamp))
        {
            // The pending AU never received its marker packet. After a gap the missing packets
            // may belong to either side, so neither can be trusted.
            if (!gap)
            {
                ++m_counters.missingMarker;
            }
            MarkCorrupt(false);
            FinishAccessUnit(info.arrival);
        }

        if (m_au)
        {
            if (gap)
            {
                // Same SSRC and timestamp on both sides of the gap: the loss is inside this AU.
                AbortFragment();
                MarkCorrupt(true);
            }
        }
        else
        {
            BeginAccessUnit(info, gap, firstPacket);
        }
        m_au->lastSequence = info.extSequence;

        if (payload == nullptr || size < 1)
        {
            ++m_counters.emptyPayload;
            MarkCorrupt(true);
        }
        else
        {
            uint8_t const type = payload[0] & 0x1F;
            if (IsNalUnitType(type) || type == h264::kNalStapA)
            {
                if (m_fuActive)
                {
                    // A new NAL started before the fragmented one saw its end fragment.
                    ++m_counters.fuaErrors;
                    AbortFragment();
                    MarkCorrupt(true);
                }
                if (type == h264::kNalStapA)
                {
                    HandleStapA(payload, size);
                }
                else
                {
                    AppendNal(payload, size);
                }
            }
            else if (type == h264::kNalFuA)
            {
                HandleFuA(payload, size);
            }
            else
            {
                // STAP-B, MTAP16/24, FU-B and reserved types are not valid in non-interleaved
                // mode; their content cannot be recovered, so the AU is incomplete.
                ++m_counters.unsupportedNal;
                MarkCorrupt(true);
            }
        }

        if (info.marker)
        {
            FinishAccessUnit(info.arrival);
        }
    }

    void H264Depacketizer::BeginAccessUnit(PacketInfo const& info, bool headMissing, bool headUnverified)
    {
        m_au = std::make_unique<AccessUnit>();
        m_au->data.reserve(64 * 1024);
        m_au->nals.reserve(8);
        m_au->ssrc = info.ssrc;
        m_au->rtpTimestamp = info.extTimestamp;
        m_au->firstSequence = info.extSequence;
        m_au->lastSequence = info.extSequence;
        m_au->firstPacketTime = info.arrival;
        m_overflow = false;
        m_fuActive = false;
        m_fuFragments = 0;
        m_headUnverified = headUnverified;
        m_firstSliceSeen = false;
        m_firstSliceAtMb0 = false;
        if (headMissing)
        {
            MarkCorrupt(false);
        }
    }

    void H264Depacketizer::FinishAccessUnit(Clock::time_point now)
    {
        if (!m_au)
        {
            return;
        }
        if (m_fuActive)
        {
            // Marker packet without the FU-A end fragment.
            if (!m_au->corrupt)
            {
                ++m_counters.fuaErrors;
            }
            AbortFragment();
            MarkCorrupt(true);
        }
        if (m_overflow)
        {
            MarkCorrupt(true);
        }
        if (m_headUnverified && !m_firstSliceAtMb0)
        {
            // Joined mid-stream and cannot prove the picture's first slice was received.
            MarkCorrupt(false);
        }

        m_au->completeTime = now;
        if (m_au->corrupt)
        {
            ++m_counters.incompleteAccessUnits;
            m_au->data.clear();
            m_au->data.shrink_to_fit();
            m_au->nals.clear();
            m_au->hasSlice = false;
            m_au->hasSps = false;
            m_au->hasPps = false;
        }
        else
        {
            ++m_counters.completeAccessUnits;
        }

        AccessUnitPtr finished = std::move(m_au);
        m_au.reset();
        m_overflow = false;
        m_fuActive = false;
        m_fuFragments = 0;
        m_headUnverified = false;
        if (m_callback)
        {
            m_callback(std::move(finished));
        }
    }

    void H264Depacketizer::MarkCorrupt(bool confined)
    {
        if (!m_au)
        {
            return;
        }
        m_au->corrupt = true;
        if (!confined)
        {
            m_au->lossConfined = false;
        }
    }

    bool H264Depacketizer::Fits(size_t additionalBytes)
    {
        if (m_overflow)
        {
            return false;
        }
        size_t needed = 0;
        if (!CheckedAdd(m_au->data.size(), additionalBytes, needed) || needed > m_limits.maxAuBytes)
        {
            ++m_counters.oversize;
            m_overflow = true;
            m_fuActive = false;
            m_au->data.clear();
            m_au->data.shrink_to_fit();
            m_au->nals.clear();
            MarkCorrupt(true);
            return false;
        }
        return true;
    }

    void H264Depacketizer::AppendNal(uint8_t const* nal, size_t size)
    {
        if ((nal[0] & 0x80) != 0)
        {
            ++m_counters.forbiddenBit;
            MarkCorrupt(true);
            return;
        }
        if (size > m_limits.maxNalBytes || m_au->nals.size() >= m_limits.maxNals)
        {
            ++m_counters.oversize;
            MarkCorrupt(true);
            return;
        }
        uint8_t const type = nal[0] & 0x1F;
        if (type == h264::kNalAud)
        {
            return;
        }
        if (IsSliceType(type))
        {
            NoteSliceStart(nal[0], nal + 1, size - 1);
        }
        if (!Fits(sizeof(h264::kStartCode) + size))
        {
            return;
        }

        auto& data = m_au->data;
        data.insert(data.end(), std::begin(h264::kStartCode), std::end(h264::kStartCode));
        NalRef ref;
        ref.offset = static_cast<uint32_t>(data.size());
        ref.size = static_cast<uint32_t>(size);
        ref.type = type;
        data.insert(data.end(), nal, nal + size);
        m_au->nals.push_back(ref);
        Classify(ref);
    }

    void H264Depacketizer::NoteSliceStart(uint8_t header, uint8_t const* sliceData, size_t sliceSize)
    {
        AccessUnit& au = *m_au;
        au.sawSlice = true;
        if (((header >> 5) & 0x03) != 0)
        {
            au.allNonRef = false;
        }
        if ((header & 0x1F) == h264::kNalSliceIdr)
        {
            au.isIdr = true;
        }
        if (!m_firstSliceSeen)
        {
            // first_mb_in_slice is the slice header's first ue(v); a leading 1 bit encodes 0.
            m_firstSliceSeen = true;
            m_firstSliceAtMb0 = sliceSize > 0 && (sliceData[0] & 0x80) != 0;
        }
    }

    void H264Depacketizer::Classify(NalRef const& ref)
    {
        AccessUnit& au = *m_au;
        if (IsSliceType(ref.type))
        {
            au.hasSlice = true;
        }
        else if (ref.type == h264::kNalSps)
        {
            au.hasSps = true;
        }
        else if (ref.type == h264::kNalPps)
        {
            au.hasPps = true;
        }
    }

    void H264Depacketizer::HandleStapA(uint8_t const* payload, size_t size)
    {
        // Validate every aggregated unit before appending any of them.
        bool valid = size >= 4;
        size_t offset = 1;
        size_t members = 0;
        while (valid && offset < size)
        {
            size_t lengthEnd = 0;
            if (!CheckedAdd(offset, 2, lengthEnd) || lengthEnd > size)
            {
                valid = false;
                break;
            }
            size_t const nalSize = (static_cast<size_t>(payload[offset]) << 8) | payload[offset + 1];
            offset = lengthEnd;
            size_t nalEnd = 0;
            if (nalSize == 0 || !CheckedAdd(offset, nalSize, nalEnd) || nalEnd > size || !IsNalUnitType(payload[offset] & 0x1F) ||
                ++members > m_limits.maxStapNals)
            {
                valid = false;
                break;
            }
            offset = nalEnd;
        }
        if (!valid)
        {
            ++m_counters.stapaErrors;
            MarkCorrupt(true);
            return;
        }

        offset = 1;
        while (offset < size)
        {
            size_t const nalSize = (static_cast<size_t>(payload[offset]) << 8) | payload[offset + 1];
            offset += 2;
            AppendNal(payload + offset, nalSize);
            offset += nalSize;
        }
    }

    void H264Depacketizer::HandleFuA(uint8_t const* payload, size_t size)
    {
        auto reject = [this](uint64_t& counter)
        {
            ++counter;
            AbortFragment();
            MarkCorrupt(true);
        };

        if (size < 3)
        {
            reject(m_counters.fuaErrors);
            return;
        }

        uint8_t const indicator = payload[0];
        uint8_t const fuHeader = payload[1];
        bool const start = (fuHeader & 0x80) != 0;
        bool const end = (fuHeader & 0x40) != 0;
        uint8_t const nalType = fuHeader & 0x1F;
        uint8_t const nri = indicator & 0x60;
        uint8_t const* fragment = payload + 2;
        size_t const fragmentSize = size - 2;

        if ((indicator & 0x80) != 0)
        {
            reject(m_counters.forbiddenBit);
            return;
        }
        if ((start && end) || !IsNalUnitType(nalType) || fragmentSize == 0)
        {
            // RFC 6184 5.8: a NAL unit must not be sent as a single FU, and FU-A cannot carry
            // aggregation or fragmentation units. An empty fragment is not a valid piece.
            reject(m_counters.fuaErrors);
            return;
        }
        if (m_fuFragments >= m_limits.maxFuFragments)
        {
            reject(m_counters.oversize);
            return;
        }
        ++m_fuFragments;

        if (start)
        {
            if (m_fuActive)
            {
                // The previous fragmented NAL never received its end fragment.
                reject(m_counters.fuaErrors);
            }
            if (m_au->nals.size() >= m_limits.maxNals)
            {
                // A fragmented NAL counts once toward the per-AU cap, like a single or STAP-A NAL.
                reject(m_counters.oversize);
                return;
            }
            m_fuFragments = 1;
            uint8_t const header = static_cast<uint8_t>(nri | nalType);
            if (IsSliceType(nalType))
            {
                NoteSliceStart(header, fragment, fragmentSize);
            }
            size_t headerAndBody = 0;
            if (!CheckedAdd(sizeof(h264::kStartCode) + 1, fragmentSize, headerAndBody) || !Fits(headerAndBody))
            {
                return;
            }
            auto& data = m_au->data;
            m_fuStartOffset = data.size();
            data.insert(data.end(), std::begin(h264::kStartCode), std::end(h264::kStartCode));
            data.push_back(header);
            data.insert(data.end(), fragment, fragment + fragmentSize);
            m_fuActive = true;
            m_fuType = nalType;
            m_fuNri = nri;
            return;
        }

        if (!m_fuActive)
        {
            // The start fragment was lost or rejected; this NAL is unusable. Only count it as a
            // protocol error when no sequence gap already explains it.
            if (!m_au->corrupt)
            {
                ++m_counters.fuaErrors;
            }
            MarkCorrupt(true);
            return;
        }
        if (nalType != m_fuType || nri != m_fuNri)
        {
            reject(m_counters.fuaErrors);
            return;
        }
        size_t const nalSoFar = m_au->data.size() - (m_fuStartOffset + sizeof(h264::kStartCode));
        size_t nalBytes = 0;
        if (!CheckedAdd(nalSoFar, fragmentSize, nalBytes) || nalBytes > m_limits.maxNalBytes)
        {
            reject(m_counters.oversize);
            return;
        }
        if (!Fits(fragmentSize))
        {
            return;
        }
        m_au->data.insert(m_au->data.end(), fragment, fragment + fragmentSize);

        if (end)
        {
            CompleteFragment();
        }
    }

    void H264Depacketizer::CompleteFragment()
    {
        if (!m_fuActive)
        {
            return;
        }
        m_fuActive = false;
        m_fuFragments = 0;

        NalRef ref;
        ref.offset = static_cast<uint32_t>(m_fuStartOffset + sizeof(h264::kStartCode));
        ref.size = static_cast<uint32_t>(m_au->data.size() - ref.offset);
        ref.type = m_au->data[ref.offset] & 0x1F;
        if (ref.type == h264::kNalAud)
        {
            m_au->data.resize(m_fuStartOffset);
            return;
        }
        m_au->nals.push_back(ref);
        Classify(ref);
    }

    void H264Depacketizer::AbortFragment()
    {
        if (m_fuActive && m_au)
        {
            m_au->data.resize(m_fuStartOffset);
        }
        m_fuActive = false;
        m_fuFragments = 0;
    }
}
