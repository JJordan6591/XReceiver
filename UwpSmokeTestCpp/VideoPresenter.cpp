#include "pch.h"
#include "VideoPresenter.h"

#include "DebugLog.h"
#include "MediaClock.h"
#include "VideoReceiver.h"

using namespace winrt;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Media::Core;
using namespace winrt::Windows::Media::MediaProperties;
using namespace winrt::Windows::Media::Playback;

namespace rx
{
    VideoPresenter::VideoPresenter(MediaPlayer const& player, std::shared_ptr<ReceiverStats> stats) :
        m_player(player),
        m_stats(std::move(stats)),
        m_failed(std::make_shared<FailedState>())
    {
        m_mediaFailedRevoker = m_player.MediaFailed(auto_revoke,
            [failed = m_failed](MediaPlayer const&, MediaPlayerFailedEventArgs const& args)
            {
                // Raised on a media thread; the handler is responsible for marshalling.
                wchar_t code[32];
                swprintf_s(code, L" (0x%08X)", static_cast<uint32_t>(args.ExtendedErrorCode()));
                hstring const message = args.ErrorMessage() + code;
                Log(L"video: MediaFailed %s", message.c_str());

                MediaFailedHandler handler;
                {
                    std::lock_guard<std::mutex> lock(failed->lock);
                    handler = failed->handler;
                }
                if (handler)
                {
                    handler(message);
                }
            });
    }

    VideoPresenter::~VideoPresenter()
    {
        m_mediaFailedRevoker.revoke();
        m_startingRevoker.revoke();
        m_sampleRequestedRevoker.revoke();
        m_closedRevoker.revoke();
        m_sampleRenderedRevoker.revoke();
    }

    void VideoPresenter::ConfigurePlayer(MediaPlayer const& player, ReceiverStats* stats)
    {
        player.AutoPlay(true);
        player.IsLoopingEnabled(false);
        try
        {
            player.CommandManager().IsEnabled(false);
        }
        catch (hresult_error const&)
        {
        }
        bool applied = false;
        try
        {
            player.RealTimePlayback(true);
            applied = player.RealTimePlayback();
        }
        catch (hresult_error const& e)
        {
            Log(L"video: RealTimePlayback not supported 0x%08x", static_cast<uint32_t>(e.code()));
        }
        if (stats)
        {
            stats->Set(Stat::RealTimePlayback, applied ? 1 : 0);
        }
    }

    void VideoPresenter::SetMediaFailedHandler(MediaFailedHandler handler)
    {
        std::lock_guard<std::mutex> lock(m_failed->lock);
        m_failed->handler = std::move(handler);
    }

    void VideoPresenter::OpenSource(SpsInfo const& sps, uint64_t sourceId, std::weak_ptr<VideoReceiver> receiver)
    {
        CloseSource(false);

        // No frame rate is declared: it is optional for the H.264 decoder's input type, and the
        // RTP-derived sample timestamps and durations are the only clock.
        VideoEncodingProperties properties = VideoEncodingProperties::CreateH264();
        if (sps.valid)
        {
            properties.Width(sps.width);
            properties.Height(sps.height);
            properties.ProfileId(static_cast<int32_t>(sps.profileIdc));
        }

        VideoStreamDescriptor descriptor(properties);
        MediaStreamSource source(descriptor);
        source.CanSeek(false);
        source.BufferTime(TimeSpan{ 0 });
        source.IsLive(true);

        m_startingRevoker = source.Starting(auto_revoke,
            [receiver, sourceId](MediaStreamSource const&, MediaStreamSourceStartingEventArgs const& args)
            {
                auto request = args.Request();
                auto const requested = request.StartPosition();
                TimeSpan const actual{ 0 };
                Log(L"video: src %llu Starting (requested %lld ticks, actual %lld)", sourceId,
                    requested ? requested.Value().count() : -1ll, actual.count());
                request.SetActualStartPosition(actual);
                if (auto self = receiver.lock())
                {
                    self->OnSourceStarting(sourceId, actual);
                }
            });

        m_sampleRequestedRevoker = source.SampleRequested(auto_revoke,
            [receiver, sourceId](MediaStreamSource const&, MediaStreamSourceSampleRequestedEventArgs const& args)
            {
                if (auto self = receiver.lock())
                {
                    self->OnSampleRequested(sourceId, args.Request());
                }
            });

        m_closedRevoker = source.Closed(auto_revoke,
            [receiver, sourceId](MediaStreamSource const&, MediaStreamSourceClosedEventArgs const& args)
            {
                int32_t const reason = static_cast<int32_t>(args.Request().Reason());
                if (auto self = receiver.lock())
                {
                    self->OnSourceClosed(sourceId, reason);
                }
                else
                {
                    Log(L"video: src %llu Closed (reason %d) after receiver shutdown", sourceId, reason);
                }
            });

        m_sampleRenderedRevoker = source.SampleRendered(auto_revoke,
            [receiver, sourceId, stats = m_stats](MediaStreamSource const&, MediaStreamSourceSampleRenderedEventArgs const& args)
            {
                stats->Add(Stat::SamplesRendered);
                stats->Set(Stat::SampleLagUs, std::chrono::duration_cast<std::chrono::microseconds>(args.SampleLag()).count());
                if (auto self = receiver.lock())
                {
                    self->OnSampleRendered(sourceId);
                }
            });

        m_source = source;
        m_sourceId = sourceId;

        m_player.Source(MediaSource::CreateFromMediaStreamSource(source));
        m_player.Play();
        Log(L"video: source %llu opened", sourceId);
    }

    void VideoPresenter::CloseSource(bool detachPlayer)
    {
        m_startingRevoker.revoke();
        m_sampleRequestedRevoker.revoke();
        m_closedRevoker.revoke();
        m_sampleRenderedRevoker.revoke();
        m_source = nullptr;

        if (detachPlayer)
        {
            try
            {
                m_player.Source(nullptr);
            }
            catch (hresult_error const&)
            {
            }
        }
    }

    void VideoPresenter::Shutdown()
    {
        try
        {
            m_player.Pause();
        }
        catch (hresult_error const&)
        {
        }
        CloseSource(true);
        m_mediaFailedRevoker.revoke();
        SetMediaFailedHandler(nullptr);
    }

    void VideoPresenter::UpdatePtsLead(VideoReceiver& receiver)
    {
        if (!m_source)
        {
            return;
        }
        try
        {
            auto session = m_player.PlaybackSession();
            if (session.PlaybackState() != MediaPlaybackState::Playing)
            {
                return;
            }
            receiver.UpdatePtsLead(m_sourceId, session.Position());
        }
        catch (hresult_error const&)
        {
        }
    }
}
