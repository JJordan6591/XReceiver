#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace rx
{
    std::vector<uint8_t> RemoveEmulationPrevention(uint8_t const* data, size_t size);

    // MSB-first bit reader over an RBSP. Any overrun latches Ok() to false and returns zeros.
    class BitReader
    {
    public:
        BitReader(uint8_t const* data, size_t size) : m_data(data), m_size(size) {}

        uint32_t ReadBits(int count);
        bool ReadFlag() { return ReadBits(1) != 0; }
        uint32_t ReadUe();
        int32_t ReadSe();
        bool Ok() const { return m_ok; }
        void Fail() { m_ok = false; }
        size_t BitsLeft() const { return m_size * 8 > m_bit ? m_size * 8 - m_bit : 0; }

    private:
        uint8_t const* m_data;
        size_t m_size;
        size_t m_bit = 0;
        bool m_ok = true;
    };

    struct SpsInfo
    {
        bool valid = false;
        uint32_t profileIdc = 0;
        uint32_t constraintFlags = 0;
        uint32_t levelIdc = 0;
        uint32_t spsId = 0;
        uint32_t chromaFormatIdc = 1;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t pocType = 0;
        uint32_t maxNumRefFrames = 0;
        bool frameMbsOnly = true;
        bool vuiPresent = false;
        bool timingInfoPresent = false;
        uint32_t numUnitsInTick = 0;
        uint32_t timeScale = 0;
        bool bitstreamRestriction = false;
        uint32_t numReorderFrames = 0;
        uint32_t maxDecFrameBuffering = 0;
    };

    // `nal` starts at the NAL header byte (type 7).
    bool ParseSps(uint8_t const* nal, size_t size, SpsInfo& out);

    // Reads first_mb_in_slice from a slice NAL (types 1-5); returns UINT32_MAX on failure.
    uint32_t ReadFirstMbInSlice(uint8_t const* nal, size_t size);

    // True if the RTP payload starts with an HEVC VPS/SPS/PPS/FU header (layer 0, temporal id 1),
    // byte patterns that are invalid or very unusual as H.264 NAL headers.
    bool LooksLikeHevcPayload(uint8_t const* payload, size_t size);
}
