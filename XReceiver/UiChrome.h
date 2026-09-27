#pragma once

#include <cstddef>
#include <cstdint>
#include <stdio.h>
#include <wchar.h>

namespace rx
{
    // User-visible product name. Package identity is separate and stays stable across renames.
    inline wchar_t const* AppDisplayName() { return L"XReceiver"; }

    enum class PlaybackOverlay : int32_t
    {
        Automatic = 0,
        AlwaysVisible = 1,
        VideoOnly = 2,
    };

    inline PlaybackOverlay SanitizeOverlay(int32_t raw)
    {
        if (raw == static_cast<int32_t>(PlaybackOverlay::AlwaysVisible))
        {
            return PlaybackOverlay::AlwaysVisible;
        }
        if (raw == static_cast<int32_t>(PlaybackOverlay::VideoOnly))
        {
            return PlaybackOverlay::VideoOnly;
        }
        return PlaybackOverlay::Automatic;
    }

    inline wchar_t const* OverlayLabel(PlaybackOverlay mode)
    {
        switch (mode)
        {
        case PlaybackOverlay::AlwaysVisible: return L"Always visible";
        case PlaybackOverlay::VideoOnly: return L"Video only";
        default: return L"Automatic";
        }
    }

    enum class ChromeState : int32_t
    {
        Stopped,
        Starting,
        Waiting,
        Receiving,
        Reconnecting,
        Error,
    };

    struct ChromeInput
    {
        PlaybackOverlay overlay = PlaybackOverlay::Automatic;
        ChromeState state = ChromeState::Stopped;
        bool usableVideo = false;
        bool waitingForKeyframe = false;
        bool panelOpen = false;
        bool diagnosticsOpen = false;
        bool statusHold = false;
        bool firstRun = false;
    };

    struct ChromeVisibility
    {
        bool status = false;
        bool controls = false;
        bool diagnostics = false;
        bool waitingGuide = false;
        bool videoClean = false;
    };

    // Presentation only. Never starts, stops, or rebuilds the media pipeline.
    inline ChromeVisibility DecideChrome(ChromeInput const& in)
    {
        ChromeVisibility out;
        if (in.firstRun)
        {
            return out;
        }

        bool const videoOnlyClean = in.overlay == PlaybackOverlay::VideoOnly && in.usableVideo && !in.waitingForKeyframe &&
                                     in.state == ChromeState::Receiving;
        if (videoOnlyClean)
        {
            out.controls = in.panelOpen;
            out.diagnostics = in.diagnosticsOpen;
            out.videoClean = !out.controls && !out.diagnostics;
            return out;
        }

        if (in.overlay == PlaybackOverlay::AlwaysVisible)
        {
            out.status = true;
        }
        else if (in.overlay == PlaybackOverlay::Automatic && in.usableVideo && in.state == ChromeState::Receiving && !in.waitingForKeyframe)
        {
            out.status = in.statusHold;
        }
        else
        {
            out.status = true;
        }

        out.controls = in.panelOpen;
        out.diagnostics = in.diagnosticsOpen;
        out.waitingGuide = out.status && in.state == ChromeState::Waiting;
        out.videoClean = false;
        return out;
    }

    enum class ChromeAction
    {
        Menu,
        View,
        Back,
    };

    // Menu, View, and B change panels only. Playback state is left untouched.
    inline ChromeInput ApplyChromeAction(ChromeInput in, ChromeAction action)
    {
        switch (action)
        {
        case ChromeAction::Menu:
            in.panelOpen = !in.panelOpen;
            break;
        case ChromeAction::View:
            in.diagnosticsOpen = !in.diagnosticsOpen;
            break;
        case ChromeAction::Back:
            in.panelOpen = false;
            in.diagnosticsOpen = false;
            break;
        }
        return in;
    }

    enum class FocusTarget
    {
        None,
        Continue,
        Start,
        Stop,
    };

    inline FocusTarget FocusForChrome(ChromeInput const& in, ChromeVisibility const& visible, bool receiverActive)
    {
        if (in.firstRun)
        {
            return FocusTarget::Continue;
        }
        if (!visible.controls)
        {
            return FocusTarget::None;
        }
        return receiverActive ? FocusTarget::Stop : FocusTarget::Start;
    }

    struct StatusCopy
    {
        wchar_t const* title;
        wchar_t const* detail;
    };

    inline StatusCopy StatusFor(ChromeState state, bool waitingForKeyframe, bool audioEnabled, bool audioFailed)
    {
        if (state == ChromeState::Receiving && waitingForKeyframe)
        {
            return { L"Waiting for video keyframe", L"Showing the last picture until video recovers." };
        }
        switch (state)
        {
        case ChromeState::Waiting:
            return { L"Ready to connect", L"Use the same local network as this Xbox." };
        case ChromeState::Starting:
            return { L"Starting receiver", L"Opening listeners." };
        case ChromeState::Receiving:
            if (!audioEnabled || audioFailed)
            {
                return { L"Receiving", L"Audio unavailable. Video continues." };
            }
            return { L"Receiving", L"Video and audio are arriving." };
        case ChromeState::Reconnecting:
            return { L"Connection interrupted", L"Waiting for media to resume." };
        case ChromeState::Error:
            return { L"Playback problem", L"Open Help, then stop and start the receiver." };
        case ChromeState::Stopped:
        default:
            return { L"Receiver stopped", L"Press Start when you are ready." };
        }
    }

    inline void FormatAvOffset(int32_t milliseconds, wchar_t* out, size_t outChars)
    {
        if (out == nullptr || outChars == 0)
        {
            return;
        }
        if (milliseconds > 0)
        {
            swprintf_s(out, outChars, L"+%d ms, audio later", milliseconds);
        }
        else if (milliseconds < 0)
        {
            swprintf_s(out, outChars, L"%d ms, audio earlier", milliseconds);
        }
        else
        {
            wcscpy_s(out, outChars, L"0 ms");
        }
    }

    // A saved port is always in range; anything else is shown as unavailable rather than as the
    // active configuration.
    inline bool IsDisplayablePort(int32_t port) { return port >= 1024 && port <= 65535; }

    inline void FormatPort(int32_t port, wchar_t* out, size_t outChars)
    {
        if (IsDisplayablePort(port))
        {
            swprintf_s(out, outChars, L"%d", port);
        }
        else
        {
            wcscpy_s(out, outChars, L"—");
        }
    }

    // Waiting status line for the configured listeners, e.g. "Video UDP 5000, audio UDP 5002."
    inline void FormatListeningPorts(int32_t videoPort, int32_t audioPort, bool audioEnabled, wchar_t* out, size_t outChars)
    {
        if (out == nullptr || outChars == 0)
        {
            return;
        }
        wchar_t video[8] = {};
        wchar_t audio[8] = {};
        FormatPort(videoPort, video, 8);
        FormatPort(audioPort, audio, 8);
        if (audioEnabled)
        {
            swprintf_s(out, outChars, L"Video UDP %s, audio UDP %s.", video, audio);
        }
        else
        {
            swprintf_s(out, outChars, L"Video UDP %s.", video);
        }
    }

    // Onboarding network line with the configured ports.
    inline void FormatOnboardingNetwork(int32_t videoPort, int32_t audioPort, wchar_t* out, size_t outChars)
    {
        if (out == nullptr || outChars == 0)
        {
            return;
        }
        wchar_t video[8] = {};
        wchar_t audio[8] = {};
        FormatPort(videoPort, video, 8);
        FormatPort(audioPort, audio, 8);
        swprintf_s(out, outChars, L"Put this Xbox and the UxPlay computer on the same local network. Video is UDP %s. Audio is UDP %s.",
                   video, audio);
    }

    inline void FormatUnavailable(bool available, int64_t value, wchar_t const* suffix, wchar_t* out, size_t outChars)
    {
        if (out == nullptr || outChars == 0)
        {
            return;
        }
        if (!available)
        {
            wcscpy_s(out, outChars, L"\u2014");
            return;
        }
        swprintf_s(out, outChars, L"%lld%s", static_cast<long long>(value), suffix == nullptr ? L"" : suffix);
    }

    struct ControlAvailability
    {
        bool start = false;
        bool stop = false;
        bool ports = false;
        bool audioToggle = false;
    };

    // Ports and audio change only while stopped. Overlay and A/V offset are never disabled; they apply at once.
    inline ControlAvailability ControlsFor(bool receiverActive, bool busy)
    {
        ControlAvailability out;
        out.start = !receiverActive && !busy;
        out.stop = receiverActive && !busy;
        out.ports = !receiverActive && !busy;
        out.audioToggle = !receiverActive && !busy;
        return out;
    }
}
