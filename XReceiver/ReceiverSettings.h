#pragma once

#include <cstdint>

#include "UiChrome.h"

namespace winrt::Windows::Foundation::Collections
{
    struct IPropertySet;
}

namespace rx
{
    enum class LossPolicy : int32_t
    {
        Strict = 0,     // after a lost reference picture, discard until the next complete IDR
        Tolerant = 1,   // drop only damaged pictures and keep decoding complete ones
    };

    struct ReceiverSettings
    {
        int32_t videoPort = 5000;
        int32_t audioPort = 5002;
        int32_t videoPayloadType = 96;
        int32_t audioPayloadType = 96;
        bool audioEnabled = true;
        bool autoStart = true;
        bool diagnosticsVisible = false;
        bool firstRunDismissed = false;
        PlaybackOverlay playbackOverlay = PlaybackOverlay::Automatic;

        int32_t videoReorderTimeoutMs = 10;
        int32_t videoReorderWindow = 256;
        int32_t audioReorderTimeoutMs = 15;
        int32_t audioReorderWindow = 32;

        int32_t frameQueueDepth = 2;
        int32_t maxFrameAgeMs = 100;
        int32_t openQueueCap = 60;
        int32_t startupQueueCap = 600;
        int32_t frameQueueMaxBytes = 64 * 1024 * 1024;

        LossPolicy lossPolicy = LossPolicy::Strict;

        bool rebuildOnFormatChange = true;

        int32_t socketBufferBytes = 2 * 1024 * 1024;

        int32_t audioMinDelayMs = 20;
        int32_t audioMaxDelayMs = 300;
        int32_t videoPipelineLatencyMs = 60;
        int32_t avOffsetMs = 0;

        // Neither audio nor video for this long means the sender has stalled. AirPlay sends few
        // or no video frames for a static screen, so this is never shorter than 5 s.
        int32_t idleTimeoutMs = 5000;
        int32_t ssrcTakeoverMs = 1000;

        static constexpr int32_t kMinIdleTimeoutMs = 5000;

        // The minimum holds two of the largest access units the depacketizer emits (4 MiB), so
        // an IDR can always be admitted.
        static constexpr int32_t kMinFrameQueueBytes = 8 * 1024 * 1024;
        static constexpr int32_t kMaxFrameQueueBytes = 256 * 1024 * 1024;

        static constexpr int32_t kCurrentSettingsVersion = 3;

        static ReceiverSettings Load();
        static ReceiverSettings LoadFromValues(winrt::Windows::Foundation::Collections::IPropertySet const& values);
        static bool HasPersistedUserSettings(winrt::Windows::Foundation::Collections::IPropertySet const& values);
        static void MigrateStoredSettings(winrt::Windows::Foundation::Collections::IPropertySet& values);
        void Save() const;
        void WriteToValues(winrt::Windows::Foundation::Collections::IPropertySet& values) const;
        void Sanitize();

        static bool IsValidPort(int32_t port) { return port >= 1024 && port <= 65535; }
    };
}
