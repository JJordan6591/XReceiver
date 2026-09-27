#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

#include "MediaClock.h"
#include "ReceiverStats.h"

namespace rx
{
    // Milestone 0a: bare DatagramSockets that only count packets, to prove the manifest
    // capability admits LAN UDP on the Xbox and to measure burst loss at the socket.
    class PocSocketCounter : public std::enable_shared_from_this<PocSocketCounter>
    {
    public:
        winrt::Windows::Foundation::IAsyncAction StartAsync(uint16_t videoPort, uint16_t audioPort);
        void Stop();
        winrt::hstring Status() const;

    private:
        struct Counter
        {
            std::atomic<int64_t> packets{ 0 };
            std::atomic<int64_t> bytes{ 0 };
            std::atomic<int64_t> gaps{ 0 };
            std::atomic<int32_t> lastSequence{ -1 };
        };

        winrt::Windows::Networking::Sockets::DatagramSocket m_video{ nullptr };
        winrt::Windows::Networking::Sockets::DatagramSocket m_audio{ nullptr };
        winrt::Windows::Networking::Sockets::DatagramSocket::MessageReceived_revoker m_videoRevoker;
        winrt::Windows::Networking::Sockets::DatagramSocket::MessageReceived_revoker m_audioRevoker;
        std::shared_ptr<Counter> m_videoCount = std::make_shared<Counter>();
        std::shared_ptr<Counter> m_audioCount = std::make_shared<Counter>();
        winrt::hstring m_error;
    };

    // Milestone 0b: plays Assets\poc_1080p60.h264 (Annex B, looping) through a video-only
    // MediaStreamSource at 60 fps to validate H.264 decode, RealTimePlayback and in-band
    // resolution changes on the Xbox without any networking.
    class PocClipPlayer : public std::enable_shared_from_this<PocClipPlayer>
    {
    public:
        winrt::Windows::Foundation::IAsyncAction StartAsync(winrt::Windows::Media::Playback::MediaPlayer player);
        void Stop();
        void OnUiTick();
        winrt::hstring Status() const;

    private:
        struct ClipFrame
        {
            std::vector<uint8_t> data;
            bool keyframe = false;
        };

        void OnSampleRequested(winrt::Windows::Media::Core::MediaStreamSourceSampleRequestedEventArgs const& args);

        winrt::Windows::Media::Playback::MediaPlayer m_player{ nullptr };
        winrt::Windows::Media::Core::MediaStreamSource m_source{ nullptr };
        winrt::Windows::Media::Core::MediaStreamSource::SampleRequested_revoker m_requestRevoker;
        winrt::Windows::Media::Core::MediaStreamSource::SampleRendered_revoker m_renderedRevoker;
        winrt::Windows::Media::Core::MediaStreamSource::Starting_revoker m_startingRevoker;

        std::vector<ClipFrame> m_frames;
        mutable std::mutex m_lock;
        size_t m_next = 0;
        int64_t m_delivered = 0;
        int64_t m_lastPtsTicks = 0;
        std::atomic<int64_t> m_sampleLagUs{ 0 };
        std::atomic<int64_t> m_ptsLeadUs{ 0 };
        bool m_realTimePlayback = false;
        uint32_t m_width = 0;
        uint32_t m_height = 0;
        size_t m_resolutionChanges = 0;
        Clock::time_point m_started{};
        winrt::hstring m_error;
    };

    // Milestone 0c: a self-contained AudioGraph that synthesizes a 440 Hz sine directly in
    // QuantumStarted (no ring, no timer, no drift control), so any glitch belongs to AudioGraph.
    // Each run is one entry of a fixed matrix: tone rate (graph native = test A, 44.1 kHz = test B)
    // crossed with the quantum size selection mode.
    class PocToneTest : public std::enable_shared_from_this<PocToneTest>
    {
    public:
        enum class ToneRate
        {
            GraphNative,    // A: generate at the graph's actual rate; no sample-rate conversion
            Input44k,       // B: 44.1 kHz input node; AudioGraph resamples (the UxPlay case)
        };

        struct Config
        {
            wchar_t const* name = L"";
            ToneRate rate = ToneRate::GraphNative;
            winrt::Windows::Media::Audio::QuantumSizeSelectionMode quantumMode =
                winrt::Windows::Media::Audio::QuantumSizeSelectionMode::LowestLatency;
            int32_t desiredSamplesPerQuantum = 0;   // ClosestToDesired only
            double amplitude = 0.2;
        };

        static size_t TestCount();
        static Config TestConfig(size_t index);

        winrt::Windows::Foundation::IAsyncAction StartAsync(Config config, size_t index);
        void Stop();
        bool IsRunning() const { return m_running.load(); }
        winrt::hstring Status() const;

    private:
        void OnQuantumStarted(winrt::Windows::Media::Audio::AudioFrameInputNode const& sender,
                              winrt::Windows::Media::Audio::FrameInputNodeQuantumStartedEventArgs const& args);
        void OnGraphError(winrt::Windows::Media::Audio::AudioGraphUnrecoverableError error);
        void Fail(winrt::hstring const& message);
        void Release();

        static constexpr int64_t kAnchorCallback = 100;

        Config m_config;
        size_t m_index = 0;

        winrt::Windows::Media::Audio::AudioGraph m_graph{ nullptr };
        winrt::Windows::Media::Audio::AudioDeviceOutputNode m_output{ nullptr };
        winrt::Windows::Media::Audio::AudioFrameInputNode m_input{ nullptr };
        winrt::Windows::Media::Audio::AudioGraph::UnrecoverableErrorOccurred_revoker m_graphErrorRevoker;
        winrt::Windows::Media::Audio::AudioFrameInputNode::QuantumStarted_revoker m_quantumRevoker;
        winrt::Windows::Media::Audio::AudioFrameInputNode::AudioFrameCompleted_revoker m_completedRevoker;

        // Written on the UI thread before the graph starts; read-only afterwards.
        uint32_t m_graphRate = 0;
        uint32_t m_graphChannels = 0;
        uint32_t m_nodeRate = 0;
        uint32_t m_nodeChannels = 0;
        uint32_t m_nodeBits = 0;
        winrt::hstring m_nodeSubtype;
        int32_t m_samplesPerQuantum = 0;
        mutable int32_t m_latencySamples = 0;       // refreshed on the UI thread while running
        mutable uint64_t m_completedQuanta = 0;
        winrt::Windows::Media::AudioProcessing m_requestedProcessing = winrt::Windows::Media::AudioProcessing::Raw;
        winrt::Windows::Media::AudioProcessing m_actualProcessing = winrt::Windows::Media::AudioProcessing::Default;
        double m_phaseStep = 0.0;
        double m_quantumNs = 0.0;
        Clock::time_point m_startedAt{};
        Clock::time_point m_stoppedAt{};
        winrt::hstring m_error;

        // Audio (QuantumStarted) thread only.
        double m_phase = 0.0;
        Clock::time_point m_lastCallback{};
        Clock::time_point m_anchor{};
        int64_t m_deficitMinUs = 0;

        // Published to the UI with relaxed atomics; single writer each.
        std::atomic<bool> m_stopRequested{ false };
        std::atomic<bool> m_running{ false };
        std::atomic<int64_t> m_callbacks{ 0 };
        std::atomic<int64_t> m_lateCallbacks{ 0 };
        std::atomic<int64_t> m_maxIntervalUs{ 0 };
        std::atomic<int64_t> m_zeroSampleCallbacks{ 0 };
        std::atomic<int64_t> m_requiredMin{ -1 };
        std::atomic<int64_t> m_requiredMax{ 0 };
        std::atomic<int64_t> m_requiredLatest{ 0 };
        std::atomic<int64_t> m_queuedLatest{ 0 };
        std::atomic<int64_t> m_queuedMax{ 0 };
        std::atomic<int64_t> m_framesSubmitted{ 0 };
        std::atomic<int64_t> m_samplesSubmitted{ 0 };
        std::atomic<int64_t> m_framesCompleted{ 0 };
        std::atomic<int64_t> m_callbackTotalUs{ 0 };
        std::atomic<int64_t> m_callbackMaxUs{ 0 };
        std::atomic<int64_t> m_slowCallbacks{ 0 };
        std::atomic<int64_t> m_stallUs{ 0 };
        std::atomic<int64_t> m_stallMaxUs{ 0 };
        std::atomic<int64_t> m_callbackErrors{ 0 };
        std::atomic<uint32_t> m_lastCallbackHr{ 0 };
        std::atomic<int64_t> m_badBuffers{ 0 };
        std::atomic<int64_t> m_graphErrors{ 0 };
        std::atomic<int32_t> m_lastGraphError{ 0 };
    };
}
