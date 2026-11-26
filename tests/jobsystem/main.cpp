// Job system + TaskGraph harness.
//
// What it checks: ParallelFor visits every index exactly once; a TaskGraph
// runs each task once, after its dependencies, and can be executed again;
// jobs still queued at ShutDown() complete (a Wait() on their counter would
// otherwise spin forever); and Init/ShutDown in a tight loop never hangs,
// which is the lost wake-up a ShutDown that stores its flag outside the
// queue mutex allowed.

#include <algorithm>
#include <atomic>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "UHE/Jobsystem/Jobsystem.h"
#include "UHE/Jobsystem/Taskgraph.h"
#include "UHE/Jobsystem/WorkStealingDeque.h"

using namespace UHE::Jobsystem;

namespace
{
int g_failures = 0;

void check(bool condition, const std::string& what)
{
    std::cout << (condition ? "  ok   - " : "  FAIL - ") << what << '\n';
    g_failures += condition ? 0 : 1;
}

struct GraphRun
{
    std::atomic<int> order{0};
    int finishedAt[4] = {-1, -1, -1, -1};
    std::atomic<int> runs[4] = {};
};

struct TaskContext
{
    GraphRun* run;
    int index;
};

void RecordTask(void* p)
{
    auto* context = static_cast<TaskContext*>(p);
    context->run->runs[context->index].fetch_add(1);
    context->run->finishedAt[context->index] = context->run->order.fetch_add(1);
}
} // namespace

int main()
{
    std::cout << "[ parallel for ]\n";
    {
        UheJobsystem jobs;
        jobs.Init();
        constexpr u32 count = 10000;
        std::vector<std::atomic<int>> visits(count);
        jobs.ParallelFor(count, [&](u32 i) { visits[i].fetch_add(1); }, 16);
        bool once = true;
        for (const auto& v : visits)
            once = once && v.load() == 1;
        check(once, "every index is visited exactly once");
        check(!jobs.IsBusy(), "nothing is left in flight");
        jobs.ShutDown();
    }

    std::cout << "[ task graph ]\n";
    {
        UheJobsystem jobs;
        jobs.Init();
        // Diamond: 0 -> {1, 2} -> 3
        GraphRun run;
        TaskContext contexts[4] = {{&run, 0}, {&run, 1}, {&run, 2}, {&run, 3}};
        TaskGraph graph;
        TaskID ids[4];
        for (int i = 0; i < 4; ++i)
            ids[i] = graph.CreateTask(&RecordTask, &contexts[i]);
        graph.AddDependency(ids[0], ids[1]);
        graph.AddDependency(ids[0], ids[2]);
        graph.AddDependency(ids[1], ids[3]);
        graph.AddDependency(ids[2], ids[3]);

        for (int frame = 0; frame < 200; ++frame)
        {
            run.order = 0;
            graph.Execute(jobs);
        }
        bool once = true;
        for (const auto& r : run.runs)
            once = once && r.load() == 200;
        check(once, "each task runs once per Execute, across 200 re-executions");
        check(run.finishedAt[0] == 0 && run.finishedAt[3] == 3, "the root runs first and the join last");
        jobs.ShutDown();
    }

    std::cout << "[ shutdown ]\n";
    {
        // Jobs queued behind a slow one when ShutDown runs must still finish:
        // their counter is what a caller's Wait() is watching.
        UheJobsystem jobs;
        jobs.Init();
        JobCounter counter;
        std::atomic<int> done{0};
        for (int i = 0; i < 64; ++i)
        {
            jobs.Execute([&done] { done.fetch_add(1); }, &counter);
        }
        jobs.ShutDown();
        check(done.load() == 64, "jobs queued at shutdown still run");
        check(counter.count.load() == 0, "and their counter reaches zero");

        // The lost wake-up was a hang, so the evidence is reaching the end;
        // CTest's timeout turns a regression into a failure.
        for (int i = 0; i < 2000; ++i)
        {
            UheJobsystem cycled;
            cycled.Init();
            cycled.ShutDown();
        }
        check(true, "2000 Init/ShutDown cycles never hang");
    }

    std::cout << "[ work-stealing deque ]\n";
    {
        // Owner order: LIFO at the bottom; thieves: FIFO at the top; growth keeps every item.
        WorkStealingDeque<int> deque(4);
        std::vector<int> items(100);
        for (int i = 0; i < 100; ++i)
        {
            items[i] = i;
            deque.Push(&items[i]);
        }
        check(deque.Steal() == &items[0], "a thief takes the oldest item");
        check(deque.Pop() == &items[99], "the owner takes the newest");
        int left = 0;
        while (deque.Pop() != nullptr)
            ++left;
        check(left == 98 && deque.Pop() == nullptr && deque.Steal() == nullptr,
              "growing from 4 slots kept all 100 items, and an empty deque returns nothing");

        // Concurrent: the owner pushes and pops while thieves steal; every
        // item is taken exactly once. Under TSan this is the deque's race test.
        constexpr int kItems = 200000;
        std::vector<int> values(kItems);
        std::vector<std::atomic<int>> taken(kItems);
        WorkStealingDeque<int> shared(64);
        std::atomic<bool> done{false};
        const auto take = [&](int* item)
        {
            if (item != nullptr)
                taken[static_cast<std::size_t>(*item)].fetch_add(1, std::memory_order_relaxed);
        };
        std::vector<std::thread> thieves;
        for (int t = 0; t < 3; ++t)
        {
            thieves.emplace_back(
                [&]
                {
                    while (!done.load(std::memory_order_acquire))
                        take(shared.Steal());
                    while (int* item = shared.Steal())
                        take(item);
                });
        }
        for (int i = 0; i < kItems; ++i)
        {
            values[i] = i;
            shared.Push(&values[i]);
            if (i % 3 == 0)
                take(shared.Pop());
        }
        while (int* item = shared.Pop())
            take(item);
        done.store(true, std::memory_order_release);
        for (auto& thief : thieves)
            thief.join();
        bool once = true;
        for (const auto& t : taken)
            once = once && t.load() == 1;
        check(once, "owner and three thieves take each of 200000 items exactly once");
    }

    std::cout << "[ continuations ]\n";
    {
        UheJobsystem jobs;
        jobs.Init();
        // Children, then a continuation that sees all of their work, without anyone blocking.
        for (int round = 0; round < 200; ++round)
        {
            JobCounter children;
            JobCounter finished;
            std::atomic<int> sum{0};
            std::atomic<int> seen{-1};
            for (int i = 1; i <= 20; ++i)
                jobs.Execute([&sum, i] { sum.fetch_add(i); }, &children);
            jobs.Then(children, [&] { seen.store(sum.load()); }, &finished);
            jobs.Wait(&finished);
            if (seen.load() != 210)
            {
                check(false, "a continuation saw " + std::to_string(seen.load()) + " instead of 210");
                break;
            }
            jobs.Wait(&children); // the counter is destroyed next: Wait covers the completer still in it
        }
        check(true, "200 rounds: each continuation ran after all 20 children and saw their work");

        JobCounter idle;
        std::atomic<bool> ran{false};
        JobCounter signal;
        jobs.Then(idle, [&] { ran.store(true); }, &signal);
        jobs.Wait(&signal);
        check(ran.load(), "a continuation on a counter already at zero runs at once");

        // A chain: each stage is the continuation of the one before, no thread waits in between.
        JobCounter stage1, stage2, stage3;
        std::vector<int> order;
        std::mutex orderMutex;
        const auto record = [&](int n)
        {
            std::lock_guard<std::mutex> lock(orderMutex);
            order.push_back(n);
        };
        jobs.Execute([&] { record(1); }, &stage1);
        jobs.Then(stage1, [&] { record(2); }, &stage2);
        jobs.Then(stage2, [&] { record(3); }, &stage3);
        jobs.Wait(&stage3);
        check(order == std::vector<int>({1, 2, 3}), "a chain of continuations runs in order");
        jobs.ShutDown();
    }

    std::cout << "[ coroutines ]\n";
    {
        UheJobsystem jobs;
        jobs.Init();
        std::atomic<int> total{0};
        std::atomic<bool> callerHadControl{false};
        std::atomic<bool> sawCallerReturn{false};
        // A job as a coroutine: it yields to its caller at Schedule (resuming
        // as a job), fans out, then suspends until the fan-out is done while
        // holding no thread, and reads the result.
        const auto task = [&]() -> JobTask
        {
            co_await jobs.Schedule();
            // The caller sets the flag after task() returned; the resumed body
            // can only observe it later, never before Schedule handed control back.
            while (!callerHadControl.load())
                std::this_thread::yield();
            sawCallerReturn.store(true);
            JobCounter parts;
            for (int i = 0; i < 100; ++i)
                jobs.Execute([&total] { total.fetch_add(1); }, &parts);
            co_await jobs.WhenDone(parts);
            // `parts` lives in this frame and dies when the coroutine ends,
            // possibly at once on another thread: the completer that resumed
            // this must already be out of it (ASan and TSan check that here).
            total.fetch_add(1000);
        };
        task(); // returns at the first co_await
        callerHadControl.store(true);
        while (total.load() != 1100)
        {
            if (!jobs.RunOneJob())
                std::this_thread::yield();
        }
        jobs.WaitForAll();
        check(sawCallerReturn.load(), "co_await Schedule hands control back to the caller and resumes as a job");
        check(total.load() == 1100,
              "co_await WhenDone resumed after all 100 parts (total " + std::to_string(total.load()) + ")");
        jobs.ShutDown();
    }

    std::cout << "[ ranges and foreign threads ]\n";
    {
        UheJobsystem jobs;
        jobs.Init();
        constexpr u32 count = 1003;
        std::vector<std::atomic<int>> visits(count);
        std::atomic<bool> aligned{true};
        jobs.ParallelForRange(
            count,
            [&](u32 begin, u32 end)
            {
                if (begin % 8 != 0 || (end != count && end % 8 != 0))
                    aligned.store(false);
                for (u32 i = begin; i < end; ++i)
                    visits[i].fetch_add(1);
            },
            16, 8);
        bool once = true;
        for (const auto& v : visits)
            once = once && v.load() == 1;
        check(once && aligned.load(), "ParallelForRange covers every index once, batches on 8-element boundaries");

        // Threads that own no deque submit through the injection queue.
        std::atomic<int> fromOutside{0};
        JobCounter outside;
        std::vector<std::thread> submitters;
        for (int t = 0; t < 4; ++t)
            submitters.emplace_back(
                [&]
                {
                    for (int i = 0; i < 500; ++i)
                        jobs.Execute([&fromOutside] { fromOutside.fetch_add(1); }, &outside);
                });
        for (auto& submitter : submitters)
            submitter.join();
        jobs.Wait(&outside);
        check(fromOutside.load() == 2000, "2000 jobs from four foreign threads all ran");
        check(!jobs.IsBusy(), "nothing is left in flight");
        jobs.ShutDown();
    }

    std::cout << (g_failures == 0 ? "\nALL CHECKS PASSED\n" : "\nFAILURES PRESENT\n");
    return g_failures == 0 ? 0 : 1;
}
