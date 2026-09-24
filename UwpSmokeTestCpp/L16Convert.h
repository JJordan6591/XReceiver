#pragma once

#include <cstddef>
#include <cstdint>

namespace rx
{
    constexpr uint32_t kL16SampleRate = 44100;
    constexpr uint32_t kL16Channels = 2;
    constexpr size_t kL16BytesPerFrame = 2 * kL16Channels;

    // RTP L16 (RFC 3551) is signed 16-bit big-endian, interleaved L, R.
    inline int16_t ReadS16Be(uint8_t const* p)
    {
        return static_cast<int16_t>(static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]));
    }

    inline float S16BeToFloat(uint8_t const* p)
    {
        return static_cast<float>(ReadS16Be(p)) * (1.0f / 32768.0f);
    }

    // Converts `sampleCount` samples (not frames). Returns the number written.
    inline size_t ConvertS16BeToFloat(uint8_t const* source, size_t sampleCount, float* destination)
    {
        for (size_t i = 0; i < sampleCount; ++i)
        {
            destination[i] = S16BeToFloat(source + 2 * i);
        }
        return sampleCount;
    }

    // Byte swap for the little-endian 16-bit PCM MediaStreamSource fallback path.
    inline void ConvertS16BeToS16Le(uint8_t const* source, size_t sampleCount, uint8_t* destination)
    {
        for (size_t i = 0; i < sampleCount; ++i)
        {
            destination[2 * i] = source[2 * i + 1];
            destination[2 * i + 1] = source[2 * i];
        }
    }
}
