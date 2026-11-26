#pragma once
#include <atomic>
#include <condition_variable>
#include <coroutine>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include "UHE/Jobsystem/WorkStealingDeque.h"

namespace UHE::Jobsystem
{

// Self-contained alias so this header compiles even when included without the
// engine-wide precompiled header (where u32 comes from UHE/Core/Core.h).
using u32 = std::uint32_t;

// Represents a single task to execute
using JobFunction = std::function<void()>;

/**
 * @brief Counts the unfinished jobs of a group; Wait() and Then() watch it reach zero.
 *
 * A counter may be destroyed once Wait() on it has returned (or its last
 * continuation has run): the job that finishes the group still touches it
 * after the count reaches zero, and Wait() waits for that too.
 */
struct JobCounter
{
    std::atomic<int> count{0};

    // Continuations (UheJobsystem::Then) and the completers still inside
    // FinishOne after the count reached zero. Scheduler-internal.
    std::mutex continuationMutex;
    std::vector<JobFunction> continuations;
    std::atomic<int> finishing{0};
};

/**
 * @brief The job scheduler: one thread per core, each with its own work-stealing deque.
 *
 * A job a worker kicks goes on that worker's deque, and the worker takes its
 * newest job first; idle threads steal the oldest jobs from the others. The
 * thread that called Init() owns deque 0 and helps while it waits; any other
 * thread's jobs go through a shared injection queue. Idle workers sleep and
 * are woken when work arrives.
 */
class UheJobsystem
{
public:
    UheJobsystem() = default;
    ~UheJobsystem();

    UheJobsystem(const UheJobsystem&) = delete;
    UheJobsystem& operator=(const UheJobsystem&) = delete;

    // Initializes the thread pool.
    // Will detect hardware cores (very important for Android big/LITTLE architectures)
    void Init();

    /// Stops the workers. Jobs still queued run on the caller first: someone
    /// may be waiting on their counter.
    void ShutDown();

    /// Kicks one job. With a counter, the counter counts it until it finishes.
    /// Before Init or after ShutDown the job runs inline.
    void Execute(JobFunction job, JobCounter* counter = nullptr);

    /**
     * @brief Runs \p job once \p counter reaches zero, without blocking a thread.
     *
     * A continuation is how a job waits on its children without Wait(): Wait()
     * runs other jobs on the waiting thread's stack, so nested waits grow it.
     * When the counter is already zero the job is kicked at once. With
     * \p signal, the continuation counts on it like any job.
     */
    void Then(JobCounter& counter, JobFunction job, JobCounter* signal = nullptr);

    // Schedules 'jobCount' jobs by calling 'fn(i)' for i in [0, jobCount).
    // The range is split into chunks so each worker gets a contiguous slice
    // (less contention than one job per element for cheap iterations).
    // Blocks until ALL iterations have completed.
    void ParallelFor(u32 jobCount, const std::function<void(u32)>& fn, u32 minBatchSize = 1);

    /**
     * @brief ParallelFor over ranges: \p fn(begin, end) per batch, for loops that vectorize.
     *
     * Every batch but the last starts and ends on a multiple of \p alignment
     * (the SIMD width in elements, e.g. 8 for AVX floats), so a batch's body
     * runs whole vectors with no scalar head; only the final batch has a tail.
     * Blocks until every batch has run.
     */
    void ParallelForRange(u32 count, const std::function<void(u32, u32)>& fn, u32 minBatchSize = 1, u32 alignment = 1);

    /// Blocks until \p counter is zero, running other jobs meanwhile.
    void Wait(JobCounter* counter);

    // Blocks until every job kicked so far has completed.
    // Unlike ShutDown, the jobsystem keeps running afterwards.
    void WaitForAll();

    // True while any job kicked through this jobsystem is still running/queued.
    bool IsBusy() const;

    // Get number of worker threads (Useful for allocating 1 Vulkan CommandPool per thread!)
    u32 GetThreadCount() const;

    // Get the ID of the current thread (0 to N). Useful for indexing into thread-local arrays.
    static u32 GetCurrentThreadIndex();

    /// Runs one ready job on the calling thread, if there is one. Used by
    /// helpers that wait (TaskGraph); \p outJob is unused and kept for callers.
    bool TryPopJob(std::pair<JobFunction, JobCounter*>& outJob);
    /// The same without the legacy argument.
    bool RunOneJob();

    // ── Await-style suspension (C++20 coroutines) ──────────────────────────
    // A job written as a coroutine (returning JobTask) suspends with
    // `co_await jobs.WhenDone(counter)` until a group finishes, or moves itself
    // onto a worker with `co_await jobs.Schedule()`. While suspended it holds
    // no thread and no stack: it resumes as a continuation.

    /// Resumes the awaiting coroutine as a job on a worker.
    struct ScheduleAwaiter
    {
        UheJobsystem* Jobs;
        bool await_ready() const noexcept { return false; }
        void await_suspend(std::coroutine_handle<> handle) const
        {
            Jobs->Execute([handle] { handle.resume(); });
        }
        void await_resume() const noexcept {}
    };
    [[nodiscard]] ScheduleAwaiter Schedule() { return ScheduleAwaiter{this}; }

    /// Resumes the awaiting coroutine once \p counter is zero (at once when it already is).
    struct CounterAwaiter
    {
        UheJobsystem* Jobs;
        JobCounter* Counter;
        bool await_ready() const noexcept
        {
            // Not while a completer is still inside: the coroutine may free the counter next.
            return Counter->count.load(std::memory_order_acquire) == 0 &&
                   Counter->finishing.load(std::memory_order_acquire) == 0;
        }
        void await_suspend(std::coroutine_handle<> handle) const
        {
            Jobs->Then(*Counter, [handle] { handle.resume(); });
        }
        void await_resume() const noexcept {}
    };
    [[nodiscard]] CounterAwaiter WhenDone(JobCounter& counter) { return CounterAwaiter{this, &counter}; }

private:
    struct Job
    {
        JobFunction Function;
        JobCounter* Counter = nullptr;
    };

    void WorkerThread(u32 threadIndex);
    void Push(Job* job);
    /// The calling thread's next job: its own deque, the injection queue, then a steal.
    Job* FindJob();
    void Run(Job* job);
    void FinishOne(JobCounter* counter);
    void WakeOne();
    /// The deque the calling thread owns, or nullptr when it owns none here.
    WorkStealingDeque<Job>* OwnQueue() const;

    std::vector<std::thread> m_Workers;
    std::vector<std::unique_ptr<WorkStealingDeque<Job>>> m_Queues; ///< [0] the Init thread's, [i] worker i's

    std::mutex m_InjectMutex;
    std::deque<Job*> m_Injected; ///< jobs from threads that own no deque
    std::atomic<int> m_InjectedCount{0};

    std::mutex m_SleepMutex;
    std::condition_variable m_WakeCondition;
    std::atomic<int> m_Sleepers{0};
    std::atomic<int> m_Queued{0}; ///< pushed and not yet taken

    std::atomic<bool> m_IsRunning{false};
    std::atomic<int> m_Submitting{0};   ///< Execute calls between their running check and their push
    std::atomic<int> m_JobsInFlight{0}; ///< kicked and not yet finished
};

/**
 * @brief A job written as a coroutine: starts at once, runs to its first co_await, and frees itself at the end.
 *
 * Fire-and-forget; report completion through a JobCounter the coroutine
 * finishes (or a value it sets). An exception escaping the coroutine
 * terminates, as one escaping a plain job would.
 */
struct JobTask
{
    struct promise_type
    {
        JobTask get_return_object() noexcept { return {}; }
        std::suspend_never initial_suspend() noexcept { return {}; }
        std::suspend_never final_suspend() noexcept { return {}; }
        void return_void() noexcept {}
        void unhandled_exception() noexcept { std::terminate(); }
    };
};

} // namespace UHE::Jobsystem
