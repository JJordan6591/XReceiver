#include "pch.h"
#include "RtpPacket.h"

#include "Checked.h"

namespace rx
{
    RtpParseResult ParseRtpPacket(uint8_t const* data, size_t size, RtpPacketView& out)
    {
        if (data == nullptr || size < kRtpHeaderSize)
        {
            return RtpParseResult::TooShort;
        }

        uint8_t const b0 = data[0];
        if ((b0 >> 6) != 2)
        {
            return RtpParseResult::BadVersion;
        }

        bool const hasPadding = (b0 & 0x20) != 0;
        bool const hasExtension = (b0 & 0x10) != 0;
        size_t const csrcCount = b0 & 0x0F;

        uint8_t const b1 = data[1];
        out.marker = (b1 & 0x80) != 0;
        out.payloadType = static_cast<uint8_t>(b1 & 0x7F);
        out.sequence = static_cast<uint16_t>((data[2] << 8) | data[3]);
        out.timestamp = (static_cast<uint32_t>(data[4]) << 24) | (static_cast<uint32_t>(data[5]) << 16) |
                        (static_cast<uint32_t>(data[6]) << 8) | static_cast<uint32_t>(data[7]);
        out.ssrc = (static_cast<uint32_t>(data[8]) << 24) | (static_cast<uint32_t>(data[9]) << 16) |
                   (static_cast<uint32_t>(data[10]) << 8) | static_cast<uint32_t>(data[11]);

        size_t csrcBytes = 0;
        if (!CheckedMul(csrcCount, 4, csrcBytes))
        {
            return RtpParseResult::BadCsrc;
        }
        size_t offset = 0;
        if (!CheckedAdd(kRtpHeaderSize, csrcBytes, offset) || offset > size)
        {
            return RtpParseResult::BadCsrc;
        }

        if (hasExtension)
        {
            size_t extensionHeader = 0;
            if (!CheckedAdd(offset, 4, extensionHeader) || extensionHeader > size)
            {
                return RtpParseResult::BadExtension;
            }
            size_t const extensionWords = (static_cast<size_t>(data[offset + 2]) << 8) | data[offset + 3];
            size_t extensionBytes = 0;
            if (!CheckedMul(extensionWords, 4, extensionBytes) || !CheckedAdd(extensionHeader, extensionBytes, offset) || offset > size)
            {
                return RtpParseResult::BadExtension;
            }
        }

        size_t end = size;
        if (hasPadding)
        {
            if (end <= offset)
            {
                return RtpParseResult::BadPadding;
            }
            size_t const padding = data[size - 1];
            // Padding is measured from the end of the datagram and must stay inside the payload.
            if (padding == 0 || padding > end - offset)
            {
                return RtpParseResult::BadPadding;
            }
            end -= padding;
        }

        if (end <= offset)
        {
            return RtpParseResult::EmptyPayload;
        }

        out.payload = data + offset;
        out.payloadSize = end - offset;
        return RtpParseResult::Ok;
    }
}
