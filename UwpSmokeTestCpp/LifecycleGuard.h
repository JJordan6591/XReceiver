#pragma once

#include <atomic>
#include <cstdint>

namespace rx
{
    // Token for asynchronous lifecycle work. Start and Stop each advance it; work that was
    // scheduled under an older token must not publish state, timers or teardown.
    class LifecycleGeneration
    {
    public:
        uint64_t Advance() noexcept { return m_value.fetch_add(1) + 1; }
        uint64_t Current() const noexcept { return m_value.load(); }
        bool IsCurrent(uint64_t token) const noexcept { return m_value.load() == token; }

    private:
        std::atomic<uint64_t> m_value{ 0 };
    };

    // Set exactly once from any thread; only the first caller sees true.
    class OnceFlag
    {
    public:
        bool Set() noexcept { return !m_set.exchange(true); }
        bool IsSet() const noexcept { return m_set.load(); }

    private:
        std::atomic<bool> m_set{ false };
    };
}
