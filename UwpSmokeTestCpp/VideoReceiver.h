#pragma once

#include <array>
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#include "H264Depacketizer.h"
#include "JitterBuffer.h"
#include "MediaClock.h"
#include "ReceiverSettings.h"
#include "ReceiverStats.h"
#include "RtpSequence.h"
#include "VideoDeliveryCore.h"

namespace rx
{
    struct VideoActivity
    {
        bool everReceived = false;
        Clock::time_point lastPacket{};
        bool everSubmitted = false;
        bool waitingForKeyframe = true;
        bool sourceActive = false;
        bool hevcDetected = false;
    };

    // Receives H.264 RTP on one UDP port and turns it into MediaStreamSource samples.
    // Socket callbacks, the housekeeping timer and Media Foundation callbacks may run
    // concurrently; all pipeline state is guarded by m_lock, and WinRT calls that complete
    // requests are made only after the lock is released. Delivery decisions live in
    // VideoDeliveryCore; this class owns the sockets and the WinRT request objects.
    class VideoReceiver : public std::enable_shared_from_this<VideoReceiver>, private JitterBuffer::Sink
    {
    public:
        using SourceRequestHandler = std::function<void(SpsInfo const& sps, uint64_t sourceId)>;

        VideoReceiver(ReceiverSettings const& settings, std::shared_ptr<ReceiverStats> stats);
        ~VideoReceiver() override;

        VideoReceiver(VideoReceiver const&) = delete;
        VideoReceiver& operator=(VideoReceiver const&) = delete;

        void SetSourceRequestHandler(SourceRequestHandler handler);

        winrt::Windows::Foundation::IAsyncAction StartAsync(uint16_t port);
        void Stop();

        void Poll(Clock::time_point now);

        // MediaStreamSource events for source generation sourceId.
        void OnSourceStarting(uint64_t sourceId, winrt::Windows::Foundation::TimeSpan actualStart);
        void OnSampleRequested(uint64_t sourceId, winrt::Windows::Media::Core::MediaStreamSourceSampleRequest const& request);
        void OnSampleRendered(uint64_t sourceId);
        void OnSourceClosed(uint64_t sourceId, int32_t reason);

        // Diagnostics only: the player's position is never used to stamp samples.
        void UpdatePtsLead(uint64_t sourceId, winrt::Windows::Foundation::TimeSpan position);

        void RequestNewSource();
        void SetLossPolicy(LossPolicy policy);

        VideoActivity Activity() const;

    private:
        struct Pending
        {
            winrt::Windows::Media::Core::MediaStreamSourceSampleRequest request{ nullptr };
            winrt::Windows::Media::Core::MediaStreamSourceSampleRequestDeferral deferral{ nullptr };
            uint64_t sourceId = 0;
            uint64_t requestSerial = 0;
        };

        struct Actions
        {
            std::vector<std::pair<Pending, std::optional<VideoDeliveryCore::Sample>>> completions;
            std::optional<std::pair<SpsInfo, uint64_t>> sourceRequest;
        };

        void OnMessage(winrt::Windows::Networking::Sockets::DatagramSocketMessageReceivedEventArgs const& args);
        void ProcessDatagram(uint8_t const* data, size_t size, Clock::time_point now);
        void RunActions(Actions& actions);
        winrt::Windows::Media::Core::MediaStreamSample CreateSample(VideoDeliveryCore::Sample& sample);
        void SubmitSample(winrt::Windows::Media::Core::MediaStreamSourceSampleRequest const& request,
                          VideoDeliveryCore::Sample& sample, wchar_t const* how);
        void OnSampleProcessed(uint64_t sourceId, uint64_t sampleSerial, int64_t pts);

        // All *Locked methods require m_lock and a non-null m_actions.
        void HandleDatagramLocked(uint8_t const* data, size_t size, Clock::time_point now);
        void HandleNewStreamLocked();
        void OnAccessUnitLocked(AccessUnitPtr au);
        void CollectLocked(VideoDeliveryCore::Output& out, Clock::time_point now);
        void RecordLatencyLocked(VideoDeliveryCore::Sample const& sample, Clock::time_point now);
        void UpdateFormatStatsLocked();
        void UpdateLatencyStatsLocked();
        void PublishDepacketizerStatsLocked();

        // JitterBuffer::Sink
        void OnOrderedPacket(RtpPacketView const& packet, int64_t extSequence, Clock::time_point arrival) override;
        void OnPacketsLost(int64_t count) override;

        ReceiverSettings m_settings;
        std::shared_ptr<ReceiverStats> m_stats;
        SourceRequestHandler m_sourceRequestHandler;

        // UI-thread only.
        winrt::Windows::Networking::Sockets::DatagramSocket m_socket{ nullptr };
        winrt::Windows::Networking::Sockets::DatagramSocket::MessageReceived_revoker m_messageRevoker;

        std::atomic<bool> m_stopped{ false };

        mutable std::mutex m_lock;
        Actions* m_actions = nullptr;

        RtpStreamTracker m_tracker;
        SequenceUnwrapper m_sequence;
        TimestampUnwrapper m_timestamp;
        TransitEstimator m_transit;
        JitterBuffer m_jitter;
        H264Depacketizer m_depacketizer;
        VideoDeliveryCore m_core;
        std::map<uint64_t, Pending> m_pending;   // retained requests by serial
        uint64_t m_requestSerial = 0;

        bool m_hadStream = false;
        bool m_everReceived = false;
        Clock::time_point m_lastPacket{};
        int m_hevcHits = 0;
        bool m_hevcDetected = false;
        bool m_hasLastIdr = false;
        Clock::time_point m_lastIdr{};

        std::array<int64_t, 128> m_latencyUs{};
        size_t m_latencyCount = 0;
        size_t m_latencyNext = 0;
        uint32_t m_pollCount = 0;
    };
}
