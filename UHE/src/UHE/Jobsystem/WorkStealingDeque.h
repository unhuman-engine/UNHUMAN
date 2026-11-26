#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace UHE::Jobsystem
{

/**
 * @brief A Chase–Lev work-stealing deque of pointers.
 *
 * One owner thread pushes and pops at the bottom (LIFO: the job it just made
 * is hot in its cache); any thread steals from the top (FIFO: the oldest,
 * usually largest, piece of work). Lock-free; the ring doubles when full.
 *
 * Follows Lê, Pop, Cohen, Zappa Nardelli, "Correct and Efficient
 * Work-Stealing for Weak Memory Models" (PPoPP 2013), with the two seq_cst
 * fences expressed as seq_cst operations on top/bottom instead: the same
 * ordering, and visible to ThreadSanitizer, which does not model standalone
 * fences and would report the deque as racy.
 *
 * A grown-out ring is kept until the deque dies: a thief may still be reading
 * a slot of the old one.
 */
template <typename T> class WorkStealingDeque
{
public:
    explicit WorkStealingDeque(std::size_t capacity = 256)
    {
        std::size_t size = 1;
        while (size < capacity)
            size <<= 1;
        m_Rings.push_back(std::make_unique<Ring>(size));
        m_Ring.store(m_Rings.back().get(), std::memory_order_relaxed);
    }

    WorkStealingDeque(const WorkStealingDeque&) = delete;
    WorkStealingDeque& operator=(const WorkStealingDeque&) = delete;

    /// @brief Owner only.
    void Push(T* item)
    {
        const std::int64_t bottom = m_Bottom.load(std::memory_order_relaxed);
        const std::int64_t top = m_Top.load(std::memory_order_acquire);
        Ring* ring = m_Ring.load(std::memory_order_relaxed);
        if (bottom - top > static_cast<std::int64_t>(ring->Mask))
        {
            m_Rings.push_back(ring->Grow(top, bottom));
            ring = m_Rings.back().get();
            m_Ring.store(ring, std::memory_order_release);
        }
        ring->At(bottom).store(item, std::memory_order_relaxed);
        m_Bottom.store(bottom + 1, std::memory_order_release);
    }

    /// @brief Owner only: the newest item, or nullptr when empty (or a thief took the last one).
    T* Pop()
    {
        const std::int64_t bottom = m_Bottom.load(std::memory_order_relaxed) - 1;
        Ring* ring = m_Ring.load(std::memory_order_relaxed);
        m_Bottom.store(bottom, std::memory_order_seq_cst);
        std::int64_t top = m_Top.load(std::memory_order_seq_cst);
        if (top > bottom)
        {
            m_Bottom.store(bottom + 1, std::memory_order_relaxed); // was empty
            return nullptr;
        }
        T* item = ring->At(bottom).load(std::memory_order_relaxed);
        if (top == bottom)
        {
            // The last item: race the thieves for it.
            if (!m_Top.compare_exchange_strong(top, top + 1, std::memory_order_seq_cst, std::memory_order_relaxed))
                item = nullptr;
            m_Bottom.store(bottom + 1, std::memory_order_relaxed);
        }
        return item;
    }

    /// @brief Any thread: the oldest item, or nullptr when empty or another thread won it.
    T* Steal()
    {
        std::int64_t top = m_Top.load(std::memory_order_seq_cst);
        const std::int64_t bottom = m_Bottom.load(std::memory_order_seq_cst);
        if (top >= bottom)
            return nullptr;
        Ring* ring = m_Ring.load(std::memory_order_acquire);
        T* item = ring->At(top).load(std::memory_order_relaxed);
        if (!m_Top.compare_exchange_strong(top, top + 1, std::memory_order_seq_cst, std::memory_order_relaxed))
            return nullptr;
        return item;
    }

    /// @brief A hint only: other threads may change it at any moment.
    [[nodiscard]] bool LooksEmpty() const
    {
        return m_Top.load(std::memory_order_relaxed) >= m_Bottom.load(std::memory_order_relaxed);
    }

private:
    struct Ring
    {
        explicit Ring(std::size_t size) : Mask(size - 1), Slots(new std::atomic<T*>[size]) {}

        std::atomic<T*>& At(std::int64_t index) { return Slots[static_cast<std::size_t>(index) & Mask]; }

        std::unique_ptr<Ring> Grow(std::int64_t top, std::int64_t bottom)
        {
            auto bigger = std::make_unique<Ring>((Mask + 1) * 2);
            for (std::int64_t i = top; i < bottom; ++i)
                bigger->At(i).store(At(i).load(std::memory_order_relaxed), std::memory_order_relaxed);
            return bigger;
        }

        std::size_t Mask;
        std::unique_ptr<std::atomic<T*>[]> Slots;
    };

    alignas(64) std::atomic<std::int64_t> m_Top{0};
    alignas(64) std::atomic<std::int64_t> m_Bottom{0};
    alignas(64) std::atomic<Ring*> m_Ring{nullptr};
    std::vector<std::unique_ptr<Ring>> m_Rings; ///< owner only; the last is current
};

} // namespace UHE::Jobsystem
