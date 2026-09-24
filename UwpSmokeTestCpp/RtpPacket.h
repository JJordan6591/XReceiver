#pragma once

#include <cstddef>
#include <cstdint>

namespace rx
{
    // Non-owning view of a parsed RTP packet. Pointers refer to the caller's buffer.
    struct RtpPacketView
    {
        uint8_t payloadType = 0;
        bool marker = false;
        uint16_t sequence = 0;
        uint32_t timestamp = 0;
        uint32_t ssrc = 0;
        uint8_t const* payload = nullptr;
        size_t payloadSize = 0;
    };

    enum class RtpParseResult
    {
        Ok,
        TooShort,
        BadVersion,
        BadCsrc,
        BadExtension,
        BadPadding,
        EmptyPayload,
    };

    constexpr size_t kRtpHeaderSize = 12;

    RtpParseResult ParseRtpPacket(uint8_t const* data, size_t size, RtpPacketView& out);
}
