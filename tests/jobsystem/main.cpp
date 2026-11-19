// Job system + TaskGraph harness.
//
// What it checks: ParallelFor visits every index exactly once; a TaskGraph
// runs each task once, after its dependencies, and can be executed again;
// jobs still queued at ShutDown() complete (a Wait() on their counter would
// otherwise spin forever); and Init/ShutDown in a tight loop never hangs,
// which is the lost wake-up a ShutDown that stores its flag outside the
// queue mutex allowed.

#include "UHE/Jobsystem/Jobsystem.h"
#include "UHE/Jobsystem/Taskgraph.h"

#include <atomic>
#include <iostream>
#include <string>
#include <vector>

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

    std::cout << (g_failures == 0 ? "\nALL CHECKS PASSED\n" : "\nFAILURES PRESENT\n");
    return g_failures == 0 ? 0 : 1;
}
