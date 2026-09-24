#pragma once

namespace rx
{
    struct SelfTestResult
    {
        int passed = 0;
        int failed = 0;
        winrt::hstring summary;
    };

    // Parser, depacketizer, gate and audio conversion tests on synthesized packets.
    SelfTestResult RunSelfTests();
}
