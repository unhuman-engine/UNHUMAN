#include "Jobsystem.h"
#include <algorithm> // For std::max

namespace UHE::Jobsystem
{

// A thread-local variable to store the unique index of the current thread.
// Main thread will be 0 (default initialized). Worker threads will be 1 to N.
thread_local uint32_t t_ThreadIndex = 0;

void UheJobsystem::Init()
{
    // If the system can't determine hardware concurrency, fallback to 4 threads.
    uint32_t numThreads = std::thread::hardware_concurrency();
    if (numThreads == 0)
        numThreads = 4;

    // We leave 1 core for the main thread, so we spawn numThreads - 1 workers
    uint32_t numWorkers = std::max(1u, numThreads - 1);

    m_IsRunning.store(true, std::memory_order_release);

    for (uint32_t i = 0; i < numWorkers; ++i)
    {
        // Thread index 0 is reserved for the main thread, so workers start at 1
        m_Workers.emplace_back(&UheJobsystem::WorkerThread, this, i + 1);
    }
}

void UheJobsystem::ShutDown()
{
    // The flag is flipped under the queue mutex. A worker evaluates its wait
    // predicate holding that mutex; flipping it outside could land between the
    // predicate reading "running" and the worker going to sleep, so the
    // notify_all below would be lost and join() would wait forever.
    {
        std::lock_guard<std::mutex> lock(m_QueueMutex);
        if (!m_IsRunning.load(std::memory_order_acquire))
            return;
        m_IsRunning.store(false, std::memory_order_release);
    }
    m_WakeCondition.notify_all();

    // Wait for all threads to finish their current job and exit
    for (auto& worker : m_Workers)
    {
        if (worker.joinable())
        {
            worker.join();
        }
    }
    m_Workers.clear();

    // Workers stop at their next loop check, possibly with jobs still queued.
    // Run those here: someone may be waiting on their counter, and a dropped
    // job would leave that Wait() spinning forever. Jobs they enqueue run too,
    // because Execute() on a stopped system runs inline.
    std::pair<JobFunction, JobCounter*> job;
    while (TryPopJob(job))
    {
    }
}

void UheJobsystem::Execute(JobFunction job, JobCounter* counter)
{
    if (!job)
        return;

    {
        std::unique_lock<std::mutex> lock(m_QueueMutex);

        // No workers (before Init or after ShutDown): queueing would strand the
        // job and its counter, so run it on the caller instead. Checked under
        // the mutex ShutDown flips the flag under, so a job is either queued
        // before the flip (and drained by ShutDown) or runs here.
        if (!m_IsRunning.load(std::memory_order_acquire))
        {
            lock.unlock();
            job();
            return;
        }

        // Count the job as in-flight BEFORE it becomes visible to other
        // threads, so IsBusy()/WaitForAll() can never miss it.
        m_JobsInFlight.fetch_add(1, std::memory_order_acq_rel);
        if (counter)
        {
            counter->count.fetch_add(1, std::memory_order_acq_rel);
        }
        m_JobQueue.emplace_back(std::move(job), counter);
    }

    // Wake up one sleeping worker thread to take the job
    m_WakeCondition.notify_one();
}

void UheJobsystem::ParallelFor(u32 jobCount, const std::function<void(u32)>& fn, u32 minBatchSize)
{
    if (jobCount == 0 || !fn)
        return;
    minBatchSize = std::max(1u, minBatchSize);

    // Single iteration (or single core): just run it inline, no scheduling overhead.
    const u32 workerCount = std::max(1u, static_cast<u32>(m_Workers.size()));
    if (jobCount == 1 || workerCount == 1)
    {
        for (u32 i = 0; i < jobCount; ++i)
            fn(i);
        return;
    }

    JobCounter counter;

    // Aim for ~2 batches per worker so stragglers can be picked up by free threads.
    u32 batchCount = (jobCount + minBatchSize - 1) / minBatchSize; // ceil
    batchCount = std::min(batchCount, workerCount * 2);
    batchCount = std::max(1u, batchCount);

    const u32 batchSize = (jobCount + batchCount - 1) / batchCount;

    for (u32 batch = 0; batch < batchCount; ++batch)
    {
        const u32 begin = batch * batchSize;
        if (begin >= jobCount)
            break;

        const u32 end = std::min(begin + batchSize, jobCount);

        Execute(
            [fn, begin, end]()
            {
                for (u32 i = begin; i < end; ++i)
                    fn(i);
            },
            &counter);
    }

    Wait(&counter);
}

void UheJobsystem::Wait(const JobCounter* counter)
{
    if (!counter)
        return;

    // Work-helping wait: instead of just yielding, the calling thread grabs jobs
    // from the queue and executes them itself. This way waiting time is never
    // wasted CPU time, and a counter whose jobs are all queued gets finished
    // even on a single-worker machine (no deadlock).
    while (counter->count.load(std::memory_order_acquire) > 0)
    {
        std::pair<JobFunction, JobCounter*> job;
        if (!TryPopJob(job))
        {
            // Queue is empty; the job is either running on another thread or
            // hasn't been scheduled yet. Let the OS reschedule us briefly.
            std::this_thread::yield();
        }
    }
}

void UheJobsystem::WaitForAll()
{
    // Same work-helping loop as Wait(), just against the global in-flight count.
    while (m_JobsInFlight.load(std::memory_order_acquire) > 0)
    {
        std::pair<JobFunction, JobCounter*> job;
        if (!TryPopJob(job))
        {
            std::this_thread::yield();
        }
    }
}

bool UheJobsystem::IsBusy() const
{
    return m_JobsInFlight.load(std::memory_order_acquire) > 0;
}

u32 UheJobsystem::GetThreadCount() const
{
    // Return total threads: Workers + 1 (Main Thread)
    return static_cast<u32>(m_Workers.size() + 1);
}

u32 UheJobsystem::GetCurrentThreadIndex()
{
    return t_ThreadIndex;
}

bool UheJobsystem::TryPopJob(std::pair<JobFunction, JobCounter*>& outJob)
{
    {
        std::lock_guard<std::mutex> lock(m_QueueMutex);
        if (m_JobQueue.empty())
            return false;

        outJob = std::move(m_JobQueue.front());
        m_JobQueue.pop_front(); // O(1) instead of the old O(n) vector erase
    }

    // Execute the job safely without holding the lock!
    if (outJob.first)
    {
        outJob.first();
    }

    // If a counter was attached, decrement it to signal completion.
    // Release so that a waiter that observes count == 0 also sees all
    // memory writes the job performed.
    if (outJob.second)
    {
        outJob.second->count.fetch_sub(1, std::memory_order_release);
    }

    // Mark this job as fully finished.
    m_JobsInFlight.fetch_sub(1, std::memory_order_release);

    return true;
}

void UheJobsystem::WorkerThread(u32 threadIndex)
{
    // Store the unique index in this thread's local storage
    t_ThreadIndex = threadIndex;

    while (m_IsRunning.load(std::memory_order_acquire))
    {
        std::pair<JobFunction, JobCounter*> job;
        if (!TryPopJob(job))
        {
            // Queue is empty: sleep until new work arrives or shutdown.
            std::unique_lock<std::mutex> lock(m_QueueMutex);
            m_WakeCondition.wait(lock, [this]()
                                 { return !m_JobQueue.empty() || !m_IsRunning.load(std::memory_order_acquire); });
        }
    }
}

} // namespace UHE::Jobsystem
