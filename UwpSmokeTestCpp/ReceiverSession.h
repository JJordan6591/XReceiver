#pragma once

#include <atomic>
#include <deque>
#include <memory>
#include <mutex>

#include "H264Bitstream.h"
#include "LifecycleGuard.h"
#include "MediaClock.h"
#include "ReceiverSettings.h"
#include "ReceiverStats.h"

namespace rx
{
    class AudioPresenter;
    class AudioReceiver;
    class PcmRingBuffer;
    class VideoPresenter;
    class VideoReceiver;

    enum class ConnectionState
    {
        Stopped,
        Starting,
        Waiting,
        Receiving,
        Reconnecting,
        Error,
        Stopping,
    };

    wchar_t const* ToString(ConnectionState state);

    // Owns one receive session: sockets, pipelines, presenters, the housekeeping timer and the
    // connection state machine. Start/Stop/OnUiTick are called on the UI thread only.
    class ReceiverSession : public std::enable_shared_from_this<ReceiverSession>
    {
    public:
        ReceiverSession(winrt::Windows::UI::Core::CoreDispatcher const& dispatcher,
                        winrt::Windows::Media::Playback::MediaPlayer const& player);
        ~ReceiverSession();

        ReceiverSession(ReceiverSession const&) = delete;
        ReceiverSession& operator=(ReceiverSession const&) = delete;

        winrt::Windows::Foundation::IAsyncAction StartAsync(ReceiverSettings settings);
        winrt::Windows::Foundation::IAsyncAction StopAsync();

        void OnUiTick();

        ConnectionState State() const { return m_state.load(); }
        bool IsActive() const;
        winrt::hstring ErrorMessage() const;
        winrt::hstring AudioStatus() const;

        std::shared_ptr<ReceiverStats> Stats() const { return m_stats; }

        void SetAvOffsetMs(int32_t ms) { m_avOffsetMs.store(ms); }
        void SetLossPolicy(LossPolicy policy);

    private:
        void OnHousekeeping();
        void EvaluateState(Clock::time_point now, VideoReceiver& video, AudioReceiver const* audio);
        void PostSourceRequest(SpsInfo const& sps, uint64_t sourceId);
        void PostMediaFailed(winrt::hstring const& message);
        void HandleMediaFailed(winrt::hstring const& message);
        void PostAudioFailed(std::weak_ptr<AudioPresenter> presenter, uint64_t generation);
        void HandleAudioFailed(std::shared_ptr<AudioPresenter> const& presenter);
        void SetError(winrt::hstring const& message);
        void SetAudioStatus(winrt::hstring const& message);
        void TearDown();

        winrt::Windows::UI::Core::CoreDispatcher m_dispatcher{ nullptr };
        winrt::Windows::Media::Playback::MediaPlayer m_player{ nullptr };
        std::shared_ptr<ReceiverStats> m_stats;

        ReceiverSettings m_settings;
        std::atomic<ConnectionState> m_state{ ConnectionState::Stopped };
        LifecycleGeneration m_generation;
        std::atomic<int32_t> m_avOffsetMs{ 0 };

        // Components are replaced on the UI thread and read by the housekeeping timer.
        mutable std::mutex m_componentsLock;
        std::shared_ptr<VideoReceiver> m_video;
        std::shared_ptr<AudioReceiver> m_audio;
        std::shared_ptr<PcmRingBuffer> m_ring;
        std::shared_ptr<AudioPresenter> m_audioPresenter;
        winrt::Windows::System::Threading::ThreadPoolTimer m_timer{ nullptr };

        // UI thread only.
        std::shared_ptr<VideoPresenter> m_videoPresenter;
        std::deque<Clock::time_point> m_mediaFailures;

        mutable std::mutex m_textLock;
        winrt::hstring m_errorMessage;
        winrt::hstring m_audioStatus;
    };
}
