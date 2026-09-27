#include "pch.h"
#include "MainPage.h"
#include "MainPage.g.cpp"

#include <algorithm>
#include <cstdarg>
#include <cwchar>
#include <string>

#include "VideoPresenter.h"
#ifdef _DEBUG
#include "PocSpikes.h"
#include "SelfTest.h"
#endif

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

    wchar_t const* StatusTitle(ConnectionState state)
    {
        switch (state)
        {
        case ConnectionState::Stopped: return L"Stopped";
        case ConnectionState::Starting: return L"Starting";
        case ConnectionState::Waiting: return L"Waiting for media";
        case ConnectionState::Receiving: return L"Receiving";
        case ConnectionState::Reconnecting: return L"Reconnecting";
        case ConnectionState::Error: return L"Playback problem";
        case ConnectionState::Stopping: return L"Stopping";
        default: return L"Stopped";
        }
    }

    std::wstring FormatUserError(hstring const& raw)
    {
        if (raw.empty())
        {
            return {};
        }
        std::wstring text = raw.c_str();
        if (text.find(L"HEVC") != std::wstring::npos || text.find(L"H.265") != std::wstring::npos ||
            text.find(L"h265") != std::wstring::npos)
        {
            return L"This receiver accepts H.264 video only. Configure the companion server to send H.264 (for UxPlay, do not use -h265).";
        }
        return text;
    }

    std::wstring FormatAudioStatus(hstring const& raw)
    {
        if (raw.empty())
        {
            return {};
        }
        std::wstring text = raw.c_str();
        if (text == L"Audio disabled")
        {
            return L"Audio is turned off in settings.";
        }
        return L"Audio: " + text;
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

namespace winrt::XReceiver::implementation
{
    rx::ChromeState ToChromeState(ConnectionState state);
    void MainPage::OnNavigatedTo(NavigationEventArgs const&)
    {
        m_settings = rx::ReceiverSettings::Load();
        FirstRunTitle().Text(rx::AppDisplayName());

        m_player = MediaPlayer();
        rx::VideoPresenter::ConfigurePlayer(m_player, nullptr);
        VideoElement().SetMediaPlayer(m_player);
        m_session = std::make_shared<rx::ReceiverSession>(Dispatcher(), m_player);

        LoadSettingsIntoUi();
#ifdef _DEBUG
        DebugToolsPanel().Visibility(Visibility::Visible);
#endif
        ShowFirstRun(!m_settings.firstRunDismissed);
        if (!FirstRunVisible())
        {
            ShowPanel(true);
        }

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
        if (FirstRunVisible() || PanelVisible())
        {
            FocusChrome();
        }
    }

    void MainPage::LoadSettingsIntoUi()
    {
        m_loadingUi = true;
        VideoPortBox().Text(to_hstring(m_settings.videoPort));
        AudioPortBox().Text(to_hstring(m_settings.audioPort));
        AudioToggle().IsOn(m_settings.audioEnabled);
        TolerantToggle().IsOn(m_settings.lossPolicy == rx::LossPolicy::Tolerant);
        AutoStartToggle().IsOn(m_settings.autoStart);
        wchar_t offset[64] = {};
        rx::FormatAvOffset(m_settings.avOffsetMs, offset, 64);
        AvOffsetText().Text(offset);
        OverlayModeText().Text(rx::OverlayLabel(m_settings.playbackOverlay));
        m_liveDiagnostics = m_settings.diagnosticsVisible && m_settings.playbackOverlay != rx::PlaybackOverlay::VideoOnly;
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
        if (PanelVisible())
        {
            FocusChrome();
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
            FocusChrome();
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
        HandleChromeAction(rx::ChromeAction::View);
    }

    void MainPage::OnHelpClick(IInspectable const&, RoutedEventArgs const&)
    {
        m_lastInput = rx::Clock::now();
        ShowFirstRun(true);
    }

    void MainPage::OnFirstRunContinueClick(IInspectable const&, RoutedEventArgs const&)
    {
        m_settings.firstRunDismissed = true;
        m_settings.Save();
        ShowFirstRun(false);
        ShowPanel(true);
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
        m_settings.avOffsetMs = std::max(-rx::ReceiverSettings::kMaxAvOffsetMs, m_settings.avOffsetMs - 10);
        wchar_t offset[64] = {};
        rx::FormatAvOffset(m_settings.avOffsetMs, offset, 64);
        AvOffsetText().Text(offset);
        m_settings.Save();
        if (m_session)
        {
            m_session->SetAvOffsetMs(m_settings.avOffsetMs);
        }
    }

    void MainPage::OnAvPlusClick(IInspectable const&, RoutedEventArgs const&)
    {
        m_settings.avOffsetMs = std::min(rx::ReceiverSettings::kMaxAvOffsetMs, m_settings.avOffsetMs + 10);
        wchar_t offset[64] = {};
        rx::FormatAvOffset(m_settings.avOffsetMs, offset, 64);
        AvOffsetText().Text(offset);
        m_settings.Save();
        if (m_session)
        {
            m_session->SetAvOffsetMs(m_settings.avOffsetMs);
        }
    }

    void MainPage::OnSelfTestClick(IInspectable const&, RoutedEventArgs const&)
    {
#ifdef _DEBUG
        m_selfTestSummary = rx::RunSelfTests().summary;
        SetDiagnosticsVisible(true);
#endif
    }

    void MainPage::OnPocSocketsClick(IInspectable const&, RoutedEventArgs const&)
    {
#ifdef _DEBUG
        TogglePocSocketsAsync();
#endif
    }

    void MainPage::OnPocClipClick(IInspectable const&, RoutedEventArgs const&)
    {
#ifdef _DEBUG
        TogglePocClipAsync();
#endif
    }

    void MainPage::OnPocToneClick(IInspectable const&, RoutedEventArgs const&)
    {
#ifdef _DEBUG
        TogglePocToneAsync();
#endif
    }

    fire_and_forget MainPage::TogglePocSocketsAsync()
    {
#ifndef _DEBUG
        co_return;
#else
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
#endif
    }

    fire_and_forget MainPage::TogglePocClipAsync()
    {
#ifndef _DEBUG
        co_return;
#else
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
#endif
    }

    fire_and_forget MainPage::TogglePocToneAsync()
    {
#ifndef _DEBUG
        co_return;
#else
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
#endif
    }

    void MainPage::StopPocs()
    {
#ifndef _DEBUG
        return;
#else
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
#endif
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
        if (FirstRunVisible())
        {
            return;
        }
        if (key == VirtualKey::GamepadMenu)
        {
            HandleChromeAction(rx::ChromeAction::Menu);
            args.Handled(true);
        }
        else if (key == VirtualKey::GamepadView)
        {
            HandleChromeAction(rx::ChromeAction::View);
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
        HandleChromeAction(rx::ChromeAction::Back);
        args.Handled(true);
    }

    bool MainPage::PanelVisible()
    {
        return ControlPanel().Visibility() == Visibility::Visible;
    }

    bool MainPage::FirstRunVisible()
    {
        return FirstRunPanel().Visibility() == Visibility::Visible;
    }

    void MainPage::ShowFirstRun(bool show)
    {
        FirstRunPanel().Visibility(show ? Visibility::Visible : Visibility::Collapsed);
        ControlPanel().IsHitTestVisible(!show);
        DiagnosticsPanel().IsHitTestVisible(false);
        if (show)
        {
            // Ports can change only while onboarding is closed, so refreshing on open is enough.
            wchar_t network[160] = {};
            rx::FormatOnboardingNetwork(m_settings.videoPort, m_settings.audioPort, network, 160);
            FirstRunNetworkText().Text(network);
            FocusChrome();
        }
    }

    void MainPage::ShowPanel(bool show)
    {
        if (FirstRunVisible())
        {
            show = false;
        }
        ControlPanel().Visibility(show ? Visibility::Visible : Visibility::Collapsed);
        if (show)
        {
            FocusChrome();
        }
        ApplyChrome(m_lastState, false);
    }

    void MainPage::SetDiagnosticsVisible(bool visible)
    {
        m_liveDiagnostics = visible;
        if (m_settings.diagnosticsVisible != visible)
        {
            m_settings.diagnosticsVisible = visible;
            m_settings.Save();
        }
        ApplyChrome(m_lastState, false);
    }

    void MainPage::UpdateControls()
    {
        rx::ControlAvailability const controls = rx::ControlsFor(m_session && m_session->IsActive(), m_busy);
        StartButton().IsEnabled(controls.start);
        StopButton().IsEnabled(controls.stop);
        VideoPortBox().IsEnabled(controls.ports);
        AudioPortBox().IsEnabled(controls.ports);
        AudioToggle().IsEnabled(controls.audioToggle);
    }

    void MainPage::OnUiTick()
    {
        if (!m_session)
        {
            return;
        }
        m_session->OnUiTick();
#ifdef _DEBUG
        if (m_pocClip)
        {
            m_pocClip->OnUiTick();
        }
#endif

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
        bool const waitingKeyframe = m_session && rx::At(snapshot, Stat::WaitingForKeyframe) != 0;
        bool const audioFailed = m_session && !m_session->AudioStatus().empty() && m_settings.audioEnabled;
        rx::StatusCopy const copy = rx::StatusFor(ToChromeState(state), waitingKeyframe, m_settings.audioEnabled, audioFailed);
        StatusText().Text(copy.title);
        StatusDot().Fill(SolidColorBrush(StateColor(state)));

        std::wstring detail = copy.detail;
        if (state == ConnectionState::Waiting)
        {
            // Ports are locked while the receiver runs, so the saved values are the listeners.
            wchar_t ports[64] = {};
            rx::FormatListeningPorts(m_settings.videoPort, m_settings.audioPort, m_settings.audioEnabled, ports, 64);
            detail += L" ";
            detail += ports;
        }
        else if (state == ConnectionState::Receiving && !waitingKeyframe)
        {
            Append(detail, L" %lld×%lld.", rx::At(snapshot, Stat::Width), rx::At(snapshot, Stat::Height));
        }

        if (m_session)
        {
            hstring const error = m_session->ErrorMessage();
            if (!error.empty())
            {
                std::wstring const friendly = FormatUserError(error);
                if (!friendly.empty())
                {
                    if (!detail.empty()) detail += L"\n";
                    detail += friendly;
                }
            }
            hstring const audio = m_session->AudioStatus();
            if (!audio.empty() && m_session->IsActive())
            {
                std::wstring const audioLine = FormatAudioStatus(audio);
                if (!audioLine.empty())
                {
                    if (!detail.empty()) detail += L"\n";
                    detail += audioLine;
                }
            }
        }
        if (!m_uiMessage.empty())
        {
            if (!detail.empty()) detail += L"\n";
            detail += m_uiMessage.c_str();
        }

        StatusDetailText().Text(detail);
        StatusDetailText().Visibility(detail.empty() ? Visibility::Collapsed : Visibility::Visible);
        ApplyChrome(state, waitingKeyframe);
    }

    rx::ChromeState ToChromeState(ConnectionState state)
    {
        switch (state)
        {
        case ConnectionState::Starting: return rx::ChromeState::Starting;
        case ConnectionState::Waiting: return rx::ChromeState::Waiting;
        case ConnectionState::Receiving: return rx::ChromeState::Receiving;
        case ConnectionState::Reconnecting: return rx::ChromeState::Reconnecting;
        case ConnectionState::Error: return rx::ChromeState::Error;
        default: return rx::ChromeState::Stopped;
        }
    }

    void MainPage::SetOverlay(rx::PlaybackOverlay overlay)
    {
        if (m_loadingUi)
        {
            return;
        }
        m_settings.playbackOverlay = overlay;
        m_settings.Save();
        OverlayModeText().Text(rx::OverlayLabel(overlay));
        if (overlay == rx::PlaybackOverlay::VideoOnly)
        {
            m_liveDiagnostics = false;
        }
        m_statusHoldUntil = rx::Clock::now() + std::chrono::seconds(5);
        ApplyChrome(m_lastState, false);
    }

    void MainPage::OnOverlayAutomaticClick(IInspectable const&, RoutedEventArgs const&)
    {
        SetOverlay(rx::PlaybackOverlay::Automatic);
    }

    void MainPage::OnOverlayAlwaysClick(IInspectable const&, RoutedEventArgs const&)
    {
        SetOverlay(rx::PlaybackOverlay::AlwaysVisible);
    }

    void MainPage::OnOverlayVideoOnlyClick(IInspectable const&, RoutedEventArgs const&)
    {
        SetOverlay(rx::PlaybackOverlay::VideoOnly);
    }

    rx::ChromeInput MainPage::ChromeInputFor(ConnectionState state, bool waitingForKeyframe)
    {
        rx::ChromeInput input;
        input.overlay = m_settings.playbackOverlay;
        input.state = ToChromeState(state);
        input.usableVideo = state == ConnectionState::Receiving && !waitingForKeyframe;
        input.waitingForKeyframe = waitingForKeyframe;
        input.panelOpen = PanelVisible();
        input.diagnosticsOpen = m_liveDiagnostics;
        input.statusHold = rx::Clock::now() < m_statusHoldUntil;
        input.firstRun = FirstRunVisible();
        return input;
    }

    void MainPage::ApplyChrome(ConnectionState state, bool waitingForKeyframe)
    {
        if (state != m_chromeState)
        {
            m_chromeState = state;
            m_statusHoldUntil = rx::Clock::now() + std::chrono::seconds(5);
        }
        rx::ChromeInput const input = ChromeInputFor(state, waitingForKeyframe);
        rx::ChromeVisibility const visible = rx::DecideChrome(input);
        StatusPill().Visibility(visible.status ? Visibility::Visible : Visibility::Collapsed);
        WaitingGuide().Visibility(visible.waitingGuide ? Visibility::Visible : Visibility::Collapsed);
        ControlPanel().Visibility(visible.controls ? Visibility::Visible : Visibility::Collapsed);
        DiagnosticsPanel().Visibility(visible.diagnostics ? Visibility::Visible : Visibility::Collapsed);
        if (m_controlsWereVisible && !visible.controls && !input.firstRun)
        {
            // Hidden controls must not keep focus, also when diagnostics stay open.
            MoveFocus(rx::FocusForChrome(input, visible, m_session && m_session->IsActive()));
        }
        m_controlsWereVisible = visible.controls;
    }

    // Menu, View and B change panels only; playback is untouched.
    void MainPage::HandleChromeAction(rx::ChromeAction action)
    {
        rx::ChromeInput const before = ChromeInputFor(m_lastState, false);
        rx::ChromeInput const after = rx::ApplyChromeAction(before, action);
        if (after.panelOpen != before.panelOpen)
        {
            ShowPanel(after.panelOpen);
        }
        if (after.diagnosticsOpen != before.diagnosticsOpen)
        {
            SetDiagnosticsVisible(after.diagnosticsOpen);
        }
    }

    void MainPage::FocusChrome()
    {
        rx::ChromeInput const input = ChromeInputFor(m_lastState, false);
        MoveFocus(rx::FocusForChrome(input, rx::DecideChrome(input), m_session && m_session->IsActive()));
    }

    void MainPage::MoveFocus(rx::FocusTarget target)
    {
        switch (target)
        {
        case rx::FocusTarget::Continue:
            FirstRunContinueButton().Focus(FocusState::Programmatic);
            break;
        case rx::FocusTarget::Start:
            StartButton().Focus(FocusState::Programmatic);
            break;
        case rx::FocusTarget::Stop:
            StopButton().Focus(FocusState::Programmatic);
            break;
        case rx::FocusTarget::None:
            this->Focus(FocusState::Programmatic);
            break;
        }
    }

    void MainPage::UpdateDiagnostics(rx::ReceiverStats::Snapshot const& s)
    {
        auto at = [&](Stat stat) { return rx::At(s, stat); };
        std::wstring text;
        static wchar_t const* const kPhases[] = { L"none", L"opening", L"starting", L"playing" };
        int64_t const phase = at(Stat::DeliveryPhase);

        // Sources publish -1 for a value that is not available yet; show it as an em dash.
        auto optional = [&](Stat stat, int64_t divisor, wchar_t const* suffix, wchar_t* out, size_t outChars)
        {
            int64_t const value = at(stat);
            rx::FormatUnavailable(value >= 0, value / divisor, suffix, out, outChars);
        };

        wchar_t health[96] = {};
        rx::FormatHealth(static_cast<uint32_t>(at(Stat::HealthFlags)), health, 96);
        text += L"— Session —\n";
        Append(text, L"health %s  uptime %lld s  this state %lld s\n",
            health, at(Stat::SessionUptimeMs) / 1000, at(Stat::StateAgeMs) / 1000);
        wchar_t memory[32] = {};
        optional(Stat::AppMemoryBytes, 1024, L" KB", memory, 32);
        Append(text, L"memory %s  peak %lld KB  stalls %lld  now %lld ms  longest %lld ms\n",
            memory, at(Stat::AppMemoryHighBytes) / 1024, at(Stat::StallCount), at(Stat::StallCurrentMs), at(Stat::StallLongestMs));

        text += L"\n— Stream / source —\n";
        Append(text, L"%lld×%lld  profile %lld  level %.1f  %.1f fps  %.2f Mbps\n",
            at(Stat::Width), at(Stat::Height), at(Stat::Profile), at(Stat::Level) / 10.0, m_fps, m_videoMbps);
        Append(text, L"real-time %s  sources %lld  sender changes %lld  accepted pkts %lld\n",
            at(Stat::RealTimePlayback) ? L"on" : L"off", at(Stat::SourceBuilds), at(Stat::VideoStreamRestarts),
            at(Stat::VideoAccepted));

        text += L"\n— Network / RTP —\n";
        Append(text, L"packets %lld  gaps %lld (lost %lld)  dup %lld  reordered %lld  late %lld\n",
            at(Stat::VideoPackets), at(Stat::VideoSequenceGaps), at(Stat::VideoLost), at(Stat::VideoDuplicate),
            at(Stat::VideoReordered), at(Stat::VideoLate));
        Append(text, L"invalid %lld  wrong PT %lld  foreign SSRC %lld  out-of-window %lld  oversize %lld  socket err %lld\n",
            at(Stat::VideoInvalid), at(Stat::VideoWrongPayloadType), at(Stat::VideoForeignSsrc), at(Stat::VideoOutOfWindow),
            at(Stat::VideoTooLarge), at(Stat::VideoSocketErrors));
        wchar_t packetAge[32] = {};
        wchar_t submitAge[32] = {};
        optional(Stat::VideoPacketAgeMs, 1, L" ms", packetAge, 32);
        optional(Stat::VideoSubmitAgeMs, 1, L" ms", submitAge, 32);
        Append(text, L"seq %lld  ts %lld  jitter %.2f ms  last packet %s  last submit %s\n",
            at(Stat::VideoLastSequence), at(Stat::VideoLastTimestamp), at(Stat::VideoJitterUs) / 1000.0,
            packetAge, submitAge);

        text += L"\n— H.264 parser & recovery —\n";
        Append(text, L"AU complete %lld  incomplete %lld (no marker %lld)  discarded %lld  IDR %lld (interval %lld ms)\n",
            at(Stat::AccessUnitsComplete), at(Stat::AccessUnitsIncomplete), at(Stat::AccessUnitsMissingMarker),
            at(Stat::AccessUnitsDiscarded), at(Stat::IdrCount), at(Stat::IdrIntervalMs));
        Append(text, L"FU-A err %lld  STAP-A err %lld  unsupported NAL %lld  malformed %lld\n",
            at(Stat::FuaErrors), at(Stat::StapaErrors), at(Stat::UnsupportedNal), at(Stat::MalformedPayload));
        Append(text, L"dropped incomplete %lld  network-damage IDR wait %lld  back-pressure IDR wait %lld  non-ref %lld  stale %lld\n",
            at(Stat::DropIncomplete), at(Stat::IdrWaitsNetwork), at(Stat::IdrWaitsBackpressure), at(Stat::DropNonRef), at(Stat::DropStale));
        Append(text, L"queue-full %lld  startup-full %lld  awaiting keyframe %s  wait %lld ms (longest %lld)\n",
            at(Stat::DropQueueFull), at(Stat::DropStartupFull), at(Stat::WaitingForKeyframe) ? L"yes" : L"no",
            at(Stat::IdrWaitCurrentMs), at(Stat::IdrWaitLongestMs));

        text += L"\n— Decoder / presentation —\n";
        Append(text, L"delivery %s  startup peak %lld frames\n",
            phase >= 0 && phase < 4 ? kPhases[phase] : L"?", at(Stat::StartupPeakFrames));
        Append(text, L"submitted %lld  queue %lld frames / %lld KB  peak %lld frames / %lld KB\n",
            at(Stat::FramesSubmitted), at(Stat::FrameQueueDepth), at(Stat::FrameQueueBytes) / 1024,
            at(Stat::FrameQueueFramesHigh), at(Stat::FrameQueueBytesHigh) / 1024);
        Append(text, L"pts jumps %lld  sample errors %lld  recv→submit is not display latency\n",
            at(Stat::PtsDiscontinuities), at(Stat::SampleErrors));
        wchar_t requestAge[32] = {};
        optional(Stat::LastRequestAgeMs, 1, L" ms ago", requestAge, 32);
        Append(text, L"requests %lld  deferred %lld  pending %lld  overlapping %lld  last request %s\n",
            at(Stat::SampleRequests), at(Stat::SampleDeferrals), at(Stat::PendingRequests), at(Stat::OverlappingRequests),
            requestAge);
        Append(text, L"processed %lld  in flight %lld  rendered %lld  decoder failures %lld\n",
            at(Stat::SamplesProcessed), at(Stat::SamplesInFlight), at(Stat::SamplesRendered), at(Stat::MediaFailures));
        Append(text, L"recv→submit p50 %.1f / p95 %.1f ms  sample lag %.1f ms  pts lead %.1f ms\n",
            at(Stat::ReceiveToSubmitP50Us) / 1000.0, at(Stat::ReceiveToSubmitP95Us) / 1000.0,
            at(Stat::SampleLagUs) / 1000.0, at(Stat::PtsLeadUs) / 1000.0);
        Append(text, L"bitstream restriction %s (max dec buf %lld)\n",
            at(Stat::BitstreamRestriction) ? L"yes" : L"no", at(Stat::MaxDecFrameBuffering));

        text += L"\n— Audio —\n";
        if (m_settings.audioEnabled)
        {
            Append(text, L"%s  graph %lld Hz  quantum %lld  resampler %s  output %.1f ms  %.0f kbps\n",
                at(Stat::AudioRunning) ? L"running" : L"not running", at(Stat::AudioGraphRate), at(Stat::AudioQuantumSamples),
                at(Stat::AudioResamplerActive) ? L"44.1k→graph" : L"no", at(Stat::AudioOutputLatencyUs) / 1000.0, m_audioKbps);
            Append(text, L"packets %lld  lost %lld  dup %lld  reordered %lld  late %lld  invalid %lld  restarts %lld\n",
                at(Stat::AudioPackets), at(Stat::AudioLost), at(Stat::AudioDuplicate), at(Stat::AudioReordered),
                at(Stat::AudioLate), at(Stat::AudioInvalid) + at(Stat::AudioWrongPayloadType), at(Stat::AudioStreamRestarts));
            Append(text, L"buffer %.1f ms (peak %.1f)  target %lld ms  drift %lld ppm  jitter %.2f ms\n",
                at(Stat::AudioFillUs) / 1000.0, at(Stat::AudioFillHighUs) / 1000.0, at(Stat::AudioTargetDelayMs),
                at(Stat::AudioDriftPpm), at(Stat::AudioJitterUs) / 1000.0);
            Append(text, L"underruns %lld  longest %.1f ms  concealed %.0f ms  overflow %lld  hard-cap %lld\n",
                at(Stat::AudioUnderruns), at(Stat::AudioUnderrunLongestUs) / 1000.0, at(Stat::AudioConcealedFrames) / 44.1,
                at(Stat::AudioOverflowFrames), at(Stat::AudioHardCapDropFrames));
            Append(text, L"gaps %lld  callback %lld us (overruns %lld)  callback err %lld  graph err %lld  accepted %lld",
                at(Stat::AudioDiscontinuities), at(Stat::AudioCallbackUs), at(Stat::AudioCallbackOverruns),
                at(Stat::AudioCallbackErrors), at(Stat::AudioGraphErrors), at(Stat::AudioAccepted));
        }
        else
        {
            text += L"disabled in settings";
        }

        text += L"\n\n— Lifecycle —\n";
        Append(text, L"end-of-stream completions %lld\n", at(Stat::EndOfStreamCompletions));

#ifdef _DEBUG
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
#endif

        DiagnosticsText().Text(text);
    }

    void MainPage::UpdateDisplayRequest(ConnectionState state)
    {
        bool wantActive = state == ConnectionState::Waiting || state == ConnectionState::Receiving ||
                          state == ConnectionState::Reconnecting;
#ifdef _DEBUG
        wantActive = wantActive || static_cast<bool>(m_pocClip);
#endif
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
