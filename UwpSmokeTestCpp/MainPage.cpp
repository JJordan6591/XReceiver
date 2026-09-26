#include "pch.h"
#include "MainPage.h"
#include "MainPage.g.cpp"

#include <algorithm>
#include <cstdarg>
#include <cwchar>
#include <string>

#include "PocSpikes.h"
#include "SelfTest.h"
#include "VideoPresenter.h"

using namespace winrt;
using namespace winrt::Windows::ApplicationModel;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Media::Playback;
using namespace winrt::Windows::System;
using namespace winrt::Windows::System::Display;
using namespace winrt::Windows::UI::Core;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;
using namespace winrt::Windows::UI::Xaml::Media;
using namespace winrt::Windows::UI::Xaml::Navigation;

using rx::ConnectionState;
using rx::Stat;

namespace
{
    void Append(std::wstring& text, wchar_t const* format, ...)
    {
        wchar_t buffer[512];
        va_list args;
        va_start(args, format);
        _vsnwprintf_s(buffer, _countof(buffer), _TRUNCATE, format, args);
        va_end(args);
        text += buffer;
    }

    Windows::UI::Color StateColor(ConnectionState state)
    {
        switch (state)
        {
        case ConnectionState::Receiving: return { 255, 76, 175, 80 };
        case ConnectionState::Waiting:
        case ConnectionState::Starting: return { 255, 255, 193, 7 };
        case ConnectionState::Reconnecting: return { 255, 255, 152, 0 };
        case ConnectionState::Error: return { 255, 244, 67, 54 };
        default: return { 255, 128, 128, 128 };
        }
    }

    int32_t ParsePort(hstring const& text)
    {
        wchar_t* end = nullptr;
        long const value = std::wcstol(text.c_str(), &end, 10);
        if (text.empty() || end == nullptr || *end != L'\0')
        {
            return -1;
        }
        return static_cast<int32_t>(value);
    }

    bool IsGamepadActivationKey(VirtualKey key)
    {
        switch (key)
        {
        case VirtualKey::GamepadA:
        case VirtualKey::GamepadDPadUp:
        case VirtualKey::GamepadDPadDown:
        case VirtualKey::GamepadDPadLeft:
        case VirtualKey::GamepadDPadRight:
        case VirtualKey::GamepadLeftThumbstickUp:
        case VirtualKey::GamepadLeftThumbstickDown:
        case VirtualKey::GamepadLeftThumbstickLeft:
        case VirtualKey::GamepadLeftThumbstickRight:
        case VirtualKey::Enter:
        case VirtualKey::Space:
            return true;
        default:
            return false;
        }
    }
}

namespace winrt::UwpSmokeTestCpp::implementation
{
    void MainPage::OnNavigatedTo(NavigationEventArgs const&)
    {
        m_settings = rx::ReceiverSettings::Load();

        m_player = MediaPlayer();
        rx::VideoPresenter::ConfigurePlayer(m_player, nullptr);
        VideoElement().SetMediaPlayer(m_player);
        m_session = std::make_shared<rx::ReceiverSession>(Dispatcher(), m_player);

        LoadSettingsIntoUi();
#ifdef _DEBUG
        DebugToolsPanel().Visibility(Visibility::Visible);
#endif

        m_uiTimer = DispatcherTimer();
        m_uiTimer.Interval(std::chrono::milliseconds(250));
        m_tickRevoker = m_uiTimer.Tick(auto_revoke, [weak = get_weak()](IInspectable const&, IInspectable const&)
            {
                if (auto self = weak.get())
                {
                    self->OnUiTick();
                }
            });
        m_uiTimer.Start();

        m_suspendingRevoker = Application::Current().Suspending(auto_revoke, { get_weak(), &MainPage::OnSuspending });
        m_resumingRevoker = Application::Current().Resuming(auto_revoke, { get_weak(), &MainPage::OnResuming });
        m_keyDownRevoker = Window::Current().CoreWindow().KeyDown(auto_revoke, { get_weak(), &MainPage::OnCoreKeyDown });
        m_backRevoker = SystemNavigationManager::GetForCurrentView().BackRequested(auto_revoke, { get_weak(), &MainPage::OnBackRequested });

        m_lastInput = rx::Clock::now();
        UpdateControls();
        if (m_settings.autoStart)
        {
            StartReceiverAsync();
        }
    }

    void MainPage::OnNavigatedFrom(NavigationEventArgs const&)
    {
        ShutdownAsync();
    }

    fire_and_forget MainPage::ShutdownAsync()
    {
        auto strong = get_strong();
        if (m_uiTimer)
        {
            m_uiTimer.Stop();
        }
        m_tickRevoker.revoke();
        m_suspendingRevoker.revoke();
        m_resumingRevoker.revoke();
        m_keyDownRevoker.revoke();
        m_backRevoker.revoke();
        StopPocs();

        if (m_session)
        {
            try
            {
                co_await m_session->StopAsync();
            }
            catch (hresult_error const&)
            {
            }
            m_session.reset();
        }
        UpdateDisplayRequest(ConnectionState::Stopped);

        VideoElement().SetMediaPlayer(nullptr);
        if (m_player)
        {
            m_player.Close();
            m_player = nullptr;
        }
    }

    void MainPage::OnLoaded(IInspectable const&, RoutedEventArgs const&)
    {
        StartButton().Focus(FocusState::Programmatic);
    }

    void MainPage::LoadSettingsIntoUi()
    {
        m_loadingUi = true;
        VideoPortBox().Text(to_hstring(m_settings.videoPort));
        AudioPortBox().Text(to_hstring(m_settings.audioPort));
        AudioToggle().IsOn(m_settings.audioEnabled);
        TolerantToggle().IsOn(m_settings.lossPolicy == rx::LossPolicy::Tolerant);
        AutoStartToggle().IsOn(m_settings.autoStart);
        AvOffsetText().Text(to_hstring(m_settings.avOffsetMs) + L" ms");
        SetDiagnosticsVisible(m_settings.diagnosticsVisible);
        m_loadingUi = false;
    }

    bool MainPage::ReadPortsFromUi()
    {
        int32_t const video = ParsePort(VideoPortBox().Text());
        int32_t const audio = ParsePort(AudioPortBox().Text());
        if (!rx::ReceiverSettings::IsValidPort(video) || !rx::ReceiverSettings::IsValidPort(audio) || video == audio)
        {
            m_uiMessage = L"Ports must be between 1024 and 65535 and different from each other.";
            return false;
        }
        m_uiMessage = L"";
        m_settings.videoPort = video;
        m_settings.audioPort = audio;
        return true;
    }

    fire_and_forget MainPage::StartReceiverAsync()
    {
        auto strong = get_strong();
        if (m_busy || !m_session || m_session->IsActive())
        {
            co_return;
        }
        if (!ReadPortsFromUi())
        {
            UpdateControls();
            co_return;
        }
        StopPocs();
        m_settings.Save();

        m_busy = true;
        UpdateControls();
        try
        {
            co_await m_session->StartAsync(m_settings);
        }
        catch (hresult_error const& e)
        {
            m_uiMessage = L"Start failed: " + e.message();
        }
        m_busy = false;
        UpdateControls();
        if (PanelVisible() && m_session && m_session->IsActive())
        {
            StopButton().Focus(FocusState::Programmatic);
        }
    }

    fire_and_forget MainPage::StopReceiverAsync()
    {
        auto strong = get_strong();
        if (m_busy || !m_session)
        {
            co_return;
        }
        m_busy = true;
        UpdateControls();
        try
        {
            co_await m_session->StopAsync();
        }
        catch (hresult_error const& e)
        {
            m_uiMessage = L"Stop failed: " + e.message();
        }
        m_busy = false;
        UpdateDisplayRequest(ConnectionState::Stopped);
        UpdateControls();
        if (PanelVisible())
        {
            StartButton().Focus(FocusState::Programmatic);
        }
    }

    void MainPage::OnStartClick(IInspectable const&, RoutedEventArgs const&)
    {
        StartReceiverAsync();
    }

    void MainPage::OnStopClick(IInspectable const&, RoutedEventArgs const&)
    {
        StopReceiverAsync();
    }

    void MainPage::OnDiagnosticsClick(IInspectable const&, RoutedEventArgs const&)
    {
        SetDiagnosticsVisible(DiagnosticsPanel().Visibility() != Visibility::Visible);
    }

    void MainPage::OnPortLostFocus(IInspectable const&, RoutedEventArgs const&)
    {
        if (ReadPortsFromUi())
        {
            m_settings.Save();
        }
    }

    void MainPage::OnAudioToggled(IInspectable const&, RoutedEventArgs const&)
    {
        if (m_loadingUi)
        {
            return;
        }
        m_settings.audioEnabled = AudioToggle().IsOn();
        m_settings.Save();
    }

    void MainPage::OnTolerantToggled(IInspectable const&, RoutedEventArgs const&)
    {
        if (m_loadingUi)
        {
            return;
        }
        m_settings.lossPolicy = TolerantToggle().IsOn() ? rx::LossPolicy::Tolerant : rx::LossPolicy::Strict;
        m_settings.Save();
        if (m_session)
        {
            m_session->SetLossPolicy(m_settings.lossPolicy);
        }
    }

    void MainPage::OnAutoStartToggled(IInspectable const&, RoutedEventArgs const&)
    {
        if (m_loadingUi)
        {
            return;
        }
        m_settings.autoStart = AutoStartToggle().IsOn();
        m_settings.Save();
    }

    void MainPage::OnAvMinusClick(IInspectable const&, RoutedEventArgs const&)
    {
        m_settings.avOffsetMs = std::max(-500, m_settings.avOffsetMs - 10);
        AvOffsetText().Text(to_hstring(m_settings.avOffsetMs) + L" ms");
        m_settings.Save();
        if (m_session)
        {
            m_session->SetAvOffsetMs(m_settings.avOffsetMs);
        }
    }

    void MainPage::OnAvPlusClick(IInspectable const&, RoutedEventArgs const&)
    {
        m_settings.avOffsetMs = std::min(500, m_settings.avOffsetMs + 10);
        AvOffsetText().Text(to_hstring(m_settings.avOffsetMs) + L" ms");
        m_settings.Save();
        if (m_session)
        {
            m_session->SetAvOffsetMs(m_settings.avOffsetMs);
        }
    }

    void MainPage::OnSelfTestClick(IInspectable const&, RoutedEventArgs const&)
    {
        m_selfTestSummary = rx::RunSelfTests().summary;
        SetDiagnosticsVisible(true);
    }

    void MainPage::OnPocSocketsClick(IInspectable const&, RoutedEventArgs const&)
    {
        TogglePocSocketsAsync();
    }

    void MainPage::OnPocClipClick(IInspectable const&, RoutedEventArgs const&)
    {
        TogglePocClipAsync();
    }

    void MainPage::OnPocToneClick(IInspectable const&, RoutedEventArgs const&)
    {
        TogglePocToneAsync();
    }

    fire_and_forget MainPage::TogglePocSocketsAsync()
    {
        auto strong = get_strong();
        if (m_pocSockets)
        {
            m_pocSockets->Stop();
            m_pocSockets.reset();
            co_return;
        }
        if (m_session && m_session->IsActive())
        {
            co_await m_session->StopAsync();
        }
        if (!ReadPortsFromUi())
        {
            co_return;
        }
        auto poc = std::make_shared<rx::PocSocketCounter>();
        m_pocSockets = poc;
        SetDiagnosticsVisible(true);
        co_await poc->StartAsync(static_cast<uint16_t>(m_settings.videoPort), static_cast<uint16_t>(m_settings.audioPort));
    }

    fire_and_forget MainPage::TogglePocClipAsync()
    {
        auto strong = get_strong();
        if (m_pocClip)
        {
            m_pocClip->Stop();
            m_pocClip.reset();
            co_return;
        }
        if (m_session && m_session->IsActive())
        {
            co_await m_session->StopAsync();
        }
        auto poc = std::make_shared<rx::PocClipPlayer>();
        m_pocClip = poc;
        SetDiagnosticsVisible(true);
        try
        {
            co_await poc->StartAsync(m_player);
        }
        catch (hresult_error const& e)
        {
            m_uiMessage = L"PoC clip failed: " + e.message();
        }
    }

    fire_and_forget MainPage::TogglePocToneAsync()
    {
        // Each press advances through the tone test matrix; the press after the last test stops
        // and leaves its final numbers on screen.
        auto strong = get_strong();
        if (m_pocTone)
        {
            m_pocTone->Stop();
        }
        if (m_toneStep >= rx::PocToneTest::TestCount())
        {
            m_toneStep = 0;
            co_return;
        }
        size_t const index = m_toneStep++;

        // The receiver owns a second AudioGraph; it must not share the device with the test.
        if (m_session && m_session->IsActive())
        {
            co_await m_session->StopAsync();
        }
        auto poc = std::make_shared<rx::PocToneTest>();
        m_pocTone = poc;
        SetDiagnosticsVisible(true);
        co_await poc->StartAsync(rx::PocToneTest::TestConfig(index), index);
    }

    void MainPage::StopPocs()
    {
        if (m_pocSockets)
        {
            m_pocSockets->Stop();
            m_pocSockets.reset();
        }
        if (m_pocClip)
        {
            m_pocClip->Stop();
            m_pocClip.reset();
        }
        if (m_pocTone)
        {
            m_pocTone->Stop();
            m_pocTone.reset();
        }
        m_toneStep = 0;
    }

    // The app's only suspend cleanup (App::OnSuspending is intentionally empty). Every step is a
    // no-op when already stopped, so a repeated suspend or a later Stop/shutdown is harmless.
    fire_and_forget MainPage::OnSuspending(IInspectable, SuspendingEventArgs args)
    {
        auto strong = get_strong();
        auto deferral = args.SuspendingOperation().GetDeferral();
        m_wasRunningBeforeSuspend = m_wasRunningBeforeSuspend || (m_session && m_session->IsActive());
        StopPocs();
        if (m_session)
        {
            try
            {
                co_await m_session->StopAsync();
            }
            catch (hresult_error const&)
            {
            }
        }
        UpdateDisplayRequest(ConnectionState::Stopped);
        deferral.Complete();
    }

    fire_and_forget MainPage::OnResuming(IInspectable, IInspectable)
    {
        auto strong = get_strong();
        co_await resume_foreground(Dispatcher());
        if (m_wasRunningBeforeSuspend)
        {
            m_wasRunningBeforeSuspend = false;
            StartReceiverAsync();
        }
    }

    void MainPage::OnCoreKeyDown(CoreWindow const&, KeyEventArgs const& args)
    {
        m_lastInput = rx::Clock::now();
        VirtualKey const key = args.VirtualKey();
        if (key == VirtualKey::GamepadMenu)
        {
            ShowPanel(!PanelVisible());
            args.Handled(true);
        }
        else if (key == VirtualKey::GamepadView)
        {
            SetDiagnosticsVisible(DiagnosticsPanel().Visibility() != Visibility::Visible);
            args.Handled(true);
        }
        else if (!PanelVisible() && IsGamepadActivationKey(key))
        {
            ShowPanel(true);
            args.Handled(true);
        }
    }

    void MainPage::OnBackRequested(IInspectable const&, BackRequestedEventArgs const& args)
    {
        // Single-page app: B only hides the panel and must never navigate or exit.
        m_lastInput = rx::Clock::now();
        if (PanelVisible())
        {
            ShowPanel(false);
        }
        args.Handled(true);
    }

    bool MainPage::PanelVisible()
    {
        return ControlPanel().Visibility() == Visibility::Visible;
    }

    void MainPage::ShowPanel(bool show)
    {
        ControlPanel().Visibility(show ? Visibility::Visible : Visibility::Collapsed);
        if (show)
        {
            bool const active = m_session && m_session->IsActive();
            if (active)
            {
                StopButton().Focus(FocusState::Programmatic);
            }
            else
            {
                StartButton().Focus(FocusState::Programmatic);
            }
        }
    }

    void MainPage::SetDiagnosticsVisible(bool visible)
    {
        DiagnosticsPanel().Visibility(visible ? Visibility::Visible : Visibility::Collapsed);
        if (m_settings.diagnosticsVisible != visible)
        {
            m_settings.diagnosticsVisible = visible;
            m_settings.Save();
        }
    }

    void MainPage::UpdateControls()
    {
        bool const active = m_session && m_session->IsActive();
        StartButton().IsEnabled(!active && !m_busy);
        StopButton().IsEnabled(active && !m_busy);
        VideoPortBox().IsEnabled(!active && !m_busy);
        AudioPortBox().IsEnabled(!active && !m_busy);
        AudioToggle().IsEnabled(!active && !m_busy);
    }

    void MainPage::OnUiTick()
    {
        if (!m_session)
        {
            return;
        }
        m_session->OnUiTick();
        if (m_pocClip)
        {
            m_pocClip->OnUiTick();
        }

        auto const now = rx::Clock::now();
        auto const snapshot = m_session->Stats()->Take();
        double const seconds = std::chrono::duration<double>(now - m_lastSnapshotTime).count();
        if (seconds > 0.1 && seconds < 5.0)
        {
            auto delta = [&](Stat s) { return std::max<int64_t>(0, rx::At(snapshot, s) - rx::At(m_lastSnapshot, s)); };
            m_fps = delta(Stat::FramesSubmitted) / seconds;
            m_videoMbps = delta(Stat::VideoBytes) * 8.0 / seconds / 1e6;
            m_audioKbps = delta(Stat::AudioBytes) * 8.0 / seconds / 1e3;
        }
        m_lastSnapshot = snapshot;
        m_lastSnapshotTime = now;

        ConnectionState const state = m_session->State();
        UpdateStatus(state, snapshot);
        if (DiagnosticsPanel().Visibility() == Visibility::Visible)
        {
            UpdateDiagnostics(snapshot);
        }
        UpdateDisplayRequest(state);
        UpdateAutoHide(state);
        if (state != m_lastState)
        {
            m_lastState = state;
            UpdateControls();
        }
    }

    void MainPage::UpdateStatus(ConnectionState state, rx::ReceiverStats::Snapshot const& snapshot)
    {
        StatusText().Text(rx::ToString(state));
        StatusDot().Fill(SolidColorBrush(StateColor(state)));

        std::wstring detail;
        switch (state)
        {
        case ConnectionState::Waiting:
            Append(detail, L"Listening on UDP %d (video)", m_settings.videoPort);
            if (m_settings.audioEnabled)
            {
                Append(detail, L" and %d (audio)", m_settings.audioPort);
            }
            break;
        case ConnectionState::Receiving:
            Append(detail, L"%lldx%lld  %.1f fps  %.1f Mbps", rx::At(snapshot, Stat::Width), rx::At(snapshot, Stat::Height), m_fps, m_videoMbps);
            if (rx::At(snapshot, Stat::WaitingForKeyframe))
            {
                detail += L"  (holding picture until the next keyframe)";
            }
            break;
        case ConnectionState::Reconnecting:
            Append(detail, L"No audio or video for %d s; waiting for UxPlay",
                std::max(m_settings.idleTimeoutMs, rx::ReceiverSettings::kMinIdleTimeoutMs) / 1000);
            break;
        default:
            break;
        }

        if (m_session)
        {
            hstring const error = m_session->ErrorMessage();
            if (!error.empty())
            {
                if (!detail.empty()) detail += L"\n";
                detail += error.c_str();
            }
            hstring const audio = m_session->AudioStatus();
            if (!audio.empty() && m_session->IsActive())
            {
                if (!detail.empty()) detail += L"\n";
                detail += audio.c_str();
            }
        }
        if (!m_uiMessage.empty())
        {
            if (!detail.empty()) detail += L"\n";
            detail += m_uiMessage.c_str();
        }

        StatusDetailText().Text(detail);
        StatusDetailText().Visibility(detail.empty() ? Visibility::Collapsed : Visibility::Visible);
    }

    void MainPage::UpdateDiagnostics(rx::ReceiverStats::Snapshot const& s)
    {
        auto at = [&](Stat stat) { return rx::At(s, stat); };
        std::wstring text;

        Append(text, L"VIDEO  %lldx%lld  profile %lld level %.1f  %.1f fps  %.2f Mbps\n",
            at(Stat::Width), at(Stat::Height), at(Stat::Profile), at(Stat::Level) / 10.0, m_fps, m_videoMbps);
        Append(text, L"  rtp  ssrc %08llx  seq %lld  ts %lld  jitter %.2f ms\n",
            at(Stat::VideoSsrc), at(Stat::VideoLastSequence), at(Stat::VideoLastTimestamp), at(Stat::VideoJitterUs) / 1000.0);
        Append(text, L"  packets %lld  gaps %lld (lost %lld)  dup %lld  reordered %lld  late %lld\n",
            at(Stat::VideoPackets), at(Stat::VideoSequenceGaps), at(Stat::VideoLost), at(Stat::VideoDuplicate),
            at(Stat::VideoReordered), at(Stat::VideoLate));
        Append(text, L"  invalid %lld  wrong pt %lld  ignored %lld  resync %lld  oversize %lld  socket err %lld\n",
            at(Stat::VideoInvalid), at(Stat::VideoWrongPayloadType), at(Stat::VideoForeignSsrc), at(Stat::VideoOutOfWindow),
            at(Stat::VideoTooLarge), at(Stat::VideoSocketErrors));
        Append(text, L"  AU complete %lld  incomplete %lld (no marker %lld)  discarded %lld  IDR %lld (interval %lld ms)\n",
            at(Stat::AccessUnitsComplete), at(Stat::AccessUnitsIncomplete), at(Stat::AccessUnitsMissingMarker),
            at(Stat::AccessUnitsDiscarded), at(Stat::IdrCount), at(Stat::IdrIntervalMs));
        Append(text, L"  bad FU-A %lld  bad STAP-A %lld  unsupported NAL %lld  malformed %lld\n",
            at(Stat::FuaErrors), at(Stat::StapaErrors), at(Stat::UnsupportedNal), at(Stat::MalformedPayload));
        Append(text, L"  dropped  incomplete %lld  awaiting IDR %lld  non-ref %lld  stale %lld  playing-full %lld  startup-full %lld%s\n",
            at(Stat::DropIncomplete), at(Stat::DropAwaitingIdr), at(Stat::DropNonRef), at(Stat::DropStale),
            at(Stat::DropQueueFull), at(Stat::DropStartupFull), at(Stat::WaitingForKeyframe) ? L"  [AWAITING IDR]" : L"");
        static wchar_t const* const kPhases[] = { L"none", L"opening", L"starting", L"playing" };
        int64_t const phase = at(Stat::DeliveryPhase);
        Append(text, L"  delivery %s  startup peak %lld frames  IDR waits: network %lld  back-pressure %lld\n",
            phase >= 0 && phase < 4 ? kPhases[phase] : L"?", at(Stat::StartupPeakFrames),
            at(Stat::IdrWaitsNetwork), at(Stat::IdrWaitsBackpressure));
        Append(text, L"  submitted %lld (%.1f fps)  queue %lld  pts discont %lld  sample err %lld\n",
            at(Stat::FramesSubmitted), m_fps, at(Stat::FrameQueueDepth),
            at(Stat::PtsDiscontinuities), at(Stat::SampleErrors));
        Append(text, L"  requests %lld  deferred %lld  pending %lld  overlapping %lld  last request %lld ms ago  ended %lld\n",
            at(Stat::SampleRequests), at(Stat::SampleDeferrals), at(Stat::PendingRequests), at(Stat::OverlappingRequests),
            at(Stat::LastRequestAgeMs), at(Stat::EndOfStreamCompletions));
        Append(text, L"  processed %lld  unprocessed %lld  rendered %lld\n",
            at(Stat::SamplesProcessed), at(Stat::SamplesInFlight), at(Stat::SamplesRendered));
        Append(text, L"  recv->submit p50 %.1f / p95 %.1f ms  sample lag %.1f ms  pts lead %.1f ms\n",
            at(Stat::ReceiveToSubmitP50Us) / 1000.0, at(Stat::ReceiveToSubmitP95Us) / 1000.0,
            at(Stat::SampleLagUs) / 1000.0, at(Stat::PtsLeadUs) / 1000.0);
        Append(text, L"  real-time %s  poc type %lld  restriction %s (max dec buf %lld)  sources %lld  decoder errors %lld  restarts %lld\n",
            at(Stat::RealTimePlayback) ? L"on" : L"off", at(Stat::PocType), at(Stat::BitstreamRestriction) ? L"yes" : L"no",
            at(Stat::MaxDecFrameBuffering), at(Stat::SourceBuilds), at(Stat::MediaFailures), at(Stat::VideoStreamRestarts));

        if (m_settings.audioEnabled)
        {
            Append(text, L"AUDIO  %s  graph %lld Hz  quantum %lld  resampler %s  output %.1f ms  %.0f kbps\n",
                at(Stat::AudioRunning) ? L"running" : L"not running", at(Stat::AudioGraphRate), at(Stat::AudioQuantumSamples),
                at(Stat::AudioResamplerActive) ? L"44.1k->graph" : L"no", at(Stat::AudioOutputLatencyUs) / 1000.0, m_audioKbps);
            Append(text, L"  packets %lld  lost %lld  dup %lld  reordered %lld  late %lld  invalid %lld  restarts %lld\n",
                at(Stat::AudioPackets), at(Stat::AudioLost), at(Stat::AudioDuplicate), at(Stat::AudioReordered),
                at(Stat::AudioLate), at(Stat::AudioInvalid) + at(Stat::AudioWrongPayloadType), at(Stat::AudioStreamRestarts));
            Append(text, L"  buffer %.1f ms  target %lld ms  drift %lld ppm  jitter %.2f ms\n",
                at(Stat::AudioFillUs) / 1000.0, at(Stat::AudioTargetDelayMs), at(Stat::AudioDriftPpm), at(Stat::AudioJitterUs) / 1000.0);
            Append(text, L"  underruns %lld  concealed %.0f ms  overflow %lld  hard-cap drops %lld  gaps %lld  callback err %lld  graph err %lld",
                at(Stat::AudioUnderruns), at(Stat::AudioConcealedFrames) / 44.1, at(Stat::AudioOverflowFrames),
                at(Stat::AudioHardCapDropFrames), at(Stat::AudioDiscontinuities), at(Stat::AudioCallbackErrors),
                at(Stat::AudioGraphErrors));
        }
        else
        {
            text += L"AUDIO  disabled";
        }

        if (!m_selfTestSummary.empty())
        {
            text += L"\n";
            text += m_selfTestSummary.c_str();
        }
        if (m_pocSockets)
        {
            text += L"\n";
            text += m_pocSockets->Status().c_str();
        }
        if (m_pocClip)
        {
            text += L"\n";
            text += m_pocClip->Status().c_str();
        }
        if (m_pocTone)
        {
            text += L"\n";
            text += m_pocTone->Status().c_str();
        }

        DiagnosticsText().Text(text);
    }

    void MainPage::UpdateDisplayRequest(ConnectionState state)
    {
        bool const wantActive = state == ConnectionState::Waiting || state == ConnectionState::Receiving ||
                                state == ConnectionState::Reconnecting || m_pocClip != nullptr;
        if (wantActive == m_displayRequestActive)
        {
            return;
        }
        try
        {
            if (!m_displayRequest)
            {
                m_displayRequest = DisplayRequest();
            }
            if (wantActive)
            {
                m_displayRequest.RequestActive();
            }
            else
            {
                m_displayRequest.RequestRelease();
            }
            m_displayRequestActive = wantActive;
        }
        catch (hresult_error const&)
        {
        }
    }

    void MainPage::UpdateAutoHide(ConnectionState state)
    {
        auto const now = rx::Clock::now();
        if (state == ConnectionState::Receiving)
        {
            if (m_lastState != ConnectionState::Receiving)
            {
                m_receivingSince = now;
            }
            bool const editing = VideoPortBox().FocusState() != FocusState::Unfocused ||
                                 AudioPortBox().FocusState() != FocusState::Unfocused;
            if (PanelVisible() && !editing && now - m_receivingSince > std::chrono::seconds(5) &&
                now - m_lastInput > std::chrono::seconds(5))
            {
                ShowPanel(false);
            }
        }
        else if (state != m_lastState && (state == ConnectionState::Error || state == ConnectionState::Stopped))
        {
            if (!PanelVisible())
            {
                ShowPanel(true);
            }
        }
    }
}
