#include "pch.h"
#include "AudioPresenter.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "DebugLog.h"

namespace
{
    // Same IID as Windows.Foundation's IMemoryBufferByteAccess in <MemoryBuffer.h>; declared
    // here so the ABI ::Windows namespace does not collide with winrt::Windows.
    struct __declspec(uuid("5b0d3235-4dba-4d44-865e-8f1d0e4fd04d")) __declspec(novtable) IMemoryBufferByteAccess : ::IUnknown
    {
        virtual HRESULT __stdcall GetBuffer(uint8_t** value, uint32_t* capacity) = 0;
    };
}

using namespace winrt;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Media;
using namespace winrt::Windows::Media::Audio;
using namespace winrt::Windows::Media::MediaProperties;
using namespace winrt::Windows::Media::Render;

namespace rx
{
    namespace
    {
        // RequiredSamples is normally one quantum and a few after a stall. A request beyond
        // this many quanta is rendered as silence rather than growing the stage.
        constexpr size_t kMaxQuantaPerRender = 8;
        constexpr size_t kMinStageFrames = 16384;

        wchar_t const* GraphErrorName(AudioGraphUnrecoverableError error)
        {
            switch (error)
            {
            case AudioGraphUnrecoverableError::AudioDeviceLost: return L"audio device lost";
            case AudioGraphUnrecoverableError::AudioSessionDisconnected: return L"audio session disconnected";
            case AudioGraphUnrecoverableError::None: return L"no error";
            default: return L"unknown failure";
            }
        }
    }

    AudioPresenter::AudioPresenter(std::shared_ptr<PcmRingBuffer> ring, std::shared_ptr<ReceiverStats> stats, uint32_t inputRate) :
        m_ring(std::move(ring)),
        m_stats(std::move(stats)),
        m_inputRate(inputRate),
        m_channels(m_ring->Channels())
    {
        m_stage.resize(kMinStageFrames * m_channels);
        ResetPlayout();
    }

    void AudioPresenter::SetFailureHandler(FailureHandler handler)
    {
        std::lock_guard<std::mutex> lock(m_errorLock);
        m_failureHandler = std::move(handler);
    }

    AudioPresenter::~AudioPresenter()
    {
        m_quantumRevoker.revoke();
        m_graphErrorRevoker.revoke();
        ReleaseGraph();
    }

    hstring AudioPresenter::LastError() const
    {
        std::lock_guard<std::mutex> lock(m_errorLock);
        return m_lastError;
    }

    void AudioPresenter::SetError(hstring const& message)
    {
        Log(L"audio: %s", message.c_str());
        std::lock_guard<std::mutex> lock(m_errorLock);
        m_lastError = message;
    }

    IAsyncAction AudioPresenter::StartAsync()
    {
        auto strong = shared_from_this();
        std::weak_ptr<AudioPresenter> weak = strong;

        try
        {
            AudioGraphSettings settings(AudioRenderCategory::Media);
            settings.QuantumSizeSelectionMode(QuantumSizeSelectionMode::SystemDefault);
            settings.DesiredRenderDeviceAudioProcessing(AudioProcessing::Raw);

            CreateAudioGraphResult result = co_await AudioGraph::CreateAsync(settings);
            if (result.Status() != AudioGraphCreationStatus::Success)
            {
                settings.DesiredRenderDeviceAudioProcessing(AudioProcessing::Default);
                result = co_await AudioGraph::CreateAsync(settings);
            }
            if (result.Status() != AudioGraphCreationStatus::Success)
            {
                SetError(L"AudioGraph creation failed (status " + to_hstring(static_cast<int32_t>(result.Status())) + L")");
                co_return;
            }
            m_graph = result.Graph();
            m_graphErrorRevoker = m_graph.UnrecoverableErrorOccurred(auto_revoke,
                [weak](AudioGraph const&, AudioGraphUnrecoverableErrorOccurredEventArgs const& args)
                {
                    if (auto self = weak.lock())
                    {
                        self->OnGraphError(args.Error());
                    }
                });
            if (m_stopRequested)
            {
                ReleaseGraph();
                co_return;
            }

            CreateAudioDeviceOutputNodeResult outputResult = co_await m_graph.CreateDeviceOutputNodeAsync();
            if (m_stopRequested)
            {
                ReleaseGraph();
                co_return;
            }
            if (outputResult.Status() != AudioDeviceNodeCreationStatus::Success)
            {
                SetError(L"Audio output node creation failed (status " + to_hstring(static_cast<int32_t>(outputResult.Status())) + L")");
                ReleaseGraph();
                co_return;
            }
            m_output = outputResult.DeviceOutputNode();

            // Feed the graph at its own rate: AudioGraph's frame-input resampling was audibly worse.
            uint32_t const graphRate = m_graph.EncodingProperties().SampleRate();
            AudioEncodingProperties requested = AudioEncodingProperties::CreatePcm(graphRate, m_channels, 32);
            requested.Subtype(MediaEncodingSubtypes::Float());
            m_input = m_graph.CreateFrameInputNode(requested);
            m_input.Stop();

            AudioEncodingProperties const actual = m_input.EncodingProperties();
            if (actual.Subtype() != MediaEncodingSubtypes::Float() || actual.BitsPerSample() != 32 ||
                actual.ChannelCount() != m_channels || actual.SampleRate() == 0)
            {
                SetError(L"Audio input node format is " + actual.Subtype() + L" " + to_hstring(actual.BitsPerSample()) + L"-bit, " +
                         to_hstring(actual.ChannelCount()) + L" ch; expected 32-bit Float, " + to_hstring(m_channels) + L" ch");
                ReleaseGraph();
                co_return;
            }
            uint32_t const nodeRate = actual.SampleRate();
            m_baseRatio = static_cast<double>(m_inputRate) / nodeRate;
            m_resampler.Configure(m_baseRatio);
            size_t const quantum = static_cast<size_t>(std::max(m_graph.SamplesPerQuantum(), 0));
            size_t const stageFrames = std::max(kMinStageFrames,
                SincResampler::StageFrames(quantum * kMaxQuantaPerRender, m_baseRatio * (1.0 + DriftTrim::kMaxCorrection)));
            m_stage.assign(stageFrames * m_channels, 0.0f);
            ResetPlayout();

            m_input.AddOutgoingConnection(m_output);
            m_quantumRevoker = m_input.QuantumStarted(auto_revoke,
                [weak](AudioFrameInputNode const& sender, FrameInputNodeQuantumStartedEventArgs const& args)
                {
                    if (auto self = weak.lock())
                    {
                        self->OnQuantumStarted(sender, args);
                    }
                });

            m_input.Start();
            m_graph.Start();

            // LatencyInSamples is only meaningful once the graph is running.
            int32_t const latencySamples = m_graph.LatencyInSamples();
            int32_t const quantumSamples = m_graph.SamplesPerQuantum();
            m_outputLatencyUs = graphRate > 0 ? static_cast<int64_t>(latencySamples) * 1'000'000 / graphRate : 0;
            m_stats->Set(Stat::AudioOutputLatencyUs, m_outputLatencyUs.load());
            m_graphRate = static_cast<uint32_t>(graphRate);
            m_stats->Set(Stat::AudioGraphRate, graphRate);
            m_stats->Set(Stat::AudioQuantumSamples, quantumSamples);
            m_stats->Set(Stat::AudioResamplerActive, nodeRate != m_inputRate ? 1 : 0);
            m_running = true;
            m_stats->Set(Stat::AudioRunning, 1);
            Log(L"audio: graph started (%u Hz, quantum %d, latency %d samples, processing %s)", graphRate, quantumSamples,
                latencySamples, m_graph.RenderDeviceAudioProcessing() == AudioProcessing::Raw ? L"Raw" : L"Default");
        }
        catch (hresult_error const& e)
        {
            SetError(L"Audio start failed: " + e.message());
            m_quantumRevoker.revoke();
            ReleaseGraph();
        }
    }

    void AudioPresenter::Stop()
    {
        m_stopRequested = true;
        m_quantumRevoker.revoke();
        if (m_graph)
        {
            try
            {
                m_graph.Stop();
            }
            catch (hresult_error const&)
            {
            }
        }
        ReleaseGraph();
        m_running = false;
        m_stats->Set(Stat::AudioRunning, 0);
    }

    void AudioPresenter::ReleaseGraph()
    {
        m_graphErrorRevoker.revoke();
        try
        {
            if (m_input)
            {
                m_input.Close();
            }
            if (m_output)
            {
                m_output.Close();
            }
            if (m_graph)
            {
                m_graph.Close();
            }
        }
        catch (hresult_error const&)
        {
        }
        m_input = nullptr;
        m_output = nullptr;
        m_graph = nullptr;
    }

    // AudioGraph's thread: records the failure and notifies the owner. Nothing here touches the
    // graph, its nodes or its handlers; the owner calls Stop() on its own thread.
    void AudioPresenter::OnGraphError(AudioGraphUnrecoverableError error)
    {
        if (!m_failed.Set())
        {
            return;
        }
        m_stopRequested = true;
        m_running = false;
        m_stats->Set(Stat::AudioRunning, 0);
        m_stats->Add(Stat::AudioGraphErrors);
        SetError(hstring(L"AudioGraph stopped: ") + GraphErrorName(error) + L". Stop and start the receiver to recover audio.");

        FailureHandler handler;
        {
            std::lock_guard<std::mutex> lock(m_errorLock);
            handler = m_failureHandler;
        }
        if (handler)
        {
            handler();
        }
    }

    // Real-time audio thread: no locks, no logging. The AudioFrame is the only allocation.
    void AudioPresenter::OnQuantumStarted(AudioFrameInputNode const& sender, FrameInputNodeQuantumStartedEventArgs const& args)
    {
        // RequiredSamples counts frames per channel at the input node's rate.
        int32_t const required = args.RequiredSamples();
        if (required <= 0 || m_stopRequested.load(std::memory_order_relaxed))
        {
            return;
        }

        try
        {
            uint32_t const frames = static_cast<uint32_t>(required);
            uint32_t const bytes = frames * m_channels * static_cast<uint32_t>(sizeof(float));
            AudioFrame frame(bytes);
            bool filled = false;
            {
                AudioBuffer buffer = frame.LockBuffer(AudioBufferAccessMode::Write);
                IMemoryBufferReference reference = buffer.CreateReference();
                uint8_t* data = nullptr;
                uint32_t capacity = 0;
                check_hresult(reference.as<IMemoryBufferByteAccess>()->GetBuffer(&data, &capacity));
                if (data != nullptr && capacity >= bytes)
                {
                    auto const started = Clock::now();
                    Render(reinterpret_cast<float*>(data), frames);
                    int64_t const callbackUs = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - started).count();
                    if (callbackUs >= 0)
                    {
                        m_stats->Set(Stat::AudioCallbackUs, callbackUs);
                        if (m_inputRate > 0)
                        {
                            // Estimate only: required frames are at the input-node rate, not HDMI latency.
                            int64_t const budgetUs = static_cast<int64_t>(frames) * 1000000 / m_inputRate;
                            if (callbackUs > budgetUs)
                            {
                                m_stats->Add(Stat::AudioCallbackOverruns);
                            }
                        }
                    }
                    buffer.Length(bytes);
                    filled = true;
                }
                reference.Close();
                buffer.Close();
            }
            if (filled)
            {
                sender.AddFrame(frame);
            }
            else
            {
                m_stats->Add(Stat::AudioCallbackErrors);
            }
        }
        catch (hresult_error const&)
        {
            m_stats->Add(Stat::AudioCallbackErrors);
        }
    }

    void AudioPresenter::ResetPlayout()
    {
        std::fill(m_stage.begin(), m_stage.begin() + SincResampler::kHistory * m_channels, 0.0f);
        m_staged = SincResampler::kHistory;
        m_position = static_cast<double>(SincResampler::kHistory);
        m_primed = false;
        m_manualStep.Reset();
        m_drift.Reset();
    }

    void AudioPresenter::Render(float* out, size_t frames)
    {
        double const framesPerMs = m_inputRate / 1000.0;

        if (m_ring->ConsumeFlushRequest())
        {
            m_ring->Discard(m_ring->Available());
            ResetPlayout();
        }

        // A manual A/V offset change moves playout now instead of waiting for drift correction:
        // an earlier target drops buffered input here, a later one inserts silence below.
        AudioTargetDelay::Sample const delay = m_target.Load();
        size_t const discard = m_manualStep.Update(delay, m_primed, framesPerMs);
        if (discard > 0)
        {
            m_ring->Discard(discard);
        }

        size_t const fill = m_ring->Available() + StagedAhead();
        size_t const target = static_cast<size_t>(delay.targetMs * framesPerMs);
        size_t const hardCap = static_cast<size_t>(std::max(m_maxMs.load(), delay.targetMs + 50) * framesPerMs);

        int64_t const fillUs = static_cast<int64_t>(fill / framesPerMs * 1000.0);
        m_stats->Set(Stat::AudioFillUs, fillUs);
        m_stats->Raise(Stat::AudioFillHighUs, fillUs);
        m_stats->Set(Stat::AudioTargetDelayMs, delay.targetMs);

        if (!m_primed)
        {
            if (fill < target || fill == 0)
            {
                std::memset(out, 0, frames * m_channels * sizeof(float));
                return;
            }
            m_primed = true;
        }

        if (fill > hardCap)
        {
            size_t const excess = fill - target;
            size_t const dropped = m_ring->Discard(excess);
            m_stats->Add(Stat::AudioHardCapDropFrames, static_cast<int64_t>(dropped));
            m_drift.Reset();
        }

        // Silence still owed by a manual step counts as fill, so the step is not mistaken for drift.
        double const errorMs = (static_cast<double>(m_ring->Available() + StagedAhead()) - static_cast<double>(target)) / framesPerMs +
                               m_manualStep.PendingHoldMs();
        double const correction = m_drift.Update(errorMs, Clock::now());
        m_stats->Set(Stat::AudioDriftPpm, static_cast<int64_t>(correction * 1e6));

        // While a later target is pending, emit silence without consuming input; the ring grows by
        // exactly the held time and playout resumes from the same position.
        size_t const hold = m_manualStep.TakeHold(frames, framesPerMs / m_baseRatio);
        if (hold > 0)
        {
            std::memset(out, 0, hold * m_channels * sizeof(float));
            out += hold * m_channels;
            frames -= hold;
            if (frames == 0)
            {
                return;
            }
        }

        double const ratio = m_baseRatio * (1.0 + correction);
        size_t const needed = SincResampler::FramesNeeded(m_position, frames, ratio);
        if (needed * m_channels > m_stage.size())
        {
            std::memset(out, 0, frames * m_channels * sizeof(float));
            m_stats->Add(Stat::AudioCallbackErrors);
            return;
        }
        if (m_staged < needed)
        {
            m_staged += m_ring->Read(m_stage.data() + m_staged * m_channels, needed - m_staged);
        }

        // Output k is available while floor(position + k * ratio) + kLookahead < staged.
        size_t produced = frames;
        if (m_staged < needed)
        {
            double const room = static_cast<double>(m_staged) - static_cast<double>(SincResampler::kLookahead) - m_position;
            produced = room > 0.0 ? std::min(frames, static_cast<size_t>(std::ceil(room / ratio))) : 0;
            while (produced > 0 &&
                   static_cast<size_t>(m_position + (produced - 1) * ratio) + SincResampler::kLookahead >= m_staged)
            {
                --produced;
            }
        }
        m_resampler.Process(m_stage.data(), m_channels, m_position, ratio, out, produced);

        if (produced < frames)
        {
            std::memset(out + produced * m_channels, 0, (frames - produced) * m_channels * sizeof(float));
            m_stats->Add(Stat::AudioUnderruns);
            uint32_t const rate = m_graphRate.load();
            if (rate > 0)
            {
                int64_t const gapUs = static_cast<int64_t>(frames - produced) * 1000000 / rate;
                m_stats->Raise(Stat::AudioUnderrunLongestUs, gapUs);
            }
            ResetPlayout();
            return;
        }

        m_position += frames * ratio;
        size_t const whole = static_cast<size_t>(m_position);
        if (whole > SincResampler::kHistory)
        {
            size_t const consumed = std::min(whole - SincResampler::kHistory, m_staged);
            std::memmove(m_stage.data(), m_stage.data() + consumed * m_channels, (m_staged - consumed) * m_channels * sizeof(float));
            m_staged -= consumed;
            m_position -= static_cast<double>(consumed);
        }
    }
}
