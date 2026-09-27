#pragma once

#include <functional>
#include <memory>
#include <mutex>

#include "H264Bitstream.h"
#include "ReceiverStats.h"

namespace rx
{
    class VideoReceiver;

    // Owns the video-only MediaStreamSource and its MediaSource, and binds them to a
    // MediaPlayer configured for real-time playback. UI thread only.
    class VideoPresenter : public std::enable_shared_from_this<VideoPresenter>
    {
    public:
        using MediaFailedHandler = std::function<void(winrt::hstring const& message)>;

        VideoPresenter(winrt::Windows::Media::Playback::MediaPlayer const& player, std::shared_ptr<ReceiverStats> stats);
        ~VideoPresenter();

        VideoPresenter(VideoPresenter const&) = delete;
        VideoPresenter& operator=(VideoPresenter const&) = delete;

        void SetMediaFailedHandler(MediaFailedHandler handler);

        void OpenSource(SpsInfo const& sps, uint64_t sourceId, std::weak_ptr<VideoReceiver> receiver);
        void CloseSource(bool detachPlayer);
        void Shutdown();

        // Feeds the player's presentation position back to the receiver for pts anchoring.
        void UpdatePtsLead(VideoReceiver& receiver);

        uint64_t SourceId() const { return m_sourceId; }
        bool HasSource() const { return m_source != nullptr; }

        static void ConfigurePlayer(winrt::Windows::Media::Playback::MediaPlayer const& player, ReceiverStats* stats);

    private:
        struct FailedState
        {
            std::mutex lock;
            MediaFailedHandler handler;
        };

        winrt::Windows::Media::Playback::MediaPlayer m_player{ nullptr };
        std::shared_ptr<ReceiverStats> m_stats;
        std::shared_ptr<FailedState> m_failed;

        winrt::Windows::Media::Core::MediaStreamSource m_source{ nullptr };
        uint64_t m_sourceId = 0;

        winrt::Windows::Media::Core::MediaStreamSource::Starting_revoker m_startingRevoker;
        winrt::Windows::Media::Core::MediaStreamSource::SampleRequested_revoker m_sampleRequestedRevoker;
        winrt::Windows::Media::Core::MediaStreamSource::Closed_revoker m_closedRevoker;
        winrt::Windows::Media::Core::MediaStreamSource::SampleRendered_revoker m_sampleRenderedRevoker;
        winrt::Windows::Media::Playback::MediaPlayer::MediaFailed_revoker m_mediaFailedRevoker;
    };
}
