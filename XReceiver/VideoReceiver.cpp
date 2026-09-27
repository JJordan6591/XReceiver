#include "pch.h"
#include "VideoReceiver.h"

#include <algorithm>
#include <cstring>

#include "DebugLog.h"

using namespace winrt;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Media::Core;
using namespace winrt::Windows::Networking::Sockets;
using namespace winrt::Windows::Storage::Streams;

namespace rx
{
    namespace
    {
        constexpr uint32_t kVideoClockRate = 90000;
        constexpr size_t kMaxPacketBytes = 2048;

        double TicksToMs(int64_t ticks)
        {
            return static_cast<double>(ticks) / 10'000.0;
        }

        JitterBuffer::Config MakeJitterConfig(ReceiverSettings const& s)
        {
            JitterBuffer::Config config;
            config.reorderWindow = s.videoReorderWindow;
            config.capacity = static_cast<size_t>(s.videoReorderWindow) * 2;
            config.timeout = FromMs(s.videoReorderTimeoutMs);
            config.maxPacketBytes = kMaxPacketBytes;
            return config;
        }

        RtpAdmissionRules MakeAdmissionRules(ReceiverSettings const& s)
        {
            RtpAdmissionRules rules;
            rules.payloadType = s.videoPayloadType;
            rules.maxPacketBytes = kMaxPacketBytes;
            rules.minPayloadBytes = 1;
            rules.takeoverAfterSilence = FromMs(s.ssrcTakeoverMs);
            return rules;
        }
    }

    VideoReceiver::VideoReceiver(ReceiverSettings const& settings, std::shared_ptr<ReceiverStats> stats) :
        m_settings(settings),
        m_stats(std::move(stats)),
        m_admission(MakeAdmissionRules(settings)),
        m_jitter(MakeJitterConfig(settings)),
        m_depacketizer([this](AccessUnitPtr au) { OnAccessUnitLocked(std::move(au)); }),
        m_core(m_stats)
    {
        m_core.Configure(VideoDeliveryCore::MakeConfig(settings));
    }

    VideoReceiver::~VideoReceiver()
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

    void VideoReceiver::SetSourceRequestHandler(SourceRequestHandler handler)
    {
        m_sourceRequestHandler = std::move(handler);
    }

    IAsyncAction VideoReceiver::StartAsync(uint16_t port)
    {
        auto strong = shared_from_this();

        DatagramSocket socket;
        try
        {
            socket.Control().InboundBufferSizeInBytes(static_cast<uint32_t>(m_settings.socketBufferBytes));
        }
        catch (hresult_error const&)
        {
            Log(L"video: InboundBufferSizeInBytes not applied");
        }
        try
        {
            socket.Control().QualityOfService(SocketQualityOfService::LowLatency);
        }
        catch (hresult_error const&)
        {
        }

        std::weak_ptr<VideoReceiver> weak = strong;
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
        Log(L"video: bound UDP %u", static_cast<unsigned>(port));
    }

    void VideoReceiver::Stop()
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

        Actions actions;
        {
            std::lock_guard<std::mutex> lock(m_lock);
            m_actions = &actions;
            VideoDeliveryCore::Output out;
            m_core.Stop(out);
            CollectLocked(out, Clock::now());
            m_actions = nullptr;
        }
        RunActions(actions);
    }

    void VideoReceiver::OnMessage(DatagramSocketMessageReceivedEventArgs const& args)
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
            // Typically an ICMP-induced reset or a socket closed during shutdown.
            m_stats->Add(Stat::VideoSocketErrors);
        }
    }

    void VideoReceiver::ProcessDatagram(uint8_t const* data, size_t size, Clock::time_point now)
    {
        Actions actions;
        {
            std::lock_guard<std::mutex> lock(m_lock);
            if (m_stopped)
            {
                return;
            }
            m_actions = &actions;
            HandleDatagramLocked(data, size, now);
            m_actions = nullptr;
        }
        RunActions(actions);
    }

    void VideoReceiver::HandleDatagramLocked(uint8_t const* data, size_t size, Clock::time_point now)
    {
        m_stats->Add(Stat::VideoPackets);
        m_stats->Add(Stat::VideoBytes, static_cast<int64_t>(size));

        RtpPacketView packet;
        switch (AdmitRtpPacket(data, size, m_admission, m_tracker, now, packet))
        {
        case RtpAdmission::Invalid:
        case RtpAdmission::TooShort:
        case RtpAdmission::PartialUnit:
            m_stats->Add(Stat::VideoInvalid);
            return;
        case RtpAdmission::WrongPayloadType:
            m_stats->Add(Stat::VideoWrongPayloadType);
            return;
        case RtpAdmission::TooLarge:
            m_stats->Add(Stat::VideoTooLarge);
            return;
        case RtpAdmission::Ignored:
            m_stats->Add(Stat::VideoForeignSsrc);
            return;
        case RtpAdmission::NewStream:
            HandleNewStreamLocked();
            break;
        case RtpAdmission::Accept:
            break;
        }

        // Rejected datagrams must not keep the session in Receiving or reset the idle timer.
        m_stats->Add(Stat::VideoAccepted);
        m_stats->Add(Stat::VideoAcceptedBytes, static_cast<int64_t>(size));
        m_everReceived = true;
        m_lastPacket = now;

        if (!m_core.Gate().HasParameterSets() && LooksLikeHevcPayload(packet.payload, packet.payloadSize))
        {
            if (++m_hevcHits >= 30 && !m_hevcDetected)
            {
                m_hevcDetected = true;
                m_stats->Set(Stat::HevcDetected, 1);
                Log(L"video: stream looks like HEVC");
            }
        }

        int64_t const extSequence = m_sequence.Unwrap(packet.sequence);
        m_stats->Set(Stat::VideoSsrc, packet.ssrc);
        m_stats->Set(Stat::VideoLastSequence, packet.sequence);
        m_stats->Set(Stat::VideoLastTimestamp, packet.timestamp);

        switch (m_jitter.Insert(data, size, packet, extSequence, now, *this))
        {
        case JitterBuffer::InsertResult::DeliveredReordered:
            m_stats->Add(Stat::VideoReordered);
            break;
        case JitterBuffer::InsertResult::Duplicate:
            m_stats->Add(Stat::VideoDuplicate);
            break;
        case JitterBuffer::InsertResult::Late:
            m_stats->Add(Stat::VideoLate);
            break;
        case JitterBuffer::InsertResult::Resynced:
            m_stats->Add(Stat::VideoOutOfWindow);
            break;
        case JitterBuffer::InsertResult::TooLarge:
            m_stats->Add(Stat::VideoTooLarge);
            break;
        default:
            break;
        }
    }

    void VideoReceiver::HandleNewStreamLocked()
    {
        if (m_hadStream)
        {
            m_stats->Add(Stat::VideoStreamRestarts);
            Log(L"video: stream restart detected (SSRC %08x)", m_tracker.Ssrc());
        }
        m_hadStream = true;

        m_jitter.Reset();
        m_sequence.Reset();
        m_timestamp.Reset();
        m_transit.Reset();
        m_depacketizer.Reset();
        m_hevcHits = 0;
        m_hasLastIdr = false;
        VideoDeliveryCore::Output out;
        m_core.OnNewStream(out);
        CollectLocked(out, Clock::now());
    }

    void VideoReceiver::OnOrderedPacket(RtpPacketView const& packet, int64_t extSequence, Clock::time_point arrival)
    {
        int64_t const extTimestamp = m_timestamp.Unwrap(packet.timestamp);
        m_transit.Add(arrival, extTimestamp, kVideoClockRate);

        H264Depacketizer::PacketInfo info;
        info.ssrc = packet.ssrc;
        info.extSequence = extSequence;
        info.extTimestamp = extTimestamp;
        info.marker = packet.marker;
        info.arrival = arrival;
        m_depacketizer.OnPacket(packet.payload, packet.payloadSize, info);
    }

    void VideoReceiver::OnPacketsLost(int64_t count)
    {
        // The depacketizer sees the resulting sequence gap on the next delivered packet.
        m_stats->Add(Stat::VideoLost, count);
    }

    void VideoReceiver::OnAccessUnitLocked(AccessUnitPtr au)
    {
        PublishDepacketizerStatsLocked();

        if (!au->corrupt && au->isIdr)
        {
            m_stats->Add(Stat::IdrCount);
            if (m_hasLastIdr)
            {
                m_stats->Set(Stat::IdrIntervalMs, static_cast<int64_t>(ToMs(au->completeTime - m_lastIdr)));
            }
            m_lastIdr = au->completeTime;
            m_hasLastIdr = true;
        }

        auto const now = Clock::now();
        VideoDeliveryCore::Output out;
        m_core.OnAccessUnit(std::move(au), now, out);
        UpdateFormatStatsLocked();
        CollectLocked(out, now);
    }

    void VideoReceiver::CollectLocked(VideoDeliveryCore::Output& out, Clock::time_point now)
    {
        for (auto& completion : out.completions)
        {
            auto it = m_pending.find(completion.requestSerial);
            if (it == m_pending.end())
            {
                Log(L"video: req %llu completion without a retained request", completion.requestSerial);
                continue;
            }
            if (completion.sample)
            {
                RecordLatencyLocked(*completion.sample, now);
            }
            m_actions->completions.emplace_back(std::move(it->second), std::move(completion.sample));
            m_pending.erase(it);
        }
        if (out.openSource)
        {
            m_actions->sourceRequest = std::move(out.openSource);
        }
    }

    void VideoReceiver::RecordLatencyLocked(VideoDeliveryCore::Sample const& sample, Clock::time_point now)
    {
        int64_t const latencyUs = std::chrono::duration_cast<std::chrono::microseconds>(now - sample.au->firstPacketTime).count();
        if (latencyUs >= 0)
        {
            m_latencyUs.Push(latencyUs);
        }
        m_everSubmittedSample = true;
        m_lastSubmit = now;
    }

    MediaStreamSample VideoReceiver::CreateSample(VideoDeliveryCore::Sample& frame)
    {
        uint32_t const size = static_cast<uint32_t>(frame.au->data.size());
        Buffer buffer(size);
        std::memcpy(buffer.data(), frame.au->data.data(), size);
        buffer.Length(size);

        MediaStreamSample sample = MediaStreamSample::CreateFromBuffer(buffer, TimeSpan{ frame.pts });
        sample.Duration(TimeSpan{ frame.duration });
        sample.KeyFrame(frame.keyframe);
        sample.Discontinuous(frame.discontinuity);

        // Registered before the sample is handed to the request.
        std::weak_ptr<VideoReceiver> weak = weak_from_this();
        sample.Processed([weak, sourceId = frame.sourceId, serial = frame.sampleSerial, pts = frame.pts](MediaStreamSample const&, IInspectable const&)
            {
                if (auto self = weak.lock())
                {
                    self->OnSampleProcessed(sourceId, serial, pts);
                }
            });
        return sample;
    }

    void VideoReceiver::OnSampleProcessed(uint64_t sourceId, uint64_t sampleSerial, int64_t pts)
    {
        std::lock_guard<std::mutex> lock(m_lock);
        if (m_core.OnSampleProcessed(sourceId, sampleSerial))
        {
            Log(L"video: src %llu sample %llu processed (pts %.3f ms)", sourceId, sampleSerial, TicksToMs(pts));
        }
        else
        {
            Log(L"video: src %llu sample %llu Processed ignored (repeated or source gone)", sourceId, sampleSerial);
        }
    }

    void VideoReceiver::SubmitSample(MediaStreamSourceSampleRequest const& request, VideoDeliveryCore::Sample& frame, wchar_t const* how)
    {
        try
        {
            request.Sample(CreateSample(frame));
        }
        catch (hresult_error const&)
        {
            // The pipeline never received it; release its accounting like a Processed callback.
            std::lock_guard<std::mutex> lock(m_lock);
            m_core.OnSampleProcessed(frame.sourceId, frame.sampleSerial);
            throw;
        }
        Log(L"video: src %llu req %llu -> sample %llu submitted %s (pts %.3f ms, dur %.3f ms, %zu bytes%s%s)",
            frame.sourceId, frame.requestSerial, frame.sampleSerial, how,
            TicksToMs(frame.pts), TicksToMs(frame.duration), frame.au->data.size(),
            frame.keyframe ? L", key" : L"", frame.discontinuity ? L", discontinuous" : L"");
    }

    void VideoReceiver::RunActions(Actions& actions)
    {
        for (auto& [pending, frame] : actions.completions)
        {
            uint64_t const serial = pending.requestSerial;
            try
            {
                if (frame)
                {
                    SubmitSample(pending.request, *frame, L"via deferral");
                }
                pending.deferral.Complete();
                Log(L"video: src %llu req %llu deferral completed%s", pending.sourceId, serial, frame ? L"" : L" without sample (end of stream)");
            }
            catch (hresult_error const& e)
            {
                m_stats->Add(Stat::SampleErrors);
                Log(L"video: src %llu req %llu completing deferral failed 0x%08x", pending.sourceId, serial, static_cast<uint32_t>(e.code()));
            }
        }

        if (actions.sourceRequest && m_sourceRequestHandler)
        {
            m_sourceRequestHandler(actions.sourceRequest->first, actions.sourceRequest->second);
        }
    }

    void VideoReceiver::OnSourceStarting(uint64_t sourceId, TimeSpan actualStart)
    {
        std::lock_guard<std::mutex> lock(m_lock);
        m_core.OnStarting(sourceId, actualStart.count());
    }

    void VideoReceiver::OnSampleRequested(uint64_t sourceId, MediaStreamSourceSampleRequest const& request)
    {
        Actions actions;
        std::optional<VideoDeliveryCore::Sample> immediate;
        uint64_t requestSerial = 0;
        {
            std::lock_guard<std::mutex> lock(m_lock);
            requestSerial = ++m_requestSerial;
            if (m_stopped)
            {
                Log(L"video: src %llu req %llu SampleRequested after stop: ending it", sourceId, requestSerial);
                return;
            }
            auto const now = Clock::now();
            m_actions = &actions;
            VideoDeliveryCore::Output out;
            switch (m_core.OnRequest(sourceId, requestSerial, now, immediate, out))
            {
            case VideoDeliveryCore::RequestResult::Immediate:
                RecordLatencyLocked(*immediate, now);
                break;
            case VideoDeliveryCore::RequestResult::Retained:
                // Stored under the same lock hold, before any frame can be matched to it.
                m_pending.emplace(requestSerial, Pending{ request, request.GetDeferral(), sourceId, requestSerial });
                break;
            case VideoDeliveryCore::RequestResult::Stale:
                break;
            }
            CollectLocked(out, now);
            m_actions = nullptr;
        }

        if (immediate)
        {
            try
            {
                SubmitSample(request, *immediate, L"immediately");
            }
            catch (hresult_error const& e)
            {
                m_stats->Add(Stat::SampleErrors);
                Log(L"video: src %llu req %llu setting sample failed 0x%08x", sourceId, requestSerial, static_cast<uint32_t>(e.code()));
            }
        }
        RunActions(actions);
    }

    void VideoReceiver::OnSampleRendered(uint64_t sourceId)
    {
        std::lock_guard<std::mutex> lock(m_lock);
        m_core.OnSampleRendered(sourceId);
    }

    void VideoReceiver::OnSourceClosed(uint64_t sourceId, int32_t reason)
    {
        Actions actions;
        {
            std::lock_guard<std::mutex> lock(m_lock);
            Log(L"video: src %llu Closed (reason %d, current %llu, %llu samples delivered, %zu pending)",
                sourceId, reason, m_core.SourceId(), sourceId == m_core.SourceId() ? m_core.DeliveredToSource() : 0ull,
                sourceId == m_core.SourceId() ? m_core.PendingCount() : size_t{ 0 });
            if (m_stopped)
            {
                return;
            }
            m_actions = &actions;
            VideoDeliveryCore::Output out;
            m_core.OnSourceClosed(sourceId, out);
            CollectLocked(out, Clock::now());
            m_actions = nullptr;
        }
        RunActions(actions);
    }

    void VideoReceiver::UpdatePtsLead(uint64_t sourceId, TimeSpan position)
    {
        std::lock_guard<std::mutex> lock(m_lock);
        if (sourceId != m_core.SourceId() || !m_core.HasLastPts())
        {
            return;
        }
        m_stats->Set(Stat::PtsLeadUs, (m_core.LastPts() - position.count()) / 10);
    }

    void VideoReceiver::RequestNewSource()
    {
        Actions actions;
        {
            std::lock_guard<std::mutex> lock(m_lock);
            m_actions = &actions;
            VideoDeliveryCore::Output out;
            m_core.RequestNewSource(out);
            CollectLocked(out, Clock::now());
            m_actions = nullptr;
        }
        RunActions(actions);
    }

    void VideoReceiver::SetLossPolicy(LossPolicy policy)
    {
        std::lock_guard<std::mutex> lock(m_lock);
        m_settings.lossPolicy = policy;
        m_core.SetLossPolicy(policy);
    }

    VideoActivity VideoReceiver::Activity() const
    {
        std::lock_guard<std::mutex> lock(m_lock);
        VideoActivity activity;
        activity.everReceived = m_everReceived;
        activity.lastPacket = m_lastPacket;
        activity.everSubmitted = m_everSubmittedSample;
        activity.lastSubmit = m_lastSubmit;
        activity.waitingForKeyframe = m_core.Gate().IsWaiting();
        activity.sourceActive = m_core.CurrentPhase() != VideoDeliveryCore::Phase::None;
        activity.hevcDetected = m_hevcDetected;
        return activity;
    }

    void VideoReceiver::Poll(Clock::time_point now)
    {
        Actions actions;
        {
            std::lock_guard<std::mutex> lock(m_lock);
            if (m_stopped)
            {
                return;
            }
            m_actions = &actions;
            m_jitter.Poll(now, *this);
            if (++m_pollCount % 25 == 0)
            {
                UpdateLatencyStatsLocked();
                m_stats->Set(Stat::VideoJitterUs, static_cast<int64_t>(m_transit.JitterMs() * 1000.0));
                PublishDepacketizerStatsLocked();
                PublishAgesLocked(now);
            }
            bool const requesting = m_core.HasRequest() && m_core.CurrentPhase() != VideoDeliveryCore::Phase::None;
            m_stats->Set(Stat::LastRequestAgeMs, requesting
                ? std::chrono::duration_cast<std::chrono::milliseconds>(now - m_core.LastRequest()).count()
                : -1);
            m_actions = nullptr;
        }
        RunActions(actions);
    }

    void VideoReceiver::PublishDepacketizerStatsLocked()
    {
        auto const& c = m_depacketizer.GetCounters();
        auto set = [this](Stat s, uint64_t v) { m_stats->Set(s, static_cast<int64_t>(v)); };
        set(Stat::VideoSequenceGaps, c.sequenceGaps);
        set(Stat::AccessUnitsComplete, c.completeAccessUnits);
        set(Stat::AccessUnitsIncomplete, c.incompleteAccessUnits);
        set(Stat::AccessUnitsMissingMarker, c.missingMarker);
        set(Stat::FuaErrors, c.fuaErrors);
        set(Stat::StapaErrors, c.stapaErrors);
        set(Stat::UnsupportedNal, c.unsupportedNal);
        set(Stat::MalformedPayload, c.emptyPayload + c.oversize + c.forbiddenBit);
    }

    void VideoReceiver::UpdateFormatStatsLocked()
    {
        SpsInfo const& sps = m_core.Gate().Sps();
        if (!sps.valid)
        {
            return;
        }
        m_stats->Set(Stat::Width, sps.width);
        m_stats->Set(Stat::Height, sps.height);
        m_stats->Set(Stat::Profile, sps.profileIdc);
        m_stats->Set(Stat::Level, sps.levelIdc);
        m_stats->Set(Stat::PocType, sps.pocType);
        m_stats->Set(Stat::BitstreamRestriction, sps.bitstreamRestriction ? 1 : 0);
        m_stats->Set(Stat::MaxDecFrameBuffering, sps.maxDecFrameBuffering);
        if (m_hevcDetected)
        {
            m_hevcDetected = false;
            m_stats->Set(Stat::HevcDetected, 0);
        }
    }

    void VideoReceiver::PublishAgesLocked(Clock::time_point now)
    {
        int64_t age = -1;
        AgeMs(m_everReceived, m_lastPacket, now, age);
        m_stats->Set(Stat::VideoPacketAgeMs, age);
        AgeMs(m_everSubmittedSample, m_lastSubmit, now, age);
        m_stats->Set(Stat::VideoSubmitAgeMs, age);

        m_idrWait.SetActive(m_core.Gate().IsWaiting(), now);
        m_stats->Set(Stat::IdrWaitCurrentMs, m_idrWait.CurrentMs());
        m_stats->Set(Stat::IdrWaitLongestMs, m_idrWait.LongestMs());
    }

    void VideoReceiver::UpdateLatencyStatsLocked()
    {
        int64_t p50 = 0;
        int64_t p95 = 0;
        if (!m_latencyUs.Percentile(50, p50))
        {
            return;
        }
        m_stats->Set(Stat::ReceiveToSubmitP50Us, p50);
        if (m_latencyUs.Percentile(95, p95))
        {
            m_stats->Set(Stat::ReceiveToSubmitP95Us, p95);
        }
    }
}
