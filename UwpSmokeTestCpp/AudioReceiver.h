#pragma once

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

#include "JitterBuffer.h"
#include "L16Convert.h"
#include "MediaClock.h"
#include "PcmRingBuffer.h"
#include "ReceiverSettings.h"
#include "ReceiverStats.h"
#include "RtpSequence.h"

namespace rx
{
    // Receives L16 RTP audio, reorders it, conceals gaps and writes float PCM to the ring.
    class AudioReceiver : public std::enable_shared_from_this<AudioReceiver>, private JitterBuffer::Sink
    {
    public:
        AudioReceiver(ReceiverSettings const& settings, std::shared_ptr<ReceiverStats> stats, std::shared_ptr<PcmRingBuffer> ring);
        ~AudioReceiver() override;

        AudioReceiver(AudioReceiver const&) = delete;
        AudioReceiver& operator=(AudioReceiver const&) = delete;

        winrt::Windows::Foundation::IAsyncAction StartAsync(uint16_t port);
        void Stop();
        void Poll(Clock::time_point now);

        bool EverReceived() const { return m_everReceived.load(); }
        Clock::time_point LastPacketTime() const { return Clock::time_point(Clock::duration(m_lastPacketTicks.load())); }
        double JitterMs() const;

        // Exposed for the self-test: processes one datagram as if received from the socket.
        void ProcessDatagram(uint8_t const* data, size_t size, Clock::time_point now);

    private:
        void OnMessage(winrt::Windows::Networking::Sockets::DatagramSocketMessageReceivedEventArgs const& args);
        void HandleNewStreamLocked();
        void WriteConcealmentLocked(int64_t frames);

        // JitterBuffer::Sink
        void OnOrderedPacket(RtpPacketView const& packet, int64_t extSequence, Clock::time_point arrival) override;
        void OnPacketsLost(int64_t count) override;

        ReceiverSettings m_settings;
        std::shared_ptr<ReceiverStats> m_stats;
        std::shared_ptr<PcmRingBuffer> m_ring;

        winrt::Windows::Networking::Sockets::DatagramSocket m_socket{ nullptr };
        winrt::Windows::Networking::Sockets::DatagramSocket::MessageReceived_revoker m_messageRevoker;

        std::atomic<bool> m_stopped{ false };
        std::atomic<bool> m_everReceived{ false };
        std::atomic<Clock::rep> m_lastPacketTicks{ 0 };

        mutable std::mutex m_lock;
        RtpStreamTracker m_tracker;
        SequenceUnwrapper m_sequence;
        TimestampUnwrapper m_timestamp;
        TransitEstimator m_transit;
        JitterBuffer m_jitter;
        bool m_hadStream = false;

        bool m_hasExpected = false;
        int64_t m_expectedTimestamp = 0;
        std::array<float, kL16Channels> m_lastSample{};
        bool m_fadeInPending = false;
        std::vector<float> m_convert;
    };
}
