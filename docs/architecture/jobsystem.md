<!-- UHE docs — index: ../README.md · roadmap: ../ROADMAP.md -->

> **Status:** `DONE` — `UheJobsystem` and `TaskGraph` are implemented (`c271467`) and this doc matches
> the code, including the bugs found while building it (§7). §10 lists the upgrades that are *not* done.
> **Related issues:** [#5](https://github.com/unhuman-engine/UNHUMAN/issues/5) (still open for §10 only)
> · **Code:** `UHE/src/UHE/Jobsystem/{Jobsystem,Taskgraph}.{h,cpp}`
> **Snapshot:** matches `improve_vulkan` @ `c271467` plus the uncommitted improvements; re-verify `file:line` refs.

# UHE JobSystem — Architecture & Design Notes

> Documentation for `UHE/src/UHE/Jobsystem/` — the multithreaded execution layer of the
> engine. Written for the `improve_vulkan` branch work (JobSystem + TaskGraph rewrite).

---

## 1. Why this exists

The Vulkan renderer needs to submit command buffers, build shadow maps, upload
assets and run gameplay logic in parallel — but with *structured dependencies*
(e.g. "GBuffer pass must finish before lighting starts"). This module provides two layers:

| Layer | File | Purpose |
|---|---|---|
| **UheJobsystem** | `Jobsystem.h/.cpp` | Thread pool + fire-and-forget jobs + counters |
| **TaskGraph** | `Taskgraph.h/.cpp` | DAG of tasks with dependencies, built on top of the jobsystem |

The jobsystem never knows about the task graph — the task graph is just a client
that kicks jobs and waits. That keeps both pieces independently testable.

---

## 2. Threading model

```
Main thread (index 0) ──┐
Worker 1     (index 1) ─┤
Worker 2     (index 2) ─┼──► shared FIFO job queue (mutex + std::deque)
        ...             │         │
Worker N-1   (index N) ─┘         ▼
                        condition_variable wakes one idle worker per job
```

Key decisions:

- **`Init()` spawns `hardware_concurrency() - 1` workers** (fallback: 4). The main
  thread is *also* a worker (index 0) — it never idles while jobs exist, so we
  effectively use all cores without oversubscribing.
- **Thread indices are stable and contiguous** (`0 .. N-1`, via a `thread_local`).
  This is the contract the Vulkan side needs for per-thread resources:
  e.g. allocate 1 `VkCommandPool` per index and never lock when recording.
- **`GetThreadCount()` returns workers + 1** because the main thread counts as a
  worker when it helps.

---

## 3. UheJobsystem internals

### 3.1 The job queue

A `std::deque<std::pair<JobFunction, JobCounter*>>` guarded by one mutex.

- `deque` was chosen over `vector` because the old code erased from the front
  (**O(n) shift on every pop** — with thousands of jobs per frame that alone
  dominated the profile). `pop_front()` on a deque is O(1).
- Jobs execute **outside the lock**: `TryPopJob()` moves the pair out under the
  mutex, releases the lock, then runs the function. A long job never blocks
  producers.
- The queue is deliberately simple. The header documents the planned upgrade:
  per-thread lock-free work-stealing deques (see §8).

### 3.2 Job lifecycle & in-flight tracking

`Execute(job, counter)`:

1. Increment `m_JobsInFlight` **before** the job becomes visible in the queue.
   *Why:* if we incremented after pushing, a fast worker could finish the job
   and decrement before our increment — `IsBusy()`/`WaitForAll()` could observe
   0 while work was still queued (a classic missed-wakeup race).
2. Increment the user's `JobCounter` (if any) — also before enqueue, so a
   `Wait()` on a not-yet-kicked counter is safe.
3. Push under lock, `notify_one()`.

`TryPopJob()` on completion decrements the counter with **`memory_order_release`**,
and waiters load with **`acquire`**. This pairing guarantees: a thread that
observes `count == 0` also sees every memory write the job performed — no
fences needed at the call site.

### 3.3 Work-helping waits (the most important behavioral change)

The old `Wait()` just yielded. Now every wait loop **pops and runs queued jobs**:

```cpp
while (counter->count.load(std::memory_order_acquire) > 0)
{
    if (!TryPopJob(job))
        std::this_thread::yield();  // queue empty → running elsewhere
}
```

- Waiting time is never wasted CPU time.
- **Deadlock-free on a single worker**: if all jobs of a counter are queued
  (none running yet), the waiting thread executes them itself.
- It's *correct* to run queued jobs here because a job in the queue is by
  definition ready to run — nobody is waiting for it to become scheduled.
- `WaitForAll()` is the same loop against the global in-flight count;
  `IsBusy()` is just `inFlight > 0 (acquire)`.

### 3.4 ParallelFor

```cpp
js.ParallelFor(count, [&](u32 i){ /* write data[i] */ }, /*minBatchSize*/ 1);
```

- Range is split into `~2 batches per worker` (clamped, min batch = `minBatchSize`).
  *Why 2, not 1:* with exactly `N` batches, one slow worker (OS preemption,
  big.LITTLE slow core) stalls the tail; the extra batches let free workers
  steal the straggler's second batch.
- Trivial cases (`1` iteration or `1` worker) run inline with zero scheduling
  overhead.
- Internally kicks batch jobs with a local `JobCounter` and work-helps in `Wait`.

### 3.5 Shutdown

`ShutDown()` flips `m_IsRunning` → `notify_all` → join. Workers block on the
condition variable with the predicate `!queue.empty() || !running`, so shutdown
wakes them even with jobs still queued (queued jobs are dropped at shutdown —
call `WaitForAll()` first if you need them drained).

---

## 4. TaskGraph internals

### 4.1 Data layout

```cpp
struct TaskNode {
    JobFn   entryPoint;                      // plain function pointer
    void*   pContext;                        // user state
    std::atomic<uint32_t> dependenciesRemaining;  // runtime countdown
    uint32_t dependencyCount;                // immutable incoming-edge count
    std::vector<TaskID> dependents;          // outgoing edges
};
std::vector<TaskNode> m_nodes;               // TaskID == index into this
```

- **`TaskID` is just an array index.** Cache-friendly, trivially serializable,
  no handle validation needed. `CreateTask()` reserves capacity then
  `emplace_back()`s, so node addresses are stable while the graph is being built.
- **`JobFn` is a `void(*)(void*)`, not `std::function`.** Building a 10k-node
  graph per frame must not allocate. State goes through `pContext` (a struct
  per task, a frame arena, whatever the caller wants).
- The **immutable `dependencyCount`** exists so `Execute()` can re-arm the
  runtime counters — this is what makes a graph re-executable every frame
  (and it's also the fix for a subtle race, see §5).

### 4.2 Execution protocol

```
Execute():
  1. remaining = nodeCount; re-arm every dependenciesRemaining = dependencyCount
  2. kick every ROOT (dependencyCount == 0) as a job
  3. help-loop: while remaining > 0 → TryPopJob() else yield

RunTask(id):                     [runs on any worker or main thread]
  1. entryPoint(pContext)
  2. OnDependencyResolved(id):
       for each dependent d:
         prev = d.dependenciesRemaining.fetch_sub(1 /*acq_rel*/)
         if prev == 1 → dispatch d as a job      // "last arrival wins"
       remaining.fetch_sub(1 /*release*/)
```

- **Dispatch-on-last-arrival**: the *only* thread that observes `prev == 1`
  dispatches the task — exactly one dispatch per task, guaranteed by atomics.
- The main thread never claims not-ready tasks. It only runs *queued* jobs,
  which are ready by construction. (An earlier draft tried to let the main
  thread force-run zero-counter tasks — that was both racy and wrong for
  multi-dependency nodes; it was removed in favor of pure queue-helping.)
- `m_totalTasksRemaining` is **signed `int32_t`** on purpose: a future
  dispatch-ownership bug drives it negative and the wait exits (tests fail
  loudly, plus a debug assert) instead of wrapping to ~4 billion and hanging
  forever.

### 4.3 Shape example (diamond)

```
        a
       / \
      b   c        b and c run in parallel;
       \ /         d waits for BOTH (two incoming edges)
        d
```

---

## 5. The dispatch-ownership invariant (most subtle rule)

> **Rule: the root scan in `Execute()` must test `dependencyCount == 0` (immutable),
> never `dependenciesRemaining == 0` (mutable).**

Why: workers start resolving dependents the moment early roots finish — *while
the root scan may still be iterating*. A non-root task can therefore reach a
runtime counter of 0 mid-scan. If the scan tested the mutable counter it would
kick that task **and** the resolver would dispatch it → the task **runs twice**
→ `remaining` underflows → `Execute` never returns.

This exact race was caught with an instrumented build:

```
TG [t2] DISPATCH 9     ← worker resolves 9's last dependency, dispatches it
TG [t0] KICK root 9    ← main's scan sees counter==0 and kicks it again  ✗
TG [t6] RUN 9
TG [t7] RUN 9          ← ran twice → remaining wrapped → deadlock
```

The invariant makes dispatch ownership **exclusive by construction**:

- a root (`dependencyCount == 0`) can never appear in anyone's `dependents`
  list, so only the scan can kick it;
- a non-root can only be dispatched by the single `prev == 1` winner;
- the two sets are disjoint.

---

## 6. Memory-ordering cheat sheet

| Operation | Ordering | Reason |
|---|---|---|
| `m_JobsInFlight.fetch_add` on kick | `acq_rel` | happens *before* queue push; keeps `IsBusy/WaitForAll` miss-free |
| `m_JobsInFlight.fetch_sub` on finish | `release` | pairs with acquire loads in waiters |
| `JobCounter` sub on finish | `release` | waiter that sees 0 sees all job writes |
| `Wait/WaitForAll/Execute` help-loop loads | `acquire` | pair with the releases above |
| dependency `fetch_sub` on resolve | `acq_rel` | acquire = all deps' writes visible to dispatched task; release = this task's writes visible downstream |
| re-arm + root-scan reads | `relaxed` | single-threaded region of `Execute()` before first kick |

---

## 7. Bugs found & fixed during development (war stories)

1. **`CreateTask` wrote one-past-the-end.** The old code did
   `reinterpret_cast<TaskNode*>(m_nodes.data() + id)` and wrote through it —
   heap corruption, and the node was never actually appended. Now:
   `reserve` + `emplace_back`.
2. **Graph reuse restored the wrong counter.** Re-arming used
   `dependents.size()` (outgoing edges) instead of the incoming count — after
   the first run, root tasks looked non-ready and nothing dispatched.
   Fix: immutable `dependencyCount` per node.
3. **Double-dispatch race** — see §5. Found via job-UID + thread-index tracing
   after intermittent stress hangs; ptrace was blocked in the dev sandbox, so
   the code itself was instrumented (copies in `/tmp/uhe_dbg/`, engine files
   kept pristine).

---

## 8. Verification methodology

Because the module is engine-independent (the header carries its own `u32`
alias and no engine includes), it's tested with a **standalone binary**
compiled directly against the two `.cpp` files, `g++ -std=c++20 -O2 -Wall
-Wextra -pthread`:

| Test | What it proves |
|---|---|
| 10 000 jobs → counter | counter correctness, no lost jobs |
| `ParallelFor` 200k elements | every index written exactly once (both default & `minBatchSize` paths) |
| blocking jobs + `IsBusy`/`WaitForAll` | in-flight tracking has no miss race |
| chain `a→b→c`, checked by global sequence | dependency ordering |
| diamond, `d` after **both** `b` and `c` | multi-dependency readiness |
| 500-task random DAG (seeded, edges only `i→j, i<j` ⇒ acyclic) × 5 re-executes | stress + graph reuse + the §5 race |

Result: 18 consecutive clean runs (8 + 10 soak) after the final fix; the race
was reproducible within a handful of runs before it.

---

## 9. Usage

```cpp
UHE::Jobsystem::UheJobsystem js;
js.Init();                        // once at startup

// Fire-and-forget
js.Execute([](){ DoThing(); });

// Group with a counter
UHE::Jobsystem::JobCounter c;
for (auto& tex : textures)
    js.Execute([&tex](){ Upload(tex); }, &c);
js.Wait(&c);                      // helps run queued uploads while waiting

// Data-parallel
js.ParallelFor(pixels.size(), [&](u32 i){ Brightness(pixels[i]); });

// Structured frame graph
UHE::Jobsystem::TaskGraph g;
TaskID begin  = g.CreateTask(&BeginFrame,  frameCtx);
TaskID gbuffer= g.CreateTask(&GBuffer,     frameCtx);
TaskID light  = g.CreateTask(&Lighting,    frameCtx);
g.AddDependency(begin, gbuffer);
g.AddDependency(gbuffer, light);
g.Execute(js);                    // once per frame — reuse the same graph!
```

Per-thread Vulkan resources:

```cpp
u32 idx = UheJobsystem::GetCurrentThreadIndex();          // 0..N-1
VkCommandBuffer cb = perThreadPools[idx].Get();           // no locks needed
```

---

## 10. Planned upgrades (from the code comments, in priority order)

1. **Per-thread job queues + lock-free work-stealing deques** (Chase–Lev style)
   — remove the single mutex from the hot path.
2. **Job continuations** — jobs returning child jobs so deep recursion
   (e.g. task-graph subgraphs) doesn't grow the OS stack.
3. ~~**Fiber-backed jobs** for await-style suspension of subsystem updates~~ —
   **deferred / rejected for now**: fibers require per-platform asm context
   switches (e.g. `boost::context` `PCONTEXT`), which is fragile across Android
   ABIs and toolchains and hard to debug. Portability is a hard constraint
   (`IDEA.md`), so prefer **C++20 coroutines** for continuations if plain task
   chaining is not enough.
4. Wire `TaskGraph` into the render-graph so passes, barriers and queue
   submissions derive from the same DAG.
5. SIMD-friendly `ParallelFor` scheduling hints (cache-line-aligned batch
   boundaries).

---

*Files: `Jobsystem.h/.cpp` (thread pool), `Taskgraph.h/.cpp` (DAG executor).
This doc matches the implementation as of the `improve_vulkan` branch,
commit series "Core: Implement JobSystem and TaskGraph" + the uncommitted
improvements.*
