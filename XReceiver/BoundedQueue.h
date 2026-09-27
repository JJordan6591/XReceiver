#pragma once

#include <cstddef>
#include <deque>
#include <utility>

namespace rx
{
    // Bounded FIFO. Not internally synchronized: the owner holds its own lock so that queue
    // updates and the related pipeline state change atomically together.
    template <typename T>
    class BoundedQueue
    {
    public:
        explicit BoundedQueue(size_t capacity = 1) : m_capacity(capacity < 1 ? 1 : capacity) {}

        void SetCapacity(size_t capacity) { m_capacity = capacity < 1 ? 1 : capacity; }
        size_t Capacity() const { return m_capacity; }
        size_t Size() const { return m_items.size(); }
        bool Empty() const { return m_items.empty(); }
        bool OverCapacity() const { return m_items.size() > m_capacity; }

        void PushBack(T item) { m_items.push_back(std::move(item)); }

        T PopFront()
        {
            T item = std::move(m_items.front());
            m_items.pop_front();
            return item;
        }

        T& Front() { return m_items.front(); }
        T const& Front() const { return m_items.front(); }

        // Removes the first element matching the predicate. Returns true if one was removed.
        template <typename Predicate>
        bool RemoveFirstIf(Predicate&& predicate)
        {
            for (auto it = m_items.begin(); it != m_items.end(); ++it)
            {
                if (predicate(*it))
                {
                    m_items.erase(it);
                    return true;
                }
            }
            return false;
        }

        size_t Clear()
        {
            size_t const count = m_items.size();
            m_items.clear();
            return count;
        }

    private:
        size_t m_capacity;
        std::deque<T> m_items;
    };
}
