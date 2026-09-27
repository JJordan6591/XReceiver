#pragma once

#include "MainPage.g.h"

#include <memory>

#include "MediaClock.h"
#include "ReceiverSession.h"
#include "ReceiverSettings.h"
#include "ReceiverStats.h"

namespace rx
{
#ifdef _DEBUG
    class PocClipPlayer;
    class PocSocketCounter;
    class PocToneTest;
#endif
}

namespace winrt::UwpSmokeTestCpp::implementation
{
    struct MainPage : MainPageT<MainPage>
    {
        MainPage()
        {
            // Xaml objects should not call InitializeComponent during construction.
            // See https://github.com/microsoft/cppwinrt/tree/master/nuget#initializecomponent
        }

        void OnNavigatedTo(Windows::UI::Xaml::Navigation::NavigationEventArgs const& e);
        void OnNavigatedFrom(Windows::UI::Xaml::Navigation::NavigationEventArgs const& e);

        void OnLoaded(Windows::Foundation::IInspectable const& sender, Windows::UI::Xaml::RoutedEventArgs const& args);
        void OnStartClick(Windows::Foundation::IInspectable const& sender, Windows::UI::Xaml::RoutedEventArgs const& args);
        void OnStopClick(Windows::Foundation::IInspectable const& sender, Windows::UI::Xaml::RoutedEventArgs const& args);
        void OnDiagnosticsClick(Windows::Foundation::IInspectable const& sender, Windows::UI::Xaml::RoutedEventArgs const& args);
        void OnHelpClick(Windows::Foundation::IInspectable const& sender, Windows::UI::Xaml::RoutedEventArgs const& args);
        void OnFirstRunContinueClick(Windows::Foundation::IInspectable const& sender, Windows::UI::Xaml::RoutedEventArgs const& args);
        void OnPortLostFocus(Windows::Foundation::IInspectable const& sender, Windows::UI::Xaml::RoutedEventArgs const& args);
        void OnAudioToggled(Windows::Foundation::IInspectable const& sender, Windows::UI::Xaml::RoutedEventArgs const& args);
        void OnTolerantToggled(Windows::Foundation::IInspectable const& sender, Windows::UI::Xaml::RoutedEventArgs const& args);
        void OnAutoStartToggled(Windows::Foundation::IInspectable const& sender, Windows::UI::Xaml::RoutedEventArgs const& args);
        void OnAvMinusClick(Windows::Foundation::IInspectable const& sender, Windows::UI::Xaml::RoutedEventArgs const& args);
        void OnAvPlusClick(Windows::Foundation::IInspectable const& sender, Windows::UI::Xaml::RoutedEventArgs const& args);
        void OnSelfTestClick(Windows::Foundation::IInspectable const& sender, Windows::UI::Xaml::RoutedEventArgs const& args);
        void OnPocSocketsClick(Windows::Foundation::IInspectable const& sender, Windows::UI::Xaml::RoutedEventArgs const& args);
        void OnPocClipClick(Windows::Foundation::IInspectable const& sender, Windows::UI::Xaml::RoutedEventArgs const& args);
        void OnPocToneClick(Windows::Foundation::IInspectable const& sender, Windows::UI::Xaml::RoutedEventArgs const& args);

    private:
        fire_and_forget StartReceiverAsync();
        fire_and_forget StopReceiverAsync();
        fire_and_forget ShutdownAsync();
        fire_and_forget TogglePocSocketsAsync();
        fire_and_forget TogglePocClipAsync();
        fire_and_forget TogglePocToneAsync();
        fire_and_forget OnSuspending(Windows::Foundation::IInspectable sender, Windows::ApplicationModel::SuspendingEventArgs args);
        fire_and_forget OnResuming(Windows::Foundation::IInspectable sender, Windows::Foundation::IInspectable args);

        void OnUiTick();
        void OnCoreKeyDown(Windows::UI::Core::CoreWindow const& sender, Windows::UI::Core::KeyEventArgs const& args);
        void OnBackRequested(Windows::Foundation::IInspectable const& sender, Windows::UI::Core::BackRequestedEventArgs const& args);

        void LoadSettingsIntoUi();
        bool ReadPortsFromUi();
        void UpdateStatus(rx::ConnectionState state, rx::ReceiverStats::Snapshot const& snapshot);
        void UpdateDiagnostics(rx::ReceiverStats::Snapshot const& snapshot);
        void UpdateDisplayRequest(rx::ConnectionState state);
        void UpdateControls();
        void UpdateAutoHide(rx::ConnectionState state);
        void ShowPanel(bool show);
        void SetDiagnosticsVisible(bool visible);
        void ShowFirstRun(bool show);
        void StopPocs();
        bool PanelVisible();
        bool FirstRunVisible();

        rx::ReceiverSettings m_settings;
        Windows::Media::Playback::MediaPlayer m_player{ nullptr };
        std::shared_ptr<rx::ReceiverSession> m_session;
        Windows::UI::Xaml::DispatcherTimer m_uiTimer{ nullptr };
        Windows::System::Display::DisplayRequest m_displayRequest{ nullptr };
        bool m_displayRequestActive = false;
        bool m_wasRunningBeforeSuspend = false;
        bool m_busy = false;
        bool m_loadingUi = false;
        winrt::hstring m_uiMessage;
        winrt::hstring m_selfTestSummary;

        rx::ReceiverStats::Snapshot m_lastSnapshot{};
        rx::Clock::time_point m_lastSnapshotTime{};
        double m_fps = 0.0;
        double m_videoMbps = 0.0;
        double m_audioKbps = 0.0;

        rx::ConnectionState m_lastState = rx::ConnectionState::Stopped;
        rx::Clock::time_point m_lastInput{};
        rx::Clock::time_point m_receivingSince{};

#ifdef _DEBUG
        std::shared_ptr<rx::PocSocketCounter> m_pocSockets;
        std::shared_ptr<rx::PocClipPlayer> m_pocClip;
        std::shared_ptr<rx::PocToneTest> m_pocTone;
        size_t m_toneStep = 0;
#endif

        Windows::UI::Xaml::DispatcherTimer::Tick_revoker m_tickRevoker;
        Windows::UI::Xaml::Application::Suspending_revoker m_suspendingRevoker;
        Windows::UI::Xaml::Application::Resuming_revoker m_resumingRevoker;
        Windows::UI::Core::CoreWindow::KeyDown_revoker m_keyDownRevoker;
        Windows::UI::Core::SystemNavigationManager::BackRequested_revoker m_backRevoker;
    };
}

namespace winrt::UwpSmokeTestCpp::factory_implementation
{
    struct MainPage : MainPageT<MainPage, implementation::MainPage>
    {
    };
}
