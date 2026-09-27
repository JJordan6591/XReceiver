#include "pch.h"
#include "H264Bitstream.h"

#include "Checked.h"

namespace rx
{
    namespace
    {
        constexpr size_t kMaxSpsBytes = 2 * 1024 * 1024;
    }
    std::vector<uint8_t> RemoveEmulationPrevention(uint8_t const* data, size_t size)
    {
        std::vector<uint8_t> out;
        out.reserve(size);
        int zeros = 0;
        for (size_t i = 0; i < size; ++i)
        {
            uint8_t const b = data[i];
            if (zeros >= 2 && b == 0x03)
            {
                zeros = 0;
                continue;
            }
            out.push_back(b);
            zeros = (b == 0) ? zeros + 1 : 0;
        }
        return out;
    }

    uint32_t BitReader::ReadBits(int count)
    {
        if (count < 0 || count > 32 || m_data == nullptr || m_size > (SIZE_MAX / 8))
        {
            m_ok = false;
            return 0;
        }
        uint32_t value = 0;
        size_t const bitCount = m_size * 8;
        for (int i = 0; i < count; ++i)
        {
            if (m_bit >= bitCount)
            {
                m_ok = false;
                return 0;
            }
            uint8_t const byte = m_data[m_bit >> 3];
            uint32_t const bit = (byte >> (7 - (m_bit & 7))) & 1u;
            value = (value << 1) | bit;
            ++m_bit;
        }
        return value;
    }

    uint32_t BitReader::ReadUe()
    {
        int leadingZeros = 0;
        while (true)
        {
            if (!m_ok)
            {
                return 0;
            }
            if (ReadBits(1) != 0)
            {
                break;
            }
            if (++leadingZeros > 31)
            {
                m_ok = false;
                return 0;
            }
        }
        if (leadingZeros == 0)
        {
            return 0;
        }
        uint32_t const suffix = ReadBits(leadingZeros);
        if (!m_ok)
        {
            return 0;
        }
        uint64_t const value = ((uint64_t{ 1 } << leadingZeros) - 1) + suffix;
        return value > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(value);
    }

    int32_t BitReader::ReadSe()
    {
        uint32_t const k = ReadUe();
        if (k & 1)
        {
            return static_cast<int32_t>((k + 1) / 2);
        }
        return -static_cast<int32_t>(k / 2);
    }

    namespace
    {
        void SkipScalingList(BitReader& br, int size)
        {
            int last = 8;
            int next = 8;
            for (int j = 0; j < size && br.Ok(); ++j)
            {
                if (next != 0)
                {
                    int const delta = br.ReadSe();
                    next = (last + delta + 256) % 256;
                }
                last = (next == 0) ? last : next;
            }
        }

        void SkipHrd(BitReader& br)
        {
            uint32_t const cpbCount = br.ReadUe() + 1;
            if (cpbCount > 32)
            {
                br.Fail();
                return;
            }
            br.ReadBits(4);
            br.ReadBits(4);
            for (uint32_t i = 0; i < cpbCount && br.Ok(); ++i)
            {
                br.ReadUe();
                br.ReadUe();
                br.ReadFlag();
            }
            br.ReadBits(5);
            br.ReadBits(5);
            br.ReadBits(5);
            br.ReadBits(5);
        }

        void ParseVui(BitReader& br, SpsInfo& sps)
        {
            if (br.ReadFlag())
            {
                uint32_t const aspectRatioIdc = br.ReadBits(8);
                if (aspectRatioIdc == 255)
                {
                    br.ReadBits(16);
                    br.ReadBits(16);
                }
            }
            if (br.ReadFlag())
            {
                br.ReadFlag();
            }
            if (br.ReadFlag())
            {
                br.ReadBits(3);
                br.ReadFlag();
                if (br.ReadFlag())
                {
                    br.ReadBits(8);
                    br.ReadBits(8);
                    br.ReadBits(8);
                }
            }
            if (br.ReadFlag())
            {
                br.ReadUe();
                br.ReadUe();
            }
            sps.timingInfoPresent = br.ReadFlag();
            if (sps.timingInfoPresent)
            {
                sps.numUnitsInTick = br.ReadBits(32);
                sps.timeScale = br.ReadBits(32);
                br.ReadFlag();
            }
            bool const nalHrd = br.ReadFlag();
            if (nalHrd)
            {
                SkipHrd(br);
            }
            bool const vclHrd = br.ReadFlag();
            if (vclHrd)
            {
                SkipHrd(br);
            }
            if (nalHrd || vclHrd)
            {
                br.ReadFlag();
            }
            br.ReadFlag();
            bool const restriction = br.ReadFlag();
            if (restriction)
            {
                br.ReadFlag();
                br.ReadUe();
                br.ReadUe();
                br.ReadUe();
                br.ReadUe();
                uint32_t const numReorder = br.ReadUe();
                uint32_t const maxDecBuffering = br.ReadUe();
                if (br.Ok())
                {
                    sps.bitstreamRestriction = true;
                    sps.numReorderFrames = numReorder;
                    sps.maxDecFrameBuffering = maxDecBuffering;
                }
            }
            if (!br.Ok())
            {
                sps.timingInfoPresent = false;
            }
        }
    }

    bool ParseSps(uint8_t const* nal, size_t size, SpsInfo& out)
    {
        out = SpsInfo{};
        if (nal == nullptr || size < 4 || size > kMaxSpsBytes || (nal[0] & 0x1F) != 7)
        {
            return false;
        }

        std::vector<uint8_t> const rbsp = RemoveEmulationPrevention(nal + 1, size - 1);
        BitReader br(rbsp.data(), rbsp.size());

        SpsInfo sps;
        sps.profileIdc = br.ReadBits(8);
        sps.constraintFlags = br.ReadBits(8);
        sps.levelIdc = br.ReadBits(8);
        sps.spsId = br.ReadUe();
        if (sps.spsId > 31)
        {
            return false;
        }

        bool separateColourPlane = false;
        switch (sps.profileIdc)
        {
        case 100: case 110: case 122: case 244: case 44: case 83:
        case 86: case 118: case 128: case 138: case 139: case 134: case 135:
        {
            sps.chromaFormatIdc = br.ReadUe();
            if (sps.chromaFormatIdc > 3)
            {
                return false;
            }
            if (sps.chromaFormatIdc == 3)
            {
                separateColourPlane = br.ReadFlag();
            }
            br.ReadUe(); // bit_depth_luma_minus8
            br.ReadUe(); // bit_depth_chroma_minus8
            br.ReadFlag(); // qpprime_y_zero_transform_bypass_flag
            if (br.ReadFlag()) // seq_scaling_matrix_present_flag
            {
                int const lists = (sps.chromaFormatIdc != 3) ? 8 : 12;
                for (int i = 0; i < lists && br.Ok(); ++i)
                {
                    if (br.ReadFlag())
                    {
                        SkipScalingList(br, i < 6 ? 16 : 64);
                    }
                }
            }
            break;
        }
        default:
            break;
        }

        br.ReadUe(); // log2_max_frame_num_minus4
        sps.pocType = br.ReadUe();
        if (sps.pocType == 0)
        {
            br.ReadUe();
        }
        else if (sps.pocType == 1)
        {
            br.ReadFlag();
            br.ReadSe();
            br.ReadSe();
            uint32_t const cycle = br.ReadUe();
            if (cycle > 255)
            {
                return false;
            }
            for (uint32_t i = 0; i < cycle && br.Ok(); ++i)
            {
                br.ReadSe();
            }
        }
        else if (sps.pocType > 2)
        {
            return false;
        }

        sps.maxNumRefFrames = br.ReadUe();
        br.ReadFlag(); // gaps_in_frame_num_value_allowed_flag
        uint32_t const widthMbs = br.ReadUe() + 1;
        uint32_t const heightMapUnits = br.ReadUe() + 1;
        sps.frameMbsOnly = br.ReadFlag();
        if (!sps.frameMbsOnly)
        {
            br.ReadFlag(); // mb_adaptive_frame_field_flag
        }
        br.ReadFlag(); // direct_8x8_inference_flag

        uint32_t cropLeft = 0, cropRight = 0, cropTop = 0, cropBottom = 0;
        if (br.ReadFlag())
        {
            cropLeft = br.ReadUe();
            cropRight = br.ReadUe();
            cropTop = br.ReadUe();
            cropBottom = br.ReadUe();
        }

        if (!br.Ok() || widthMbs > 1024 || heightMapUnits > 1024)
        {
            return false;
        }

        uint32_t const frameHeightFactor = sps.frameMbsOnly ? 1 : 2;
        uint32_t width = widthMbs * 16;
        uint32_t height = frameHeightFactor * heightMapUnits * 16;

        uint32_t cropUnitX = 1;
        uint32_t cropUnitY = frameHeightFactor;
        if (sps.chromaFormatIdc != 0 && !separateColourPlane)
        {
            uint32_t const subWidth = (sps.chromaFormatIdc == 1 || sps.chromaFormatIdc == 2) ? 2 : 1;
            uint32_t const subHeight = (sps.chromaFormatIdc == 1) ? 2 : 1;
            cropUnitX = subWidth;
            cropUnitY = subHeight * frameHeightFactor;
        }

        uint64_t const cropSumX = uint64_t{ cropLeft } + cropRight;
        uint64_t const cropSumY = uint64_t{ cropTop } + cropBottom;
        if (cropUnitX != 0 && cropSumX > UINT64_MAX / cropUnitX)
        {
            return false;
        }
        if (cropUnitY != 0 && cropSumY > UINT64_MAX / cropUnitY)
        {
            return false;
        }
        uint64_t const cropX = cropSumX * cropUnitX;
        uint64_t const cropY = cropSumY * cropUnitY;
        if (cropX >= width || cropY >= height)
        {
            return false;
        }
        width -= static_cast<uint32_t>(cropX);
        height -= static_cast<uint32_t>(cropY);

        if (width < 16 || height < 16 || width > 8192 || height > 8192)
        {
            return false;
        }

        sps.width = width;
        sps.height = height;

        sps.vuiPresent = br.ReadFlag();
        if (sps.vuiPresent && br.Ok())
        {
            ParseVui(br, sps);
        }

        sps.valid = true;
        out = sps;
        return true;
    }

    uint32_t ReadFirstMbInSlice(uint8_t const* nal, size_t size)
    {
        if (nal == nullptr || size < 2)
        {
            return UINT32_MAX;
        }
        uint8_t const type = nal[0] & 0x1F;
        if (type < 1 || type > 5)
        {
            return UINT32_MAX;
        }
        size_t const probe = size - 1 < 16 ? size - 1 : 16;
        std::vector<uint8_t> const rbsp = RemoveEmulationPrevention(nal + 1, probe);
        BitReader br(rbsp.data(), rbsp.size());
        uint32_t const firstMb = br.ReadUe();
        return br.Ok() ? firstMb : UINT32_MAX;
    }

    bool LooksLikeHevcPayload(uint8_t const* payload, size_t size)
    {
        if (payload == nullptr || size < 3)
        {
            return false;
        }
        uint8_t const b0 = payload[0];
        uint8_t const b1 = payload[1];
        if ((b0 & 0x80) != 0 || b1 != 0x01)
        {
            return false;
        }
        uint8_t const type = (b0 >> 1) & 0x3F;
        bool const layerZero = (b0 & 0x01) == 0;
        return layerZero && (type == 32 || type == 33 || type == 34 || type == 49);
    }
}
