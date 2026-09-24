#include "pch.h"
#include "PocSpikes.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstring>
#include <iterator>
#include <string>

#include "DebugLog.h"
#include "H264AccessUnit.h"
#include "H264Bitstream.h"
#include "L16Convert.h"

using namespace winrt;
using namespace winrt::Windows::ApplicationModel;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Media;
using namespace winrt::Windows::Media::Audio;
using namespace winrt::Windows::Media::Core;
using namespace winrt::Windows::Media::MediaProperties;
using namespace winrt::Windows::Media::Playback;
using namespace winrt::Windows::Media::Render;
using namespace winrt::Windows::Networking::Sockets;
using namespace winrt::Windows::Storage;
using namespace winrt::Windows::Storage::Streams;

namespace rx
{
    namespace
    {
        std::wstring Format(wchar_t const* format, ...)
        {
            wchar_t buffer[512];
            va_list args;
            va_start(args, format);
            _vsnwprintf_s(buffer, _countof(buffer), _TRUNCATE, format, args);
            va_end(args);
            return buffer;
        }
    }

    // ---------------------------------------------------------------- PocSocketCounter

    IAsyncAction PocSocketCounter::StartAsync(uint16_t videoPort, uint16_t audioPort)
    {
        auto strong = shared_from_this();

        auto attach = [](DatagramSocket& socket, DatagramSocket::MessageReceived_revoker& revoker, std::shared_ptr<Counter> counter)
        {
            socket = DatagramSocket();
            try
            {
                socket.Control().InboundBufferSizeInBytes(2 * 1024 * 1024);
            }
            catch (hresult_error const&)
            {
            }
            revoker = socket.MessageReceived(auto_revoke,
                [counter](DatagramSocket const&, DatagramSocketMessageReceivedEventArgs const& args)
                {
                    try
                    {
                        auto reader = args.GetDataReader();
                        uint32_t const length = reader.UnconsumedBufferLength();
                        IBuffer buffer = reader.ReadBuffer(length);
                        counter->packets++;
                        counter->bytes += length;
                        if (length >= 4)
                        {
                            int32_t const sequence = (buffer.data()[2] << 8) | buffer.data()[3];
                            int32_t const last = counter->lastSequence.exchange(sequence);
                            if (last >= 0 && ((last + 1) & 0xFFFF) != sequence)
                            {
                                counter->gaps++;
                            }
                        }
                    }
                    catch (hresult_error const&)
                    {
                    }
                });
        };

        attach(m_video, m_videoRevoker, m_videoCount);
        attach(m_audio, m_audioRevoker, m_audioCount);

        try
        {
            co_await m_video.BindServiceNameAsync(to_hstring(videoPort));
            co_await m_audio.BindServiceNameAsync(to_hstring(audioPort));
        }
        catch (hresult_error const& e)
        {
            m_error = L"bind failed: " + e.message();
        }
    }

    void PocSocketCounter::Stop()
    {
        m_videoRevoker.revoke();
        m_audioRevoker.revoke();
        for (auto* socket : { &m_video, &m_audio })
        {
            if (*socket)
            {
                try
                {
                    socket->Close();
                }
                catch (hresult_error const&)
                {
                }
                *socket = nullptr;
            }
        }
    }

    hstring PocSocketCounter::Status() const
    {
        std::wstring text = Format(L"PoC sockets  video: %lld pkts, %lld KB, %lld seq gaps | audio: %lld pkts, %lld KB, %lld seq gaps",
            m_videoCount->packets.load(), m_videoCount->bytes.load() / 1024, m_videoCount->gaps.load(),
            m_audioCount->packets.load(), m_audioCount->bytes.load() / 1024, m_audioCount->gaps.load());
        if (!m_error.empty())
        {
            text += L"\n  ";
            text += m_error.c_str();
        }
        return hstring(text);
    }

    // ---------------------------------------------------------------- PocClipPlayer

    IAsyncAction PocClipPlayer::StartAsync(MediaPlayer player)
    {
        auto strong = shared_from_this();
        m_player = player;

        IBuffer fileBuffer{ nullptr };
        try
        {
            StorageFolder folder = Package::Current().InstalledLocation();
            StorageFile file = co_await folder.GetFileAsync(L"Assets\\poc_1080p60.h264");
            fileBuffer = co_await FileIO::ReadBufferAsync(file);
        }
        catch (hresult_error const& e)
        {
            m_error = L"Assets\\poc_1080p60.h264 not found in the package (" + e.message() + L"). See readme.txt.";
        }
        if (!fileBuffer)
        {
            co_return;
        }

        // Split the Annex B stream into NAL units, then group them into access units.
        uint8_t const* data = fileBuffer.data();
        size_t const size = fileBuffer.Length();
        std::vector<std::pair<size_t, size_t>> nals;
        size_t nalStart = SIZE_MAX;
        for (size_t i = 0; i + 3 <= size; ++i)
        {
            if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1)
            {
                if (nalStart != SIZE_MAX)
                {
                    size_t end = i;
                    while (end > nalStart && data[end - 1] == 0)
                    {
                        --end;
                    }
                    nals.emplace_back(nalStart, end - nalStart);
                }
                nalStart = i + 3;
                i += 2;
            }
        }
        if (nalStart != SIZE_MAX && nalStart < size)
        {
            nals.emplace_back(nalStart, size - nalStart);
        }

        ClipFrame current;
        bool currentHasSlice = false;
        SpsInfo lastSps;
        auto flush = [&]()
        {
            if (currentHasSlice)
            {
                m_frames.push_back(std::move(current));
            }
            current = ClipFrame{};
            currentHasSlice = false;
        };
        for (auto const& [offset, length] : nals)
        {
            if (length == 0)
            {
                continue;
            }
            uint8_t const* nal = data + offset;
            uint8_t const type = nal[0] & 0x1F;
            bool const isSlice = type >= 1 && type <= 5;
            bool boundary = false;
            if (type == h264::kNalAud || type == h264::kNalSps || type == h264::kNalPps || type == h264::kNalSei)
            {
                boundary = currentHasSlice;
            }
            else if (isSlice && currentHasSlice)
            {
                boundary = ReadFirstMbInSlice(nal, length) == 0;
            }
            if (boundary)
            {
                flush();
            }
            if (type == h264::kNalAud)
            {
                continue;
            }
            if (type == h264::kNalSps)
            {
                SpsInfo info;
                if (ParseSps(nal, length, info))
                {
                    if (lastSps.valid && (info.width != lastSps.width || info.height != lastSps.height))
                    {
                        ++m_resolutionChanges;
                    }
                    if (!lastSps.valid)
                    {
                        m_width = info.width;
                        m_height = info.height;
                    }
                    lastSps = info;
                }
            }
            current.data.insert(current.data.end(), std::begin(h264::kStartCode), std::end(h264::kStartCode));
            current.data.insert(current.data.end(), nal, nal + length);
            if (type == h264::kNalSliceIdr)
            {
                current.keyframe = true;
            }
            currentHasSlice = currentHasSlice || isSlice;
        }
        flush();

        auto firstKey = std::find_if(m_frames.begin(), m_frames.end(), [](ClipFrame const& f) { return f.keyframe; });
        if (firstKey == m_frames.end() || !lastSps.valid)
        {
            m_error = L"Clip has no IDR frame or no valid SPS.";
            m_frames.clear();
            co_return;
        }
        m_frames.erase(m_frames.begin(), firstKey);

        VideoEncodingProperties properties = VideoEncodingProperties::CreateH264();
        properties.Width(m_width);
        properties.Height(m_height);
        properties.FrameRate().Numerator(60);
        properties.FrameRate().Denominator(1);
        MediaStreamSource source(VideoStreamDescriptor{ properties });
        source.CanSeek(false);
        source.BufferTime(TimeSpan{ 0 });
        source.IsLive(true);

        std::weak_ptr<PocClipPlayer> weak = strong;
        m_startingRevoker = source.Starting(auto_revoke, [](MediaStreamSource const&, MediaStreamSourceStartingEventArgs const& args)
            {
                args.Request().SetActualStartPosition(TimeSpan{ 0 });
            });
        m_requestRevoker = source.SampleRequested(auto_revoke, [weak](MediaStreamSource const&, MediaStreamSourceSampleRequestedEventArgs const& args)
            {
                if (auto self = weak.lock())
                {
                    self->OnSampleRequested(args);
                }
            });
        m_renderedRevoker = source.SampleRendered(auto_revoke, [weak](MediaStreamSource const&, MediaStreamSourceSampleRenderedEventArgs const& args)
            {
                if (auto self = weak.lock())
                {
                    self->m_sampleLagUs = std::chrono::duration_cast<std::chrono::microseconds>(args.SampleLag()).count();
                }
            });
        m_source = source;

        try
        {
            m_player.RealTimePlayback(true);
            m_realTimePlayback = m_player.RealTimePlayback();
        }
        catch (hresult_error const&)
        {
            m_realTimePlayback = false;
        }
        m_started = Clock::now();
        m_player.Source(MediaSource::CreateFromMediaStreamSource(source));
        m_player.Play();
        Log(L"poc clip: %zu frames, %ux%u", m_frames.size(), m_width, m_height);
    }

    void PocClipPlayer::OnSampleRequested(MediaStreamSourceSampleRequestedEventArgs const& args)
    {
        std::lock_guard<std::mutex> lock(m_lock);
        if (m_frames.empty())
        {
            return;
        }
        ClipFrame const& frame = m_frames[m_next];
        m_next = (m_next + 1) % m_frames.size();

        int64_t const ptsTicks = m_delivered * 10'000'000 / 60;
        ++m_delivered;
        m_lastPtsTicks = ptsTicks;

        uint32_t const size = static_cast<uint32_t>(frame.data.size());
        Buffer buffer(size);
        std::memcpy(buffer.data(), frame.data.data(), size);
        buffer.Length(size);
        MediaStreamSample sample = MediaStreamSample::CreateFromBuffer(buffer, TimeSpan{ ptsTicks });
        sample.Duration(TimeSpan{ 10'000'000 / 60 });
        sample.KeyFrame(frame.keyframe);
        args.Request().Sample(sample);
    }

    void PocClipPlayer::OnUiTick()
    {
        if (!m_source || !m_player)
        {
            return;
        }
        try
        {
            auto session = m_player.PlaybackSession();
            if (session.PlaybackState() == MediaPlaybackState::Playing)
            {
                int64_t lastPts = 0;
                {
                    std::lock_guard<std::mutex> lock(m_lock);
                    lastPts = m_lastPtsTicks;
                }
                m_ptsLeadUs = (lastPts - session.Position().count()) / 10;
            }
        }
        catch (hresult_error const&)
        {
        }
    }

    void PocClipPlayer::Stop()
    {
        m_requestRevoker.revoke();
        m_renderedRevoker.revoke();
        m_startingRevoker.revoke();
        if (m_player)
        {
            try
            {
                m_player.Pause();
                m_player.Source(nullptr);
            }
            catch (hresult_error const&)
            {
            }
        }
        m_source = nullptr;
    }

    hstring PocClipPlayer::Status() const
    {
        if (!m_error.empty())
        {
            return L"PoC clip: " + m_error;
        }
        int64_t delivered = 0;
        {
            std::lock_guard<std::mutex> lock(m_lock);
            delivered = m_delivered;
        }
        double const elapsed = std::chrono::duration<double>(Clock::now() - m_started).count();
        double const fps = elapsed > 0.5 ? delivered / elapsed : 0.0;
        return hstring(Format(L"PoC clip: %ux%u, %zu AUs, %zu resolution changes, RealTimePlayback %s\n"
                              L"  delivered %lld (%.1f fps), sample lag %.1f ms, pts lead over position %.1f ms",
            m_width, m_height, m_frames.size(), m_resolutionChanges, m_realTimePlayback ? L"on" : L"off",
            delivered, fps, m_sampleLagUs.load() / 1000.0, m_ptsLeadUs.load() / 1000.0));
    }

    // ---------------------------------------------------------------- PocToneTest

    namespace
    {
        // Same IID as Windows.Foundation's IMemoryBufferByteAccess in <MemoryBuffer.h>; declared
        // here so the ABI ::Windows namespace does not collide with winrt::Windows.
        struct __declspec(uuid("5b0d3235-4dba-4d44-865e-8f1d0e4fd04d")) __declspec(novtable) IMemoryBufferByteAccess : ::IUnknown
        {
            virtual HRESULT __stdcall GetBuffer(uint8_t** value, uint32_t* capacity) = 0;
        };

        constexpr double kTwoPi = 6.283185307179586476925286766559;
        constexpr double kToneHz = 440.0;
        constexpr int32_t kSlightlyLargerQuantum = 240;     // 5 ms at 48 kHz

        PocToneTest::Config const kToneTests[] = {
            { L"A native rate, LowestLatency", PocToneTest::ToneRate::GraphNative, QuantumSizeSelectionMode::LowestLatency, 0 },
            { L"B 44.1 kHz, LowestLatency", PocToneTest::ToneRate::Input44k, QuantumSizeSelectionMode::LowestLatency, 0 },
            { L"A native rate, ClosestToDesired 240", PocToneTest::ToneRate::GraphNative, QuantumSizeSelectionMode::ClosestToDesired, kSlightlyLargerQuantum },
            { L"B 44.1 kHz, ClosestToDesired 240", PocToneTest::ToneRate::Input44k, QuantumSizeSelectionMode::ClosestToDesired, kSlightlyLargerQuantum },
            { L"A native rate, SystemDefault", PocToneTest::ToneRate::GraphNative, QuantumSizeSelectionMode::SystemDefault, 0 },
            { L"B 44.1 kHz, SystemDefault", PocToneTest::ToneRate::Input44k, QuantumSizeSelectionMode::SystemDefault, 0 },
        };

        wchar_t const* ProcessingName(AudioProcessing processing)
        {
            return processing == AudioProcessing::Raw ? L"Raw" : L"Default";
        }

        wchar_t const* QuantumModeName(QuantumSizeSelectionMode mode)
        {
            switch (mode)
            {
            case QuantumSizeSelectionMode::LowestLatency: return L"LowestLatency";
            case QuantumSizeSelectionMode::ClosestToDesired: return L"ClosestToDesired";
            default: return L"SystemDefault";
            }
        }

        wchar_t const* GraphErrorName(int32_t error)
        {
            switch (static_cast<AudioGraphUnrecoverableError>(error))
            {
            case AudioGraphUnrecoverableError::None: return L"none";
            case AudioGraphUnrecoverableError::AudioDeviceLost: return L"AudioDeviceLost";
            case AudioGraphUnrecoverableError::AudioSessionDisconnected: return L"AudioSessionDisconnected";
            default: return L"UnknownFailure";
            }
        }

        void StoreMax(std::atomic<int64_t>& target, int64_t value)
        {
            if (value > target.load(std::memory_order_relaxed))
            {
                target.store(value, std::memory_order_relaxed);
            }
        }
    }

    size_t PocToneTest::TestCount()
    {
        return std::size(kToneTests);
    }

    PocToneTest::Config PocToneTest::TestConfig(size_t index)
    {
        return kToneTests[std::min(index, std::size(kToneTests) - 1)];
    }

    IAsyncAction PocToneTest::StartAsync(Config config, size_t index)
    {
        auto strong = shared_from_this();
        m_config = config;
        m_index = index;
        m_startedAt = Clock::now();
        std::weak_ptr<PocToneTest> weak = strong;

        try
        {
            AudioGraphSettings settings(AudioRenderCategory::Media);
            settings.QuantumSizeSelectionMode(config.quantumMode);
            if (config.quantumMode == QuantumSizeSelectionMode::ClosestToDesired)
            {
                settings.DesiredSamplesPerQuantum(config.desiredSamplesPerQuantum);
            }
            m_requestedProcessing = AudioProcessing::Raw;
            settings.DesiredRenderDeviceAudioProcessing(m_requestedProcessing);

            CreateAudioGraphResult result = co_await AudioGraph::CreateAsync(settings);
            if (result.Status() != AudioGraphCreationStatus::Success)
            {
                m_requestedProcessing = AudioProcessing::Default;
                settings.DesiredRenderDeviceAudioProcessing(m_requestedProcessing);
                result = co_await AudioGraph::CreateAsync(settings);
            }
            if (result.Status() != AudioGraphCreationStatus::Success)
            {
                Fail(L"AudioGraph creation failed (status " + to_hstring(static_cast<int32_t>(result.Status())) + L")");
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
                Release();
                co_return;
            }

            CreateAudioDeviceOutputNodeResult outputResult = co_await m_graph.CreateDeviceOutputNodeAsync();
            if (m_stopRequested)
            {
                Release();
                co_return;
            }
            if (outputResult.Status() != AudioDeviceNodeCreationStatus::Success)
            {
                Fail(L"Audio output node creation failed (status " + to_hstring(static_cast<int32_t>(outputResult.Status())) + L")");
                Release();
                co_return;
            }
            m_output = outputResult.DeviceOutputNode();

            AudioEncodingProperties const graphProperties = m_graph.EncodingProperties();
            m_graphRate = graphProperties.SampleRate();
            m_graphChannels = graphProperties.ChannelCount();
            m_samplesPerQuantum = m_graph.SamplesPerQuantum();
            m_latencySamples = m_graph.LatencyInSamples();
            m_actualProcessing = m_graph.RenderDeviceAudioProcessing();

            uint32_t const toneRate = config.rate == ToneRate::GraphNative ? m_graphRate : kL16SampleRate;
            AudioEncodingProperties requested = AudioEncodingProperties::CreatePcm(toneRate, kL16Channels, 32);
            requested.Subtype(MediaEncodingSubtypes::Float());
            m_input = m_graph.CreateFrameInputNode(requested);
            m_input.Stop();

            // Render with the node's actual format, not the one requested.
            AudioEncodingProperties const actual = m_input.EncodingProperties();
            m_nodeRate = actual.SampleRate();
            m_nodeChannels = actual.ChannelCount();
            m_nodeBits = actual.BitsPerSample();
            m_nodeSubtype = actual.Subtype();
            if (m_nodeSubtype != MediaEncodingSubtypes::Float() || m_nodeBits != 32 || m_nodeChannels == 0 || m_nodeRate == 0)
            {
                Fail(L"Frame input node format is " + m_nodeSubtype + L" " + to_hstring(m_nodeBits) + L"-bit, " +
                     to_hstring(m_nodeChannels) + L" ch, " + to_hstring(m_nodeRate) + L" Hz; expected 32-bit Float");
                Release();
                co_return;
            }
            m_phase = 0.0;
            m_phaseStep = kTwoPi * kToneHz / m_nodeRate;
            m_quantumNs = m_graphRate > 0 ? m_samplesPerQuantum * 1e9 / m_graphRate : 0.0;

            m_input.AddOutgoingConnection(m_output);
            m_quantumRevoker = m_input.QuantumStarted(auto_revoke,
                [weak](AudioFrameInputNode const& sender, FrameInputNodeQuantumStartedEventArgs const& args)
                {
                    if (auto self = weak.lock())
                    {
                        self->OnQuantumStarted(sender, args);
                    }
                });
            m_completedRevoker = m_input.AudioFrameCompleted(auto_revoke,
                [weak](AudioFrameInputNode const&, AudioFrameCompletedEventArgs const&)
                {
                    if (auto self = weak.lock())
                    {
                        self->m_framesCompleted.fetch_add(1, std::memory_order_relaxed);
                    }
                });

            // Connected and subscribed input node first, then the graph clock.
            m_input.Start();
            m_graph.Start();
            m_latencySamples = m_graph.LatencyInSamples();
            m_running = true;
            Log(L"poc tone: %s | graph %u Hz %u ch, quantum %d, latency %d, processing %s | node %u Hz %u ch",
                config.name, m_graphRate, m_graphChannels, m_samplesPerQuantum, m_latencySamples,
                ProcessingName(m_actualProcessing), m_nodeRate, m_nodeChannels);
        }
        catch (hresult_error const& e)
        {
            Fail(L"Tone start failed: " + e.message());
            Release();
        }
    }

    // Real-time audio thread: no locks, no logging, no UI or file access. The only allocation
    // is the AudioFrame itself, which the API requires per submission.
    void PocToneTest::OnQuantumStarted(AudioFrameInputNode const& sender, FrameInputNodeQuantumStartedEventArgs const& args)
    {
        auto const start = Clock::now();
        if (m_stopRequested.load(std::memory_order_relaxed))
        {
            return;
        }

        int64_t const n = m_callbacks.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n > 1)
        {
            int64_t const intervalNs = std::chrono::duration_cast<std::chrono::nanoseconds>(start - m_lastCallback).count();
            StoreMax(m_maxIntervalUs, intervalNs / 1000);
            if (n > kAnchorCallback && static_cast<double>(intervalNs) > m_quantumNs * 1.5)
            {
                m_lateCallbacks.fetch_add(1, std::memory_order_relaxed);
            }
        }
        m_lastCallback = start;

        // Wall time minus rendered time. Pre-roll bursts push it negative, so the baseline is its
        // most-ahead value; growth above that is time the graph did not render (device starvation)
        // plus slow clock drift of a few ms per minute.
        if (n == kAnchorCallback)
        {
            m_anchor = start;
            m_deficitMinUs = 0;
        }
        else if (n > kAnchorCallback)
        {
            double const elapsedNs = static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(start - m_anchor).count());
            double const renderedNs = static_cast<double>(n - kAnchorCallback) * m_quantumNs;
            int64_t const deficitUs = static_cast<int64_t>((elapsedNs - renderedNs) / 1000.0);
            m_deficitMinUs = std::min(m_deficitMinUs, deficitUs);
            int64_t const stallUs = deficitUs - m_deficitMinUs;
            m_stallUs.store(stallUs, std::memory_order_relaxed);
            StoreMax(m_stallMaxUs, stallUs);
        }

        int64_t const queued = static_cast<int64_t>(sender.QueuedSampleCount());
        m_queuedLatest.store(queued, std::memory_order_relaxed);
        StoreMax(m_queuedMax, queued);

        // RequiredSamples counts frames per channel at the input node's rate.
        int32_t const required = args.RequiredSamples();
        m_requiredLatest.store(required, std::memory_order_relaxed);
        int64_t const currentMin = m_requiredMin.load(std::memory_order_relaxed);
        if (currentMin < 0 || required < currentMin)
        {
            m_requiredMin.store(required, std::memory_order_relaxed);
        }
        StoreMax(m_requiredMax, required);

        if (required <= 0)
        {
            m_zeroSampleCallbacks.fetch_add(1, std::memory_order_relaxed);
        }
        else
        {
            try
            {
                uint32_t const frames = static_cast<uint32_t>(required);
                uint32_t const channels = m_nodeChannels;
                uint32_t const bytes = frames * channels * static_cast<uint32_t>(sizeof(float));
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
                        float* out = reinterpret_cast<float*>(data);
                        float const amplitude = static_cast<float>(m_config.amplitude);
                        for (uint32_t i = 0; i < frames; ++i)
                        {
                            float const value = amplitude * static_cast<float>(std::sin(m_phase));
                            for (uint32_t c = 0; c < channels; ++c)
                            {
                                out[i * channels + c] = value;
                            }
                            m_phase += m_phaseStep;
                            if (m_phase >= kTwoPi)
                            {
                                m_phase -= kTwoPi;
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
                    m_framesSubmitted.fetch_add(1, std::memory_order_relaxed);
                    m_samplesSubmitted.fetch_add(required, std::memory_order_relaxed);
                }
                else
                {
                    m_badBuffers.fetch_add(1, std::memory_order_relaxed);
                }
            }
            catch (hresult_error const& e)
            {
                m_callbackErrors.fetch_add(1, std::memory_order_relaxed);
                m_lastCallbackHr.store(static_cast<uint32_t>(e.code()), std::memory_order_relaxed);
            }
        }

        int64_t const tookUs = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count();
        m_callbackTotalUs.fetch_add(tookUs, std::memory_order_relaxed);
        StoreMax(m_callbackMaxUs, tookUs);
        if (static_cast<double>(tookUs) * 1000.0 > m_quantumNs * 0.5)
        {
            m_slowCallbacks.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void PocToneTest::OnGraphError(AudioGraphUnrecoverableError error)
    {
        m_graphErrors.fetch_add(1, std::memory_order_relaxed);
        m_lastGraphError.store(static_cast<int32_t>(error), std::memory_order_relaxed);
        m_stopRequested = true;
        Log(L"poc tone: AudioGraph unrecoverable error %s", GraphErrorName(static_cast<int32_t>(error)));
    }

    void PocToneTest::Fail(hstring const& message)
    {
        Log(L"poc tone: %s", message.c_str());
        m_error = message;
    }

    void PocToneTest::Release()
    {
        m_quantumRevoker.revoke();
        m_completedRevoker.revoke();
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

    void PocToneTest::Stop()
    {
        m_stopRequested = true;
        m_quantumRevoker.revoke();
        m_completedRevoker.revoke();
        if (m_graph)
        {
            try
            {
                m_graph.Stop();
                m_completedQuanta = m_graph.CompletedQuantumCount();
                m_latencySamples = m_graph.LatencyInSamples();
            }
            catch (hresult_error const&)
            {
            }
        }
        Release();
        if (m_running.exchange(false))
        {
            m_stoppedAt = Clock::now();
        }
    }

    hstring PocToneTest::Status() const
    {
        bool const running = m_running.load();
        auto const end = running || m_stoppedAt == Clock::time_point{} ? Clock::now() : m_stoppedAt;
        double const elapsedS = std::chrono::duration<double>(end - m_startedAt).count();

        std::wstring text = Format(L"PoC tone %zu/%zu: %s  [%s, %.0f s]  press PoC: tone for %s",
            m_index + 1, TestCount(), m_config.name, running ? L"running" : L"stopped", elapsedS,
            m_index + 1 < TestCount() ? L"the next test" : L"stop");
        if (!m_error.empty())
        {
            text += L"\n  ERROR: ";
            text += m_error.c_str();
        }
        int64_t const graphErrors = m_graphErrors.load();
        if (graphErrors > 0)
        {
            text += Format(L"\n  GRAPH ERROR: %s (graph stopped rendering; press PoC: tone to move on)",
                GraphErrorName(m_lastGraphError.load()));
        }
        if (m_graphRate == 0)
        {
            return hstring(text);
        }

        double const quantumMs = m_quantumNs / 1e6;
        if (m_graph)
        {
            try
            {
                m_completedQuanta = m_graph.CompletedQuantumCount();
                m_latencySamples = m_graph.LatencyInSamples();
            }
            catch (hresult_error const&)
            {
            }
        }
        int64_t const callbacks = m_callbacks.load();
        int64_t const samples = m_samplesSubmitted.load();
        int64_t const submitted = m_framesSubmitted.load();
        int64_t const completed = m_framesCompleted.load();
        double const consumedHz = callbacks > 0 && m_quantumNs > 0 ? samples / (callbacks * m_quantumNs / 1e9) : 0.0;
        double const avgCallbackUs = callbacks > 0 ? static_cast<double>(m_callbackTotalUs.load()) / callbacks : 0.0;

        text += Format(L"\n  graph %u Hz %u ch  quantum %d samples (%.2f ms, %s)  latency %d samples (%.2f ms)",
            m_graphRate, m_graphChannels, m_samplesPerQuantum, quantumMs, QuantumModeName(m_config.quantumMode),
            m_latencySamples, m_latencySamples * 1000.0 / m_graphRate);
        text += Format(L"\n  render processing %s (requested %s)  input node %u Hz %u ch %s %u-bit",
            ProcessingName(m_actualProcessing), ProcessingName(m_requestedProcessing),
            m_nodeRate, m_nodeChannels, m_nodeSubtype.c_str(), m_nodeBits);
        text += Format(L"\n  quantum callbacks %lld  graph quanta %llu  late %lld (>%.2f ms, max gap %.2f ms)  zero-sample %lld",
            callbacks, m_completedQuanta, m_lateCallbacks.load(), quantumMs * 1.5, m_maxIntervalUs.load() / 1000.0,
            m_zeroSampleCallbacks.load());
        text += Format(L"\n  callback time avg %.0f us, max %lld us, slow (>50%% of quantum) %lld",
            avgCallbackUs, m_callbackMaxUs.load(), m_slowCallbacks.load());
        text += Format(L"\n  RequiredSamples min %lld max %lld latest %lld  QueuedSampleCount latest %lld max %lld",
            m_requiredMin.load(), m_requiredMax.load(), m_requiredLatest.load(), m_queuedLatest.load(), m_queuedMax.load());
        text += Format(L"\n  frames submitted %lld (%lld samples, %.1f Hz consumed)  AudioFrameCompleted %lld  outstanding %lld",
            submitted, samples, consumedHz, completed, submitted - completed);
        text += Format(L"\n  unrendered time (starvation + drift) %.2f ms now, %.2f ms max  (%.1f quanta)",
            m_stallUs.load() / 1000.0, m_stallMaxUs.load() / 1000.0,
            m_quantumNs > 0 ? m_stallUs.load() * 1000.0 / m_quantumNs : 0.0);
        text += Format(L"\n  errors: graph %lld  callback %lld (last 0x%08X)  bad buffers %lld",
            graphErrors, m_callbackErrors.load(), m_lastCallbackHr.load(), m_badBuffers.load());
        return hstring(text);
    }
}
