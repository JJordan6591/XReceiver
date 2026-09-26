#include "pch.h"
#include "AudioReceiver.h"

#include <algorithm>

#include "DebugLog.h"

using namespace winrt;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Networking::Sockets;
using namespace winrt::Windows::Storage::Streams;

namespace rx
{
    namespace
    {
        constexpr int64_t kMaxConcealFrames = kL16SampleRate / 5;       // 200 ms
        constexpr size_t kFadeFrames = kL16SampleRate * 3 / 1000;       // 3 ms
        constexpr size_t kMaxPacketBytes = 2048;
        constexpr size_t kConvertFrames = 1024;

        JitterBuffer::Config MakeJitterConfig(ReceiverSettings const& s)
        {
            JitterBuffer::Config config;
            config.reorderWindow = s.audioReorderWindow;
            config.capacity = static_cast<size_t>(s.audioReorderWindow) * 2;
            config.timeout = FromMs(s.audioReorderTimeoutMs);
            config.maxPacketBytes = kMaxPacketBytes;
            return config;
        }

        RtpAdmissionRules MakeAdmissionRules(ReceiverSettings const& s)
        {
            RtpAdmissionRules rules;
            rules.payloadType = s.audioPayloadType;
            rules.maxPacketBytes = kMaxPacketBytes;
            rules.minPayloadBytes = kL16BytesPerFrame;
            rules.takeoverAfterSilence = FromMs(s.ssrcTakeoverMs);
            return rules;
        }
    }

    AudioReceiver::AudioReceiver(ReceiverSettings const& settings, std::shared_ptr<ReceiverStats> stats,
                                 std::shared_ptr<PcmRingBuffer> ring) :
        m_settings(settings),
        m_stats(std::move(stats)),
        m_ring(std::move(ring)),
        m_admission(MakeAdmissionRules(settings)),
        m_jitter(MakeJitterConfig(settings)),
        m_convert(kConvertFrames * kL16Channels, 0.0f)
    {
    }

    AudioReceiver::~AudioReceiver()
    {
        m_messageRevoker.revoke();
        if (m_socket)
        {
            try
            {
                m_socket.Close();
            }
            catch (hresult_error const&)
            {
            }
        }
    }

    IAsyncAction AudioReceiver::StartAsync(uint16_t port)
    {
        auto strong = shared_from_this();

        DatagramSocket socket;
        try
        {
            socket.Control().InboundBufferSizeInBytes(256 * 1024);
        }
        catch (hresult_error const&)
        {
        }
        try
        {
            socket.Control().QualityOfService(SocketQualityOfService::LowLatency);
        }
        catch (hresult_error const&)
        {
        }

        std::weak_ptr<AudioReceiver> weak = strong;
        m_messageRevoker = socket.MessageReceived(auto_revoke,
            [weak](DatagramSocket const&, DatagramSocketMessageReceivedEventArgs const& args)
            {
                if (auto self = weak.lock())
                {
                    self->OnMessage(args);
                }
            });
        m_socket = socket;

        co_await socket.BindServiceNameAsync(to_hstring(port));
        Log(L"audio: bound UDP %u", static_cast<unsigned>(port));
    }

    void AudioReceiver::Stop()
    {
        m_stopped = true;
        m_messageRevoker.revoke();
        if (m_socket)
        {
            try
            {
                m_socket.Close();
            }
            catch (hresult_error const&)
            {
            }
            m_socket = nullptr;
        }
    }

    void AudioReceiver::OnMessage(DatagramSocketMessageReceivedEventArgs const& args)
    {
        if (m_stopped)
        {
            return;
        }
        auto const now = Clock::now();
        try
        {
            auto reader = args.GetDataReader();
            uint32_t const length = reader.UnconsumedBufferLength();
            if (length == 0)
            {
                return;
            }
            IBuffer buffer = reader.ReadBuffer(length);
            ProcessDatagram(buffer.data(), length, now);
        }
        catch (hresult_error const&)
        {
            m_stats->Add(Stat::AudioSocketErrors);
        }
    }

    void AudioReceiver::ProcessDatagram(uint8_t const* data, size_t size, Clock::time_point now)
    {
        std::lock_guard<std::mutex> lock(m_lock);
        if (m_stopped)
        {
            return;
        }
        m_stats->Add(Stat::AudioPackets);
        m_stats->Add(Stat::AudioBytes, static_cast<int64_t>(size));

        RtpPacketView packet;
        switch (AdmitRtpPacket(data, size, m_admission, m_tracker, now, packet))
        {
        case RtpAdmission::Invalid:
        case RtpAdmission::TooLarge:
        case RtpAdmission::TooShort:
            m_stats->Add(Stat::AudioInvalid);
            return;
        case RtpAdmission::WrongPayloadType:
            m_stats->Add(Stat::AudioWrongPayloadType);
            return;
        case RtpAdmission::Ignored:
            m_stats->Add(Stat::AudioForeignSsrc);
            return;
        case RtpAdmission::NewStream:
            HandleNewStreamLocked();
            break;
        case RtpAdmission::Accept:
            break;
        }

        // Rejected datagrams must not keep the session in Receiving or reset the idle timer.
        m_lastPacketTicks = now.time_since_epoch().count();
        m_everReceived = true;

        m_stats->Set(Stat::AudioSsrc, packet.ssrc);
        int64_t const extSequence = m_sequence.Unwrap(packet.sequence);
        switch (m_jitter.Insert(data, size, packet, extSequence, now, *this))
        {
        case JitterBuffer::InsertResult::DeliveredReordered:
            m_stats->Add(Stat::AudioReordered);
            break;
        case JitterBuffer::InsertResult::Duplicate:
            m_stats->Add(Stat::AudioDuplicate);
            break;
        case JitterBuffer::InsertResult::Late:
            m_stats->Add(Stat::AudioLate);
            break;
        case JitterBuffer::InsertResult::TooLarge:
            m_stats->Add(Stat::AudioInvalid);
            break;
        default:
            break;
        }
    }

    void AudioReceiver::HandleNewStreamLocked()
    {
        if (m_hadStream)
        {
            m_stats->Add(Stat::AudioStreamRestarts);
            Log(L"audio: stream restart detected (SSRC %08x)", m_tracker.Ssrc());
        }
        m_hadStream = true;
        m_jitter.Reset();
        m_sequence.Reset();
        m_timestamp.Reset();
        m_transit.Reset();
        m_hasExpected = false;
        m_lastSample.fill(0.0f);
        m_fadeInPending = true;
        m_ring->RequestFlush();
    }

    void AudioReceiver::OnOrderedPacket(RtpPacketView const& packet, int64_t, Clock::time_point arrival)
    {
        if (packet.payloadSize < kL16BytesPerFrame)
        {
            m_stats->Add(Stat::AudioInvalid);
            return;
        }
        if (packet.payloadSize % kL16BytesPerFrame != 0)
        {
            m_stats->Add(Stat::AudioPartialFrames);
        }

        int64_t const frames = static_cast<int64_t>(packet.payloadSize / kL16BytesPerFrame);
        int64_t const extTimestamp = m_timestamp.Unwrap(packet.timestamp);
        m_transit.Add(arrival, extTimestamp, kL16SampleRate);

        if (!m_hasExpected)
        {
            m_hasExpected = true;
            m_expectedTimestamp = extTimestamp;
        }

        int64_t skip = 0;
        int64_t const gap = extTimestamp - m_expectedTimestamp;
        if (gap > 0)
        {
            if (gap <= kMaxConcealFrames)
            {
                WriteConcealmentLocked(gap);
            }
            else
            {
                m_stats->Add(Stat::AudioDiscontinuities);
                m_ring->RequestFlush();
                m_fadeInPending = true;
            }
        }
        else if (gap < 0)
        {
            skip = -gap;
            if (skip >= frames)
            {
                m_stats->Add(Stat::AudioLate);
                return;
            }
        }

        uint8_t const* source = packet.payload + skip * kL16BytesPerFrame;
        int64_t remaining = frames - skip;
        size_t fadePosition = 0;
        while (remaining > 0)
        {
            size_t const chunk = static_cast<size_t>(std::min<int64_t>(remaining, kConvertFrames));
            ConvertS16BeToFloat(source, chunk * kL16Channels, m_convert.data());

            if (m_fadeInPending)
            {
                for (size_t f = 0; f < chunk && fadePosition < kFadeFrames; ++f, ++fadePosition)
                {
                    float const gain = static_cast<float>(fadePosition) / kFadeFrames;
                    for (size_t c = 0; c < kL16Channels; ++c)
                    {
                        m_convert[f * kL16Channels + c] *= gain;
                    }
                }
                if (fadePosition >= kFadeFrames)
                {
                    m_fadeInPending = false;
                }
            }

            size_t const written = m_ring->Write(m_convert.data(), chunk);
            if (written < chunk)
            {
                m_stats->Add(Stat::AudioOverflowFrames, static_cast<int64_t>(chunk - written));
            }
            for (size_t c = 0; c < kL16Channels; ++c)
            {
                m_lastSample[c] = m_convert[(chunk - 1) * kL16Channels + c];
            }
            source += chunk * kL16BytesPerFrame;
            remaining -= static_cast<int64_t>(chunk);
        }
        m_fadeInPending = false;
        m_expectedTimestamp = extTimestamp + frames;
    }

    void AudioReceiver::WriteConcealmentLocked(int64_t frames)
    {
        m_stats->Add(Stat::AudioConcealedFrames, frames);
        size_t fadePosition = 0;
        int64_t remaining = frames;
        while (remaining > 0)
        {
            size_t const chunk = static_cast<size_t>(std::min<int64_t>(remaining, kConvertFrames));
            for (size_t f = 0; f < chunk; ++f, ++fadePosition)
            {
                float const gain = fadePosition < kFadeFrames ? 1.0f - static_cast<float>(fadePosition) / kFadeFrames : 0.0f;
                for (size_t c = 0; c < kL16Channels; ++c)
                {
                    m_convert[f * kL16Channels + c] = m_lastSample[c] * gain;
                }
            }
            size_t const written = m_ring->Write(m_convert.data(), chunk);
            if (written < chunk)
            {
                m_stats->Add(Stat::AudioOverflowFrames, static_cast<int64_t>(chunk - written));
            }
            remaining -= static_cast<int64_t>(chunk);
        }
        m_lastSample.fill(0.0f);
        m_fadeInPending = true;
    }

    void AudioReceiver::OnPacketsLost(int64_t count)
    {
        m_stats->Add(Stat::AudioLost, count);
    }

    void AudioReceiver::Poll(Clock::time_point now)
    {
        std::lock_guard<std::mutex> lock(m_lock);
        if (m_stopped)
        {
            return;
        }
        m_jitter.Poll(now, *this);
        m_stats->Set(Stat::AudioJitterUs, static_cast<int64_t>(m_transit.JitterMs() * 1000.0));
    }

    double AudioReceiver::JitterMs() const
    {
        std::lock_guard<std::mutex> lock(m_lock);
        return m_transit.JitterMs();
    }
}
