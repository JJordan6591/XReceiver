#include "pch.h"
#include "ReceiverSettings.h"

#include <algorithm>

using namespace winrt;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Foundation::Collections;
using namespace winrt::Windows::Storage;

namespace rx
{
    namespace
    {
        int32_t ReadInt(IPropertySet const& values, wchar_t const* key, int32_t fallback)
        {
            auto value = values.TryLookup(key);
            return value ? unbox_value_or<int32_t>(value, fallback) : fallback;
        }

        bool ReadBool(IPropertySet const& values, wchar_t const* key, bool fallback)
        {
            auto value = values.TryLookup(key);
            return value ? unbox_value_or<bool>(value, fallback) : fallback;
        }

        void RemoveObsoleteKeys(IPropertySet& values)
        {
            for (wchar_t const* obsolete : {
                     L"keyframeWaitTimeoutMs",
                     L"acceptRecoveryPoint",
                     L"timestampMode",
                     L"resetTimeoutMs",
                     L"videoPtsLeadMs",
                 })
            {
                if (values.HasKey(obsolete))
                {
                    values.Remove(obsolete);
                }
            }
        }

        bool HasAnyUserKey(IPropertySet const& values)
        {
            for (wchar_t const* key : {
                     L"videoPort",
                     L"audioPort",
                     L"videoPayloadType",
                     L"audioPayloadType",
                     L"audioEnabled",
                     L"autoStart",
                     L"diagnosticsVisible",
                     L"videoReorderTimeoutMs",
                     L"videoReorderWindow",
                     L"audioReorderTimeoutMs",
                     L"audioReorderWindow",
                     L"frameQueueDepth",
                     L"maxFrameAgeMs",
                     L"openQueueCap",
                     L"startupQueueCap",
                     L"frameQueueMaxBytes",
                     L"lossPolicy",
                     L"rebuildOnFormatChange",
                     L"socketBufferBytes",
                     L"audioMinDelayMs",
                     L"audioMaxDelayMs",
                     L"videoPipelineLatencyMs",
                     L"avOffsetMs",
                     L"idleTimeoutMs",
                     L"ssrcTakeoverMs",
                     L"firstRunDismissed",
                 })
            {
                if (values.HasKey(key))
                {
                    return true;
                }
            }
            return false;
        }
    }

    bool ReceiverSettings::HasPersistedUserSettings(IPropertySet const& values)
    {
        return HasAnyUserKey(values);
    }

    void ReceiverSettings::MigrateStoredSettings(IPropertySet& values)
    {
        int32_t const storedVersion = ReadInt(values, L"settingsVersion", 0);
        if (storedVersion > kCurrentSettingsVersion)
        {
            return;
        }

        RemoveObsoleteKeys(values);

        if (storedVersion < kCurrentSettingsVersion)
        {
            if (storedVersion == 0 && HasAnyUserKey(values) && !values.HasKey(L"diagnosticsVisible"))
            {
                values.Insert(L"diagnosticsVisible", box_value(true));
            }
            if (values.HasKey(L"settingsVersion"))
            {
                values.Remove(L"settingsVersion");
            }
            values.Insert(L"settingsVersion", box_value(kCurrentSettingsVersion));
        }
    }

    ReceiverSettings ReceiverSettings::LoadFromValues(IPropertySet const& values)
    {
        ReceiverSettings s;
        int32_t const storedVersion = ReadInt(values, L"settingsVersion", 0);
        bool const legacyUnversioned = storedVersion == 0 && HasPersistedUserSettings(values);
        bool const diagnosticsFallback = legacyUnversioned ? true : s.diagnosticsVisible;

        s.videoPort = ReadInt(values, L"videoPort", s.videoPort);
        s.audioPort = ReadInt(values, L"audioPort", s.audioPort);
        s.videoPayloadType = ReadInt(values, L"videoPayloadType", s.videoPayloadType);
        s.audioPayloadType = ReadInt(values, L"audioPayloadType", s.audioPayloadType);
        s.audioEnabled = ReadBool(values, L"audioEnabled", s.audioEnabled);
        s.autoStart = ReadBool(values, L"autoStart", s.autoStart);
        s.diagnosticsVisible = ReadBool(values, L"diagnosticsVisible", diagnosticsFallback);
        s.firstRunDismissed = ReadBool(values, L"firstRunDismissed", s.firstRunDismissed);
        s.videoReorderTimeoutMs = ReadInt(values, L"videoReorderTimeoutMs", s.videoReorderTimeoutMs);
        s.videoReorderWindow = ReadInt(values, L"videoReorderWindow", s.videoReorderWindow);
        s.audioReorderTimeoutMs = ReadInt(values, L"audioReorderTimeoutMs", s.audioReorderTimeoutMs);
        s.audioReorderWindow = ReadInt(values, L"audioReorderWindow", s.audioReorderWindow);
        s.frameQueueDepth = ReadInt(values, L"frameQueueDepth", s.frameQueueDepth);
        s.maxFrameAgeMs = ReadInt(values, L"maxFrameAgeMs", s.maxFrameAgeMs);
        s.openQueueCap = ReadInt(values, L"openQueueCap", s.openQueueCap);
        s.startupQueueCap = ReadInt(values, L"startupQueueCap", s.startupQueueCap);
        s.frameQueueMaxBytes = ReadInt(values, L"frameQueueMaxBytes", s.frameQueueMaxBytes);
        s.lossPolicy = static_cast<LossPolicy>(ReadInt(values, L"lossPolicy", static_cast<int32_t>(s.lossPolicy)));
        s.rebuildOnFormatChange = ReadBool(values, L"rebuildOnFormatChange", s.rebuildOnFormatChange);
        s.socketBufferBytes = ReadInt(values, L"socketBufferBytes", s.socketBufferBytes);
        s.audioMinDelayMs = ReadInt(values, L"audioMinDelayMs", s.audioMinDelayMs);
        s.audioMaxDelayMs = ReadInt(values, L"audioMaxDelayMs", s.audioMaxDelayMs);
        s.videoPipelineLatencyMs = ReadInt(values, L"videoPipelineLatencyMs", s.videoPipelineLatencyMs);
        s.avOffsetMs = ReadInt(values, L"avOffsetMs", s.avOffsetMs);
        s.idleTimeoutMs = ReadInt(values, L"idleTimeoutMs", s.idleTimeoutMs);
        s.ssrcTakeoverMs = ReadInt(values, L"ssrcTakeoverMs", s.ssrcTakeoverMs);
        return s;
    }

    ReceiverSettings ReceiverSettings::Load()
    {
        ReceiverSettings s;
        try
        {
            auto values = ApplicationData::Current().LocalSettings().Values();
            MigrateStoredSettings(values);
            s = LoadFromValues(values);
        }
        catch (hresult_error const&)
        {
            s = ReceiverSettings{};
        }
        s.Sanitize();
        return s;
    }

    void ReceiverSettings::WriteToValues(IPropertySet& values) const
    {
        values.Insert(L"settingsVersion", box_value(kCurrentSettingsVersion));
        values.Insert(L"videoPort", box_value(videoPort));
        values.Insert(L"audioPort", box_value(audioPort));
        values.Insert(L"videoPayloadType", box_value(videoPayloadType));
        values.Insert(L"audioPayloadType", box_value(audioPayloadType));
        values.Insert(L"audioEnabled", box_value(audioEnabled));
        values.Insert(L"autoStart", box_value(autoStart));
        values.Insert(L"diagnosticsVisible", box_value(diagnosticsVisible));
        values.Insert(L"firstRunDismissed", box_value(firstRunDismissed));
        values.Insert(L"videoReorderTimeoutMs", box_value(videoReorderTimeoutMs));
        values.Insert(L"videoReorderWindow", box_value(videoReorderWindow));
        values.Insert(L"audioReorderTimeoutMs", box_value(audioReorderTimeoutMs));
        values.Insert(L"audioReorderWindow", box_value(audioReorderWindow));
        values.Insert(L"frameQueueDepth", box_value(frameQueueDepth));
        values.Insert(L"maxFrameAgeMs", box_value(maxFrameAgeMs));
        values.Insert(L"openQueueCap", box_value(openQueueCap));
        values.Insert(L"startupQueueCap", box_value(startupQueueCap));
        values.Insert(L"frameQueueMaxBytes", box_value(frameQueueMaxBytes));
        values.Insert(L"lossPolicy", box_value(static_cast<int32_t>(lossPolicy)));
        values.Insert(L"rebuildOnFormatChange", box_value(rebuildOnFormatChange));
        values.Insert(L"socketBufferBytes", box_value(socketBufferBytes));
        values.Insert(L"audioMinDelayMs", box_value(audioMinDelayMs));
        values.Insert(L"audioMaxDelayMs", box_value(audioMaxDelayMs));
        values.Insert(L"videoPipelineLatencyMs", box_value(videoPipelineLatencyMs));
        values.Insert(L"avOffsetMs", box_value(avOffsetMs));
        values.Insert(L"idleTimeoutMs", box_value(idleTimeoutMs));
        values.Insert(L"ssrcTakeoverMs", box_value(ssrcTakeoverMs));
        RemoveObsoleteKeys(values);
    }

    void ReceiverSettings::Save() const
    {
        try
        {
            auto values = ApplicationData::Current().LocalSettings().Values();
            WriteToValues(values);
        }
        catch (hresult_error const&)
        {
        }
    }

    void ReceiverSettings::Sanitize()
    {
        ReceiverSettings const defaults;
        if (!IsValidPort(videoPort)) videoPort = defaults.videoPort;
        if (!IsValidPort(audioPort)) audioPort = defaults.audioPort;
        if (videoPort == audioPort)
        {
            videoPort = defaults.videoPort;
            audioPort = defaults.audioPort;
        }
        videoPayloadType = std::clamp(videoPayloadType, 0, 127);
        audioPayloadType = std::clamp(audioPayloadType, 0, 127);
        videoReorderTimeoutMs = std::clamp(videoReorderTimeoutMs, 0, 200);
        videoReorderWindow = std::clamp(videoReorderWindow, 8, 1000);
        audioReorderTimeoutMs = std::clamp(audioReorderTimeoutMs, 0, 200);
        audioReorderWindow = std::clamp(audioReorderWindow, 4, 200);
        frameQueueDepth = std::clamp(frameQueueDepth, 1, 6);
        maxFrameAgeMs = std::clamp(maxFrameAgeMs, 20, 1000);
        openQueueCap = std::clamp(openQueueCap, frameQueueDepth, 240);
        startupQueueCap = std::clamp(startupQueueCap, openQueueCap, 1200);
        frameQueueMaxBytes = std::clamp(frameQueueMaxBytes, kMinFrameQueueBytes, kMaxFrameQueueBytes);
        if (lossPolicy != LossPolicy::Strict && lossPolicy != LossPolicy::Tolerant) lossPolicy = defaults.lossPolicy;
        socketBufferBytes = std::clamp(socketBufferBytes, 64 * 1024, 16 * 1024 * 1024);
        audioMinDelayMs = std::clamp(audioMinDelayMs, 5, 500);
        audioMaxDelayMs = std::clamp(audioMaxDelayMs, audioMinDelayMs, 1000);
        videoPipelineLatencyMs = std::clamp(videoPipelineLatencyMs, 0, 1000);
        avOffsetMs = std::clamp(avOffsetMs, -500, 500);
        idleTimeoutMs = std::clamp(idleTimeoutMs, kMinIdleTimeoutMs, 60000);
        ssrcTakeoverMs = std::clamp(ssrcTakeoverMs, 100, 60000);
    }
}
