#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

#include "AudioPlayout.h"
#include "LifecycleGuard.h"
#include "MediaClock.h"
#include "PcmRingBuffer.h"
#include "ReceiverStats.h"
#include "SincResampler.h"

namespace rx
{
    // Plays PCM from the ring through AudioGraph. The ring's fill level is the audio delay:
    // playback starts once the fill reaches the target, a slow controller trims clock drift
    // with a tiny resampling-ratio adjustment, and a hard cap flushes runaway latency. A manual
    // A/V offset step bypasses the slow controller and moves playout on the next quantum.
    // The graph always runs at its native rate with the SystemDefault quantum (the only
    // configuration that stayed clean on the Xbox); this class does the sample-rate conversion.
    class AudioPresenter : public std::enable_shared_from_this<AudioPresenter>
    {
    public:
        // Called at most once, on AudioGraph's thread, after an unrecoverable graph error. It must
        // not call Stop() synchronously; the owner marshals teardown to its own thread.
        using FailureHandler = std::function<void()>;

        AudioPresenter(std::shared_ptr<PcmRingBuffer> ring, std::shared_ptr<ReceiverStats> stats, uint32_t inputRate);
        ~AudioPresenter();

        AudioPresenter(AudioPresenter const&) = delete;
        AudioPresenter& operator=(AudioPresenter const&) = delete;

        void SetFailureHandler(FailureHandler handler);

        // Completes with IsRunning() false and LastError() set on failure; never throws.
        winrt::Windows::Foundation::IAsyncAction StartAsync();
        // Owner's thread only. Idempotent.
        void Stop();

        // Ordinary target movement; drift correction converges on it gradually.
        void SetTargetDelayMs(int32_t ms) { m_target.Set(ms); }
        // A user A/V offset change; playout moves by the change on the next quantum.
        void StepTargetDelayMs(int32_t ms) { m_target.Step(ms); }
        void SetMaxDelayMs(int32_t ms) { m_maxMs.store(ms); }

        bool IsRunning() const { return m_running.load(); }
        double OutputLatencyMs() const { return m_outputLatencyUs.load() / 1000.0; }
        winrt::hstring LastError() const;

    private:
        void OnQuantumStarted(winrt::Windows::Media::Audio::AudioFrameInputNode const& sender,
                              winrt::Windows::Media::Audio::FrameInputNodeQuantumStartedEventArgs const& args);
        void OnGraphError(winrt::Windows::Media::Audio::AudioGraphUnrecoverableError error);
        void Render(float* out, size_t frames);
        void ResetPlayout();
        size_t StagedAhead() const { return m_staged > SincResampler::kHistory ? m_staged - SincResampler::kHistory : 0; }
        void SetError(winrt::hstring const& message);
        void ReleaseGraph();

        std::shared_ptr<PcmRingBuffer> m_ring;
        std::shared_ptr<ReceiverStats> m_stats;
        uint32_t const m_inputRate;
        uint32_t const m_channels;

        winrt::Windows::Media::Audio::AudioGraph m_graph{ nullptr };
        winrt::Windows::Media::Audio::AudioDeviceOutputNode m_output{ nullptr };
        winrt::Windows::Media::Audio::AudioFrameInputNode m_input{ nullptr };
        winrt::Windows::Media::Audio::AudioFrameInputNode::QuantumStarted_revoker m_quantumRevoker;
        winrt::Windows::Media::Audio::AudioGraph::UnrecoverableErrorOccurred_revoker m_graphErrorRevoker;

        std::atomic<bool> m_stopRequested{ false };
        std::atomic<bool> m_running{ false };
        OnceFlag m_failed;
        AudioTargetDelay m_target{ 60 };
        std::atomic<int32_t> m_maxMs{ 300 };
        std::atomic<int64_t> m_outputLatencyUs{ 0 };
        std::atomic<uint32_t> m_graphRate{ 0 };

        mutable std::mutex m_errorLock;
        winrt::hstring m_lastError;
        FailureHandler m_failureHandler;

        // Written before the graph starts; audio-thread state afterwards. The stage is sized
        // once for the largest render the quantum thread may request and never grows there.
        SincResampler m_resampler;
        double m_baseRatio = 1.0;
        std::vector<float> m_stage;     // kHistory frames of history, then unread input
        size_t m_staged = 0;
        double m_position = 0.0;
        bool m_primed = false;
        ManualDelayStep m_manualStep;
        DriftTrim m_drift;
    };
}
