#include "pch.h"
#include "ReceiverSession.h"

#include <algorithm>

#include "AudioPresenter.h"
#include "AudioReceiver.h"
#include "DebugLog.h"
#include "L16Convert.h"
#include "PcmRingBuffer.h"
#include "VideoPresenter.h"
#include "VideoReceiver.h"

using namespace winrt;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Media::Playback;
using namespace winrt::Windows::System::Threading;
using namespace winrt::Windows::UI::Core;

namespace rx
{
    wchar_t const* ToString(ConnectionState state)
    {
        switch (state)
        {
        case ConnectionState::Stopped: return L"Stopped";
        case ConnectionState::Starting: return L"Starting";
        case ConnectionState::Waiting: return L"Waiting";
        case ConnectionState::Receiving: return L"Receiving";
        case ConnectionState::Reconnecting: return L"Reconnecting";
        case ConnectionState::Error: return L"Error";
        case ConnectionState::Stopping: return L"Stopping";
        }
        return L"Unknown";
    }

    ReceiverSession::ReceiverSession(CoreDispatcher const& dispatcher, MediaPlayer const& player) :
        m_dispatcher(dispatcher),
        m_player(player),
        m_stats(std::make_shared<ReceiverStats>())
    {
    }

    ReceiverSession::~ReceiverSession()
    {
        TearDown();
    }

    bool ReceiverSession::IsActive() const
    {
        auto const state = m_state.load();
        return state != ConnectionState::Stopped && state != ConnectionState::Stopping;
    }

    hstring ReceiverSession::ErrorMessage() const
    {
        std::lock_guard<std::mutex> lock(m_textLock);
        return m_errorMessage;
    }

    hstring ReceiverSession::AudioStatus() const
    {
        std::lock_guard<std::mutex> lock(m_textLock);
        return m_audioStatus;
    }

    void ReceiverSession::SetError(hstring const& message)
    {
        if (!message.empty())
        {
            Log(L"session: %s", message.c_str());
        }
        std::lock_guard<std::mutex> lock(m_textLock);
        m_errorMessage = message;
    }

    void ReceiverSession::SetAudioStatus(hstring const& message)
    {
        std::lock_guard<std::mutex> lock(m_textLock);
        m_audioStatus = message;
    }

    IAsyncAction ReceiverSession::StartAsync(ReceiverSettings settings)
    {
        auto strong = shared_from_this();

        auto const current = m_state.load();
        if (current != ConnectionState::Stopped && current != ConnectionState::Error)
        {
            co_return;
        }
        if (current == ConnectionState::Error)
        {
            TearDown();
        }

        uint64_t const generation = m_generation.Advance();
        m_settings = settings;
        m_settings.Sanitize();
        m_avOffsetMs = m_settings.avOffsetMs;
        m_stats->Reset();
        m_mediaFailures.clear();
        SetError(L"");
        SetAudioStatus(L"");
        m_state = ConnectionState::Starting;

        std::weak_ptr<ReceiverSession> weak = strong;

        VideoPresenter::ConfigurePlayer(m_player, m_stats.get());
        m_videoPresenter = std::make_shared<VideoPresenter>(m_player, m_stats);
        m_videoPresenter->SetMediaFailedHandler([weak](hstring const& message)
            {
                if (auto self = weak.lock())
                {
                    self->PostMediaFailed(message);
                }
            });

        auto video = std::make_shared<VideoReceiver>(m_settings, m_stats);
        video->SetSourceRequestHandler([weak](SpsInfo const& sps, uint64_t sourceId)
            {
                if (auto self = weak.lock())
                {
                    self->PostSourceRequest(sps, sourceId);
                }
            });
        {
            std::lock_guard<std::mutex> lock(m_componentsLock);
            m_video = video;
        }

        hstring bindError;
        try
        {
            co_await video->StartAsync(static_cast<uint16_t>(m_settings.videoPort));
        }
        catch (hresult_error const& e)
        {
            bindError = L"Could not bind video UDP port " + to_hstring(m_settings.videoPort) + L": " + e.message();
        }
        co_await resume_foreground(m_dispatcher);
        if (!m_generation.IsCurrent(generation))
        {
            co_return;
        }
        if (!bindError.empty())
        {
            SetError(bindError);
            TearDown();
            m_state = ConnectionState::Error;
            co_return;
        }

        if (m_settings.audioEnabled)
        {
            auto ring = std::make_shared<PcmRingBuffer>(kL16SampleRate, kL16Channels);
            auto audio = std::make_shared<AudioReceiver>(m_settings, m_stats, ring);
            auto audioPresenter = std::make_shared<AudioPresenter>(ring, m_stats, kL16SampleRate);
            audioPresenter->SetMaxDelayMs(std::min(m_settings.audioMaxDelayMs + 200, 500));
            audioPresenter->SetTargetDelayMs(m_settings.audioMinDelayMs + 40);
            std::weak_ptr<AudioPresenter> weakPresenter = audioPresenter;
            audioPresenter->SetFailureHandler([weak, weakPresenter, generation]()
                {
                    if (auto self = weak.lock())
                    {
                        self->PostAudioFailed(weakPresenter, generation);
                    }
                });
            {
                std::lock_guard<std::mutex> lock(m_componentsLock);
                m_ring = ring;
                m_audio = audio;
                m_audioPresenter = audioPresenter;
            }

            hstring audioBindError;
            try
            {
                co_await audio->StartAsync(static_cast<uint16_t>(m_settings.audioPort));
            }
            catch (hresult_error const& e)
            {
                audioBindError = L"Audio port " + to_hstring(m_settings.audioPort) + L" bind failed: " + e.message();
            }
            co_await resume_foreground(m_dispatcher);
            if (!m_generation.IsCurrent(generation))
            {
                co_return;
            }

            if (audioBindError.empty())
            {
                co_await audioPresenter->StartAsync();
                co_await resume_foreground(m_dispatcher);
                if (!m_generation.IsCurrent(generation))
                {
                    co_return;
                }
                if (!audioPresenter->IsRunning())
                {
                    SetAudioStatus(audioPresenter->LastError());
                }
            }
            else
            {
                // Audio problems never stop video.
                SetAudioStatus(audioBindError);
                audio->Stop();
                std::lock_guard<std::mutex> lock(m_componentsLock);
                m_audio = nullptr;
            }
        }
        else
        {
            SetAudioStatus(L"Audio disabled");
        }

        // A Start superseded by Stop or a newer Start must not publish the timer or Waiting.
        if (!m_generation.IsCurrent(generation))
        {
            co_return;
        }
        m_timer = ThreadPoolTimer::CreatePeriodicTimer([weak](ThreadPoolTimer const&)
            {
                if (auto self = weak.lock())
                {
                    self->OnHousekeeping();
                }
            },
            std::chrono::milliseconds(10));

        m_state = ConnectionState::Waiting;
        Log(L"session: started (video %d, audio %d)", m_settings.videoPort, m_settings.audioPort);
    }

    IAsyncAction ReceiverSession::StopAsync()
    {
        auto strong = shared_from_this();
        if (m_state.load() == ConnectionState::Stopped)
        {
            co_return;
        }
        m_generation.Advance();
        m_state = ConnectionState::Stopping;
        TearDown();
        m_state = ConnectionState::Stopped;
        Log(L"session: stopped");
        co_return;
    }

    void ReceiverSession::TearDown()
    {
        if (m_timer)
        {
            m_timer.Cancel();
            m_timer = nullptr;
        }

        std::shared_ptr<VideoReceiver> video;
        std::shared_ptr<AudioReceiver> audio;
        std::shared_ptr<AudioPresenter> audioPresenter;
        {
            std::lock_guard<std::mutex> lock(m_componentsLock);
            video = std::move(m_video);
            audio = std::move(m_audio);
            audioPresenter = std::move(m_audioPresenter);
            m_ring.reset();
        }

        if (video)
        {
            video->Stop();
        }
        if (audio)
        {
            audio->Stop();
        }
        if (m_videoPresenter)
        {
            m_videoPresenter->Shutdown();
            m_videoPresenter.reset();
        }
        if (audioPresenter)
        {
            audioPresenter->Stop();
        }
    }

    void ReceiverSession::OnHousekeeping()
    {
        std::shared_ptr<VideoReceiver> video;
        std::shared_ptr<AudioReceiver> audio;
        {
            std::lock_guard<std::mutex> lock(m_componentsLock);
            video = m_video;
            audio = m_audio;
        }
        if (!video)
        {
            return;
        }

        auto const now = Clock::now();
        video->Poll(now);
        if (audio)
        {
            audio->Poll(now);
        }
        EvaluateState(now, *video, audio.get());
    }

    void ReceiverSession::EvaluateState(Clock::time_point now, VideoReceiver& video, AudioReceiver const* audio)
    {
        auto current = m_state.load();
        if (current == ConnectionState::Stopped || current == ConnectionState::Stopping ||
            current == ConnectionState::Starting || current == ConnectionState::Error)
        {
            return;
        }

        VideoActivity const activity = video.Activity();
        if (activity.hevcDetected)
        {
            SetError(L"The video stream appears to be HEVC (H.265). Run UxPlay without -h265.");
            m_state.compare_exchange_strong(current, ConnectionState::Error);
            return;
        }

        // The session is alive while either stream delivers packets: a static AirPlay screen
        // sends few or no video frames while audio continues. Sockets, decoder and cached
        // parameter sets are kept through any gap; only the displayed state changes.
        bool everReceived = activity.everReceived;
        Clock::time_point lastActivity = activity.lastPacket;
        if (audio && audio->EverReceived())
        {
            Clock::time_point const audioLast = audio->LastPacketTime();
            if (!everReceived || audioLast > lastActivity)
            {
                lastActivity = audioLast;
            }
            everReceived = true;
        }

        auto const stallAfter = FromMs(std::max(m_settings.idleTimeoutMs, ReceiverSettings::kMinIdleTimeoutMs));
        ConnectionState next = ConnectionState::Waiting;
        if (everReceived && activity.everSubmitted)
        {
            next = now - lastActivity >= stallAfter ? ConnectionState::Reconnecting : ConnectionState::Receiving;
        }

        if (next != current)
        {
            m_state.compare_exchange_strong(current, next);
        }
    }

    void ReceiverSession::PostSourceRequest(SpsInfo const& sps, uint64_t sourceId)
    {
        std::weak_ptr<ReceiverSession> weak = weak_from_this();
        uint64_t const generation = m_generation.Current();
        try
        {
            m_dispatcher.RunAsync(CoreDispatcherPriority::High, [weak, sps, sourceId, generation]()
                {
                    auto self = weak.lock();
                    if (!self || !self->m_generation.IsCurrent(generation) || !self->m_videoPresenter)
                    {
                        return;
                    }
                    std::shared_ptr<VideoReceiver> video;
                    {
                        std::lock_guard<std::mutex> lock(self->m_componentsLock);
                        video = self->m_video;
                    }
                    if (!video)
                    {
                        return;
                    }
                    try
                    {
                        self->m_videoPresenter->OpenSource(sps, sourceId, video);
                    }
                    catch (hresult_error const& e)
                    {
                        self->HandleMediaFailed(L"Could not open video source: " + e.message());
                    }
                });
        }
        catch (hresult_error const&)
        {
        }
    }

    void ReceiverSession::PostMediaFailed(hstring const& message)
    {
        std::weak_ptr<ReceiverSession> weak = weak_from_this();
        uint64_t const generation = m_generation.Current();
        try
        {
            m_dispatcher.RunAsync(CoreDispatcherPriority::Normal, [weak, message, generation]()
                {
                    auto self = weak.lock();
                    if (self && self->m_generation.IsCurrent(generation))
                    {
                        self->HandleMediaFailed(message);
                    }
                });
        }
        catch (hresult_error const&)
        {
        }
    }

    void ReceiverSession::PostAudioFailed(std::weak_ptr<AudioPresenter> presenter, uint64_t generation)
    {
        // Raised on AudioGraph's thread. Releasing the graph there could re-enter AudioGraph, so
        // teardown runs on the UI thread that owns the session, and only for the same Start.
        std::weak_ptr<ReceiverSession> weak = weak_from_this();
        try
        {
            m_dispatcher.RunAsync(CoreDispatcherPriority::Normal, [weak, presenter, generation]()
                {
                    auto self = weak.lock();
                    if (self && self->m_generation.IsCurrent(generation))
                    {
                        self->HandleAudioFailed(presenter.lock());
                    }
                });
        }
        catch (hresult_error const&)
        {
        }
    }

    void ReceiverSession::HandleAudioFailed(std::shared_ptr<AudioPresenter> const& presenter)
    {
        {
            std::lock_guard<std::mutex> lock(m_componentsLock);
            if (!presenter || presenter != m_audioPresenter)
            {
                return;
            }
        }
        // Only the graph goes: video keeps playing and the audio receiver keeps counting packets,
        // so audio activity still keeps a static screen in Receiving.
        presenter->Stop();
        SetAudioStatus(presenter->LastError());
        Log(L"session: audio graph released after an unrecoverable error; video continues");
    }

    void ReceiverSession::HandleMediaFailed(hstring const& message)
    {
        m_stats->Add(Stat::MediaFailures);
        auto const now = Clock::now();
        m_mediaFailures.push_back(now);
        while (!m_mediaFailures.empty() && now - m_mediaFailures.front() > std::chrono::seconds(60))
        {
            m_mediaFailures.pop_front();
        }

        if (m_videoPresenter)
        {
            m_videoPresenter->CloseSource(true);
        }

        if (m_mediaFailures.size() >= 3)
        {
            SetError(L"Video pipeline failed repeatedly: " + message);
            m_state = ConnectionState::Error;
            return;
        }

        SetError(L"Last media error: " + message);
        std::shared_ptr<VideoReceiver> video;
        {
            std::lock_guard<std::mutex> lock(m_componentsLock);
            video = m_video;
        }
        if (video)
        {
            video->RequestNewSource();
        }
    }

    void ReceiverSession::OnUiTick()
    {
        std::shared_ptr<VideoReceiver> video;
        std::shared_ptr<AudioPresenter> audioPresenter;
        {
            std::lock_guard<std::mutex> lock(m_componentsLock);
            video = m_video;
            audioPresenter = m_audioPresenter;
        }

        if (video && m_videoPresenter)
        {
            m_videoPresenter->UpdatePtsLead(*video);
        }

        if (audioPresenter && audioPresenter->IsRunning())
        {
            AvSyncInputs inputs;
            inputs.videoPipelineLatencyMs = m_settings.videoPipelineLatencyMs;
            inputs.videoReceiveToSubmitMs = m_stats->Get(Stat::ReceiveToSubmitP50Us) / 1000.0;
            inputs.audioOutputLatencyMs = audioPresenter->OutputLatencyMs();
            inputs.audioJitterMs = m_stats->Get(Stat::AudioJitterUs) / 1000.0;
            inputs.userOffsetMs = m_avOffsetMs.load();
            inputs.minDelayMs = m_settings.audioMinDelayMs;
            inputs.maxDelayMs = m_settings.audioMaxDelayMs;
            audioPresenter->SetTargetDelayMs(ComputeAudioTargetDelayMs(inputs));
        }
    }

    void ReceiverSession::SetLossPolicy(LossPolicy policy)
    {
        m_settings.lossPolicy = policy;
        std::shared_ptr<VideoReceiver> video;
        {
            std::lock_guard<std::mutex> lock(m_componentsLock);
            video = m_video;
        }
        if (video)
        {
            video->SetLossPolicy(policy);
        }
    }
}
