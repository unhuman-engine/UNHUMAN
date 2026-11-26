#include "Jobsystem.h"
#include <algorithm>

namespace UHE::Jobsystem
{
namespace
{
// The worker index of this thread: 0 for the main thread, 1..N for workers.
thread_local uint32_t t_ThreadIndex = 0;

// The jobsystem whose deque this thread owns, and its slot. A thread owns at
// most one deque, of one jobsystem: a worker of one system kicking a job on
// another goes through that system's injection queue.
thread_local const UheJobsystem* t_Owner = nullptr;
thread_local uint32_t t_OwnedSlot = 0;

// Where a thief starts looking: spreads thieves over the victims.
thread_local uint32_t t_StealSeed = 0x9E3779B9u;

uint32_t NextRandom()
{
    uint32_t x = t_StealSeed;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    t_StealSeed = x;
    return x;
}
} // namespace

UheJobsystem::~UheJobsystem()
{
    ShutDown();
}

void UheJobsystem::Init()
{
    if (m_IsRunning.load(std::memory_order_acquire))
        return;

    // If the system can't determine hardware concurrency, fallback to 4 threads.
    uint32_t numThreads = std::thread::hardware_concurrency();
    if (numThreads == 0)
        numThreads = 4;

    // We leave 1 core for the main thread, so we spawn numThreads - 1 workers
    const uint32_t numWorkers = std::max(1u, numThreads - 1);

    m_Queues.clear();
    for (uint32_t i = 0; i <= numWorkers; ++i)
        m_Queues.push_back(std::make_unique<WorkStealingDeque<Job>>());

    // The calling thread owns deque 0 and helps from it while it waits.
    t_Owner = this;
    t_OwnedSlot = 0;

    m_IsRunning.store(true, std::memory_order_seq_cst);
    for (uint32_t i = 0; i < numWorkers; ++i)
    {
        // Thread index 0 is reserved for the main thread, so workers start at 1
        m_Workers.emplace_back(&UheJobsystem::WorkerThread, this, i + 1);
    }
}

void UheJobsystem::ShutDown()
{
    if (!m_IsRunning.exchange(false, std::memory_order_seq_cst))
        return;
    {
        // Under the sleep mutex: a worker checks its wait predicate holding it,
        // so the flag cannot change between that check and its sleep.
        std::lock_guard<std::mutex> lock(m_SleepMutex);
    }
    m_WakeCondition.notify_all();
    for (auto& worker : m_Workers)
    {
        if (worker.joinable())
            worker.join();
    }
    m_Workers.clear();

    // Jobs still queued run here: someone may be waiting on their counter. A
    // job they kick runs inline (the system is stopped), and an Execute that
    // saw the system running before the flag flipped is waited for.
    while (m_Submitting.load(std::memory_order_seq_cst) > 0 || m_JobsInFlight.load(std::memory_order_acquire) > 0)
    {
        if (!RunOneJob())
            std::this_thread::yield();
    }
    if (t_Owner == this)
        t_Owner = nullptr;
    m_Queues.clear();
}

WorkStealingDeque<UheJobsystem::Job>* UheJobsystem::OwnQueue() const
{
    return t_Owner == this && t_OwnedSlot < m_Queues.size() ? m_Queues[t_OwnedSlot].get() : nullptr;
}

void UheJobsystem::Execute(JobFunction job, JobCounter* counter)
{
    if (!job)
        return;

    // Announced before the running check, so ShutDown waits for this push
    // instead of finishing its drain just before the job lands in a queue.
    m_Submitting.fetch_add(1, std::memory_order_seq_cst);
    if (!m_IsRunning.load(std::memory_order_seq_cst))
    {
        m_Submitting.fetch_sub(1, std::memory_order_seq_cst);
        job(); // no workers: run it here rather than strand it
        return;
    }

    // Counted BEFORE it becomes visible to other threads, so IsBusy(),
    // WaitForAll() and a counter's Wait() can never miss it.
    m_JobsInFlight.fetch_add(1, std::memory_order_acq_rel);
    if (counter != nullptr)
        counter->count.fetch_add(1, std::memory_order_acq_rel);
    Push(new Job{std::move(job), counter});
    m_Submitting.fetch_sub(1, std::memory_order_seq_cst);
}

void UheJobsystem::Push(Job* job)
{
    if (WorkStealingDeque<Job>* own = OwnQueue())
    {
        own->Push(job);
    }
    else
    {
        std::lock_guard<std::mutex> lock(m_InjectMutex);
        m_Injected.push_back(job);
        m_InjectedCount.fetch_add(1, std::memory_order_release);
    }
    m_Queued.fetch_add(1, std::memory_order_seq_cst);
    WakeOne();
}

void UheJobsystem::WakeOne()
{
    // A worker going to sleep raises m_Sleepers and then re-checks m_Queued;
    // this raised m_Queued and then reads m_Sleepers. Both seq_cst, so one of
    // the two sees the other: no lost wake-up, and no mutex when all are busy.
    if (m_Sleepers.load(std::memory_order_seq_cst) == 0)
        return;
    {
        std::lock_guard<std::mutex> lock(m_SleepMutex);
    }
    m_WakeCondition.notify_one();
}

UheJobsystem::Job* UheJobsystem::FindJob()
{
    Job* job = nullptr;
    if (WorkStealingDeque<Job>* own = OwnQueue())
        job = own->Pop();
    if (job == nullptr && m_InjectedCount.load(std::memory_order_acquire) > 0)
    {
        std::lock_guard<std::mutex> lock(m_InjectMutex);
        if (!m_Injected.empty())
        {
            job = m_Injected.front();
            m_Injected.pop_front();
            m_InjectedCount.fetch_sub(1, std::memory_order_release);
        }
    }
    if (job == nullptr && !m_Queues.empty())
    {
        const auto count = static_cast<uint32_t>(m_Queues.size());
        const uint32_t start = NextRandom() % count;
        for (uint32_t i = 0; i < count && job == nullptr; ++i)
        {
            const uint32_t victim = (start + i) % count;
            if (t_Owner == this && victim == t_OwnedSlot)
                continue;
            job = m_Queues[victim]->Steal();
        }
    }
    if (job != nullptr)
        m_Queued.fetch_sub(1, std::memory_order_seq_cst);
    return job;
}

void UheJobsystem::Run(Job* job)
{
    if (job->Function)
        job->Function();
    JobCounter* counter = job->Counter;
    delete job;
    if (counter != nullptr)
        FinishOne(counter);
    // Last: observing zero gives a waiter the right to tear everything down.
    m_JobsInFlight.fetch_sub(1, std::memory_order_release);
}

void UheJobsystem::FinishOne(JobCounter* counter)
{
    // `finishing` covers the window after the count reaches zero in which this
    // thread still uses the counter (its continuations); Wait() waits it out,
    // so the counter's owner cannot destroy it under us.
    counter->finishing.fetch_add(1, std::memory_order_acq_rel);
    std::vector<JobFunction> ready;
    if (counter->count.fetch_sub(1, std::memory_order_acq_rel) == 1)
    {
        std::lock_guard<std::mutex> lock(counter->continuationMutex);
        ready.swap(counter->continuations);
    }
    // The last access to the counter. The continuations start only after it:
    // one may free the counter at once (a coroutine whose frame holds it).
    counter->finishing.fetch_sub(1, std::memory_order_acq_rel);
    for (JobFunction& continuation : ready)
        Execute(std::move(continuation));
}

void UheJobsystem::Then(JobCounter& counter, JobFunction job, JobCounter* signal)
{
    if (!job)
        return;
    if (signal != nullptr)
    {
        // Counted now, so a Wait on the signal cannot pass before the continuation runs.
        signal->count.fetch_add(1, std::memory_order_acq_rel);
        job = [this, inner = std::move(job), signal]
        {
            inner();
            FinishOne(signal);
        };
    }
    {
        std::lock_guard<std::mutex> lock(counter.continuationMutex);
        // A completer drains the list after the count reaches zero, under this
        // mutex: seen above zero here, the job is on the list in time.
        if (counter.count.load(std::memory_order_acquire) > 0)
        {
            counter.continuations.push_back(std::move(job));
            return;
        }
    }
    // Already zero, but the completer that took it there may still be inside
    // the counter (a few instructions). The job may free the counter, so it
    // starts once nobody is.
    while (counter.finishing.load(std::memory_order_acquire) > 0)
        std::this_thread::yield();
    Execute(std::move(job));
}

void UheJobsystem::ParallelFor(u32 jobCount, const std::function<void(u32)>& fn, u32 minBatchSize)
{
    if (jobCount == 0 || !fn)
        return;
    ParallelForRange(
        jobCount,
        [&fn](u32 begin, u32 end)
        {
            for (u32 i = begin; i < end; ++i)
                fn(i);
        },
        minBatchSize, 1);
}

void UheJobsystem::ParallelForRange(u32 count, const std::function<void(u32, u32)>& fn, u32 minBatchSize, u32 alignment)
{
    if (count == 0 || !fn)
        return;
    alignment = std::max(1u, alignment);
    minBatchSize = std::max(1u, minBatchSize);

    // One batch (or one core): run it inline, no scheduling overhead.
    const u32 workerCount = std::max(1u, static_cast<u32>(m_Workers.size()));
    if (count <= minBatchSize || workerCount == 1 || !m_IsRunning.load(std::memory_order_acquire))
    {
        fn(0, count);
        return;
    }

    // Aim for ~2 batches per worker so stragglers can be picked up by free
    // threads; round the size up to whole vectors.
    u32 batchCount = std::min((count + minBatchSize - 1) / minBatchSize, workerCount * 2);
    batchCount = std::max(1u, batchCount);
    u32 batchSize = (count + batchCount - 1) / batchCount;
    batchSize = (batchSize + alignment - 1) / alignment * alignment;

    JobCounter counter;
    for (u32 begin = 0; begin < count; begin += batchSize)
    {
        const u32 end = std::min(begin + batchSize, count);
        Execute([&fn, begin, end]() { fn(begin, end); }, &counter);
    }
    Wait(&counter);
}

void UheJobsystem::Wait(JobCounter* counter)
{
    if (counter == nullptr)
        return;

    // Work-helping wait: instead of just yielding, the calling thread runs
    // ready jobs itself, so waiting is never wasted CPU time and a counter
    // whose jobs are all queued finishes even on a single-worker machine.
    while (counter->count.load(std::memory_order_acquire) > 0 || counter->finishing.load(std::memory_order_acquire) > 0)
    {
        if (!RunOneJob())
            std::this_thread::yield();
    }
}

void UheJobsystem::WaitForAll()
{
    // Same work-helping loop as Wait(), just against the global in-flight count.
    while (m_JobsInFlight.load(std::memory_order_acquire) > 0)
    {
        if (!RunOneJob())
            std::this_thread::yield();
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

bool UheJobsystem::RunOneJob()
{
    Job* job = FindJob();
    if (job == nullptr)
        return false;
    Run(job);
    return true;
}

bool UheJobsystem::TryPopJob(std::pair<JobFunction, JobCounter*>& outJob)
{
    (void)outJob;
    return RunOneJob();
}

void UheJobsystem::WorkerThread(u32 threadIndex)
{
    t_ThreadIndex = threadIndex;
    t_Owner = this;
    t_OwnedSlot = threadIndex;
    t_StealSeed = 0x9E3779B9u * (threadIndex + 1);

    while (true)
    {
        if (RunOneJob())
            continue;
        if (!m_IsRunning.load(std::memory_order_acquire))
            break;
        // Nothing to run: sleep until a push or shutdown wakes this worker.
        std::unique_lock<std::mutex> lock(m_SleepMutex);
        m_Sleepers.fetch_add(1, std::memory_order_seq_cst);
        m_WakeCondition.wait(
            lock, [this]
            { return m_Queued.load(std::memory_order_seq_cst) > 0 || !m_IsRunning.load(std::memory_order_seq_cst); });
        m_Sleepers.fetch_sub(1, std::memory_order_seq_cst);
    }
    t_Owner = nullptr;
}

} // namespace UHE::Jobsystem
