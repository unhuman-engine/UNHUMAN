#pragma once
#include <cstddef>
#include <functional>
#include <utility>
#include <vector>
#include <version>

namespace UHE::RHI
{

/**
 * \brief LIFO queue of deferred destructors, flushed once per frame.
 *
 * GPU resources must outlive the frame that still references them, so "destroy"
 * means "enqueue" here: the real teardown runs only after the owning frame's
 * fence/timeline has passed (see VulkanFrameContext).
 *
 * Deletors are stored as \c std::move_only_function (C++23), which lets callers
 * capture move-only resources — like the \c vk::raii smart handles — without
 * wrapping them in a \c shared_ptr.
 */
class DeletionQueue
{
public:
    using Deleter = std::move_only_function<void()>;

    /// Enqueues \p fn. It runs on the next Flush(), in reverse order of insertion.
    void Push(Deleter&& fn) { m_deletors.push_back(std::move(fn)); }

    /// Runs every pending deleter once, newest first (reverse of creation order).
    void Flush()
    {
        while (!m_deletors.empty())
        {
            std::vector<Deleter> batch;
            batch.swap(m_deletors);
            for (auto it = batch.rbegin(); it != batch.rend(); ++it)
            {
                if (*it)
                {
                    (*it)();
                }
            }
        }
    }

    [[nodiscard]] bool Empty() const { return m_deletors.empty(); }
    [[nodiscard]] std::size_t Size() const { return m_deletors.size(); }

private:
    std::vector<Deleter> m_deletors;
};

} // namespace UHE::RHI
