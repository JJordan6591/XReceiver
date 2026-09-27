#include "pch.h"
#include "H264KeyframeGate.h"

namespace rx
{
    void H264KeyframeGate::Reset()
    {
        m_waiting = true;
        m_sps.clear();
        m_pps.clear();
        m_spsInfo = SpsInfo{};
    }

    void H264KeyframeGate::EnterWaiting()
    {
        m_waiting = true;
    }

    void H264KeyframeGate::OnReferenceLost()
    {
        if (m_config.policy == LossPolicy::Strict)
        {
            m_waiting = true;
        }
    }

    void H264KeyframeGate::MarkUnprimed()
    {
        m_waiting = true;
    }

    H264KeyframeGate::Result H264KeyframeGate::Process(AccessUnit& au)
    {
        Result result;

        if (au.corrupt)
        {
            // All slices of a picture share nal_ref_idc, so one non-reference slice proves the
            // whole picture is non-reference. Only then is the reference chain intact.
            bool const nonReference = au.lossConfined && au.sawSlice && au.allNonRef;
            if (nonReference)
            {
                result.decision = Decision::DropNonRef;
                return result;
            }
            if (!m_waiting)
            {
                result.referenceLost = true;
                OnReferenceLost();
                result.enteredWaiting = m_waiting;
            }
            result.decision = Decision::DropIncomplete;
            return result;
        }

        CacheParameterSets(au, result);

        if (!au.hasSlice)
        {
            result.decision = Decision::DropNoSlice;
            return result;
        }

        if (result.formatChanged && !au.isIdr)
        {
            result.enteredWaiting = !m_waiting;
            m_waiting = true;
            result.decision = Decision::DropAwaitingIdr;
            return result;
        }

        if (m_waiting)
        {
            if (au.isIdr && HasParameterSets())
            {
                InjectParameterSets(au);
                m_waiting = false;
                au.discontinuity = true;
                result.resynced = true;
                result.decision = Decision::Submit;
                return result;
            }
            result.decision = Decision::DropAwaitingIdr;
            return result;
        }

        if (au.isIdr)
        {
            InjectParameterSets(au);
        }
        result.decision = Decision::Submit;
        return result;
    }

    void H264KeyframeGate::CacheParameterSets(AccessUnit const& au, Result& result)
    {
        for (NalRef const& ref : au.nals)
        {
            uint8_t const* nal = au.data.data() + ref.offset;
            if (ref.type == h264::kNalSps)
            {
                SpsInfo info;
                if (ParseSps(nal, ref.size, info))
                {
                    if (m_spsInfo.valid && (info.width != m_spsInfo.width || info.height != m_spsInfo.height))
                    {
                        result.formatChanged = true;
                    }
                    m_sps.assign(nal, nal + ref.size);
                    m_spsInfo = info;
                }
            }
            else if (ref.type == h264::kNalPps)
            {
                m_pps.assign(nal, nal + ref.size);
            }
        }
    }

    void H264KeyframeGate::InjectParameterSets(AccessUnit& au) const
    {
        if ((au.hasSps && au.hasPps) || !HasParameterSets())
        {
            return;
        }

        std::vector<uint8_t> prefix;
        std::vector<NalRef> prefixRefs;
        auto appendNal = [&](std::vector<uint8_t> const& nal, uint8_t type)
        {
            prefix.insert(prefix.end(), std::begin(h264::kStartCode), std::end(h264::kStartCode));
            NalRef ref;
            ref.offset = static_cast<uint32_t>(prefix.size());
            ref.size = static_cast<uint32_t>(nal.size());
            ref.type = type;
            prefix.insert(prefix.end(), nal.begin(), nal.end());
            prefixRefs.push_back(ref);
        };

        if (!au.hasSps)
        {
            appendNal(m_sps, h264::kNalSps);
        }
        if (!au.hasPps)
        {
            appendNal(m_pps, h264::kNalPps);
        }

        uint32_t const shift = static_cast<uint32_t>(prefix.size());
        for (NalRef& ref : au.nals)
        {
            ref.offset += shift;
        }
        au.data.insert(au.data.begin(), prefix.begin(), prefix.end());
        au.nals.insert(au.nals.begin(), prefixRefs.begin(), prefixRefs.end());
        au.hasSps = true;
        au.hasPps = true;
    }
}
