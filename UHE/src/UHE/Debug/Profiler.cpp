#include "UHE/Debug/Profiler.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace UHE
{
    namespace
    {
        constexpr u32 kSamplesPerThread = 1024; // per frame; overflow is reported, not hidden
        constexpr u32 kAveragingWindow = 120;   // frames used for the average column
        constexpr u32 kFrameHistory = 300;      // frames kept for the history graph
        constexpr u32 kNoThread = 0xFFFFFFFFu;

        // Per-thread sample buffer. The main thread is the only writer *and*
        // reader of its own buffer (frame boundaries are main-thread), so it
        // needs no synchronization; other threads are drained under `Mutex`.
        struct ThreadBuffer
        {
            ProfileSample Samples[kSamplesPerThread];
            u32 Count = 0;
            u32 Depth = 0;
            ProfileThreadId Id = 0;
            bool IsMainThread = false;
            std::mutex Mutex;
            char Name[32] = {};
        };

        std::atomic<bool> s_Enabled{true};
        std::atomic<bool> s_Capturing{true};
        std::atomic<u32> s_NextThreadId{0};
        std::atomic<u32> s_DroppedSamples{0};

        std::mutex s_BufferMutex;
        std::vector<ThreadBuffer*> s_Buffers;

        thread_local ThreadBuffer* t_Buffer = nullptr;

        // ─── aggregation state (main thread only) ───────────────────────────
        struct ScopeKey
        {
            const char* Name;
            const char* Module;

            bool operator==(const ScopeKey& other) const
            {
                return Name == other.Name && Module == other.Module;
            }
        };

        struct ScopeKeyHash
        {
            std::size_t operator()(const ScopeKey& key) const
            {
                const std::size_t h1 = std::hash<const void*>()(key.Name);
                const std::size_t h2 = std::hash<const void*>()(key.Module);
                return h1 ^ (h2 + 0x9E3779B97F4A7C15ull + (h1 << 6) + (h1 >> 2));
            }
        };

        // One captured frame's totals for a single scope name.
        struct FrameEntry
        {
            ProfileScopeStats Stats;
            ScopeKey Key{nullptr, nullptr};
            u64 ThreadMask = 0;
        };

        struct ScopeAccumulator
        {
            f32 Window[kAveragingWindow] = {};
            u32 WindowCount = 0;
            u32 WindowNext = 0;
            f32 MinSinceReset = 0.0f;
            f32 MaxSinceReset = 0.0f;
        };

        std::unordered_map<ScopeKey, ScopeAccumulator, ScopeKeyHash> s_Accumulators;
        std::vector<ProfileScopeStats> s_ScopeStats;
        std::vector<ProfileNode> s_FrameTree;
        std::vector<ScopeKey> s_TreeNodeKeys; // literals, parallel to s_FrameTree
        std::vector<ProfileThreadStats> s_ThreadStats;
        std::vector<ProfileFrameRecord> s_FrameHistory;

        // Scratch, kept alive so steady-state frames do not allocate.
        std::vector<ProfileSample> s_MergedSamples;
        std::vector<ThreadBuffer*> s_DrainList;
        std::vector<FrameEntry> s_FrameEntries;
        std::unordered_map<ProfileThreadId, u32> s_ThreadSlots;
        std::unordered_map<ScopeKey, u32, ScopeKeyHash> s_EntrySlots;

        f64 s_FrameStartMs = 0.0;
        bool s_HaveFrameStart = false;
        f32 s_LastFrameMs = 0.0f;
        f32 s_AverageFrameMs = 0.0f;
        f32 s_MinFrameMs = 0.0f;
        f32 s_MaxFrameMs = 0.0f;
        f32 s_ScopedCpuMs = 0.0f;
        u32 s_FrameSampleCount = 0;
        u32 s_FrameDropped = 0;
        u64 s_CapturedFrames = 0;
        u32 s_MainThreadId = kNoThread;

        const char* Basename(const char* path)
        {
            if (path == nullptr)
                return "";
            const char* slash = std::strrchr(path, '/');
            const char* backslash = std::strrchr(path, '\\');
            if (backslash != nullptr && (slash == nullptr || backslash > slash))
                slash = backslash;
            return slash != nullptr ? slash + 1 : path;
        }

        ThreadBuffer& CurrentBuffer()
        {
            if (t_Buffer == nullptr)
            {
                ThreadBuffer* buffer = new ThreadBuffer();
                buffer->Id = s_NextThreadId.fetch_add(1, std::memory_order_relaxed);
                buffer->IsMainThread = (buffer->Id == s_MainThreadId);
                std::snprintf(buffer->Name, sizeof(buffer->Name), "Thread %u", buffer->Id);
                {
                    std::lock_guard<std::mutex> lock(s_BufferMutex);
                    s_Buffers.push_back(buffer);
                }
                t_Buffer = buffer;
            }
            return *t_Buffer;
        }

        void AppendSample(ThreadBuffer& buffer, const char* name, const char* module, f64 startMs)
        {
            if (buffer.Count < kSamplesPerThread)
            {
                ProfileSample& sample = buffer.Samples[buffer.Count++];
                sample.Name = name;
                sample.Module = module;
                sample.StartMs = startMs;
                sample.EndMs = Profiler::NowMs();
                sample.Depth = static_cast<u16>(buffer.Depth);
                sample.Thread = buffer.Id;
            }
            else
            {
                s_DroppedSamples.fetch_add(1, std::memory_order_relaxed);
            }
        }

        // Empties the thread buffers without aggregating them: used when capture
        // is off so a stale sample can never leak into the next captured frame.
        void DiscardPendingSamples(const std::vector<ThreadBuffer*>& buffers)
        {
            for (ThreadBuffer* buffer : buffers)
            {
                if (buffer->IsMainThread)
                {
                    buffer->Count = 0;
                    continue;
                }
                std::lock_guard<std::mutex> lock(buffer->Mutex);
                buffer->Count = 0;
            }
        }
    } // namespace

    // ─── clock ──────────────────────────────────────────────────────────────
    f64 Profiler::NowMs()
    {
        using Clock = std::chrono::steady_clock;
        static const Clock::time_point origin = Clock::now();
        return std::chrono::duration<f64, std::milli>(Clock::now() - origin).count();
    }

    // ─── configuration ──────────────────────────────────────────────────────
    void Profiler::SetEnabled(bool enabled) { s_Enabled.store(enabled, std::memory_order_relaxed); }
    bool Profiler::IsEnabled() { return s_Enabled.load(std::memory_order_relaxed); }

    void Profiler::SetCapturing(bool capturing) { s_Capturing.store(capturing, std::memory_order_relaxed); }
    bool Profiler::IsCapturing() { return s_Capturing.load(std::memory_order_relaxed); }

    ProfileThreadId Profiler::GetMainThreadId()
    {
        return s_MainThreadId == kNoThread ? 0u : s_MainThreadId;
    }

    void Profiler::SetThreadName(const char* name)
    {
        if (name == nullptr)
            return;
        ThreadBuffer& buffer = CurrentBuffer();
        std::snprintf(buffer.Name, sizeof(buffer.Name), "%s", name);
    }

    void Profiler::ResetHistory()
    {
        s_Accumulators.clear();
        s_FrameHistory.clear();
        s_MinFrameMs = 0.0f;
        s_MaxFrameMs = 0.0f;
        s_AverageFrameMs = 0.0f;
        s_CapturedFrames = 0;
    }

    // ─── recording ──────────────────────────────────────────────────────────
    f64 Profiler::BeginScope()
    {
        if (!s_Enabled.load(std::memory_order_relaxed) || !s_Capturing.load(std::memory_order_relaxed))
            return -1.0;

        ThreadBuffer& buffer = CurrentBuffer();
        ++buffer.Depth;
        return NowMs();
    }

    void Profiler::EndScope(const char* name, const char* module, f64 startMs)
    {
        ThreadBuffer& buffer = CurrentBuffer();
        if (buffer.Depth > 0)
            --buffer.Depth;

        if (buffer.IsMainThread)
        {
            AppendSample(buffer, name, module, startMs);
            return;
        }

        // Non-main threads are drained from the frame-boundary thread, so a job
        // thread's buffer must never be read mid-write.
        std::lock_guard<std::mutex> lock(buffer.Mutex);
        AppendSample(buffer, name, module, startMs);
    }

    // ─── frame boundaries ───────────────────────────────────────────────────
    void Profiler::BeginFrame()
    {
        // The first BeginFrame registers the main thread so its buffer stays
        // lock-free for the rest of the session.
        if (s_MainThreadId == kNoThread)
        {
            ThreadBuffer& buffer = CurrentBuffer();
            s_MainThreadId = buffer.Id;
            buffer.IsMainThread = true;
            std::snprintf(buffer.Name, sizeof(buffer.Name), "Main");
        }
        s_FrameStartMs = NowMs();
        s_HaveFrameStart = true;
        s_DroppedSamples.store(0, std::memory_order_relaxed);
    }

    void Profiler::EndFrame()
    {
        // Capture off (disabled or frozen): drop whatever the thread buffers
        // collected and leave the last captured frame intact, so the panel keeps
        // showing it instead of blanking out and the history stops growing.
        if (!s_Enabled.load(std::memory_order_relaxed) || !s_Capturing.load(std::memory_order_relaxed))
        {
            {
                std::lock_guard<std::mutex> lock(s_BufferMutex);
                s_DrainList = s_Buffers;
            }
            DiscardPendingSamples(s_DrainList);
            s_DroppedSamples.store(0, std::memory_order_relaxed);
            s_HaveFrameStart = false;
            return;
        }

        const f64 endMs = NowMs();
        const f32 frameMs = s_HaveFrameStart ? static_cast<f32>(endMs - s_FrameStartMs) : 0.0f;

        // ── drain the per-thread buffers ────────────────────────────────────
        {
            std::lock_guard<std::mutex> lock(s_BufferMutex);
            s_DrainList = s_Buffers;
        }

        struct ThreadRange
        {
            ProfileThreadId Id;
            u32 Begin;
            u32 End;
        };
        std::vector<ThreadRange> ranges;
        ranges.reserve(s_DrainList.size());
        s_MergedSamples.clear();

        for (ThreadBuffer* buffer : s_DrainList)
        {
            const u32 begin = static_cast<u32>(s_MergedSamples.size());
            if (buffer->IsMainThread)
            {
                for (u32 i = 0; i < buffer->Count; ++i)
                    s_MergedSamples.push_back(buffer->Samples[i]);
                buffer->Count = 0;
            }
            else
            {
                std::lock_guard<std::mutex> lock(buffer->Mutex);
                for (u32 i = 0; i < buffer->Count; ++i)
                    s_MergedSamples.push_back(buffer->Samples[i]);
                buffer->Count = 0;
            }
            if (s_MergedSamples.size() > begin)
                ranges.push_back({buffer->Id, begin, static_cast<u32>(s_MergedSamples.size())});
        }

        // ── call tree (per thread, pre-order) ───────────────────────────────
        // Samples are appended on scope *exit*, so walking a thread's range
        // backwards yields scope *entry* order and a depth stack relinks parents.
        s_FrameTree.clear();
        s_TreeNodeKeys.clear();
        s_ThreadStats.clear();
        s_ThreadSlots.clear();

        struct StackEntry
        {
            u16 RecordedDepth;
            i32 Node;
        };
        std::vector<StackEntry> stack;

        f32 scopedCpuMs = 0.0f;

        for (const ThreadRange& range : ranges)
        {
            stack.clear();
            const u32 threadSlot =
                s_ThreadSlots.try_emplace(range.Id, static_cast<u32>(s_ThreadStats.size())).first->second;
            if (threadSlot == s_ThreadStats.size())
                s_ThreadStats.push_back({range.Id, GetThreadName(range.Id), 0.0f, 0});

            for (u32 i = range.End; i-- > range.Begin;)
            {
                const ProfileSample& sample = s_MergedSamples[i];
                while (!stack.empty() && stack.back().RecordedDepth >= sample.Depth)
                    stack.pop_back();

                ProfileNode node;
                node.Name = sample.Name != nullptr ? sample.Name : "?";
                node.Module = Basename(sample.Module);
                node.StartMs = sample.StartMs;
                node.TotalMs = static_cast<f32>(sample.EndMs - sample.StartMs);
                node.SelfMs = node.TotalMs;
                node.Depth = static_cast<u16>(stack.size());
                node.Parent = stack.empty() ? -1 : stack.back().Node;
                node.Thread = range.Id;

                const i32 index = static_cast<i32>(s_FrameTree.size());
                if (node.Parent >= 0)
                {
                    ProfileNode& parent = s_FrameTree[static_cast<std::size_t>(node.Parent)];
                    parent.SelfMs -= node.TotalMs;
                }
                else
                {
                    s_ThreadStats[threadSlot].BusyMs += node.TotalMs;
                    if (range.Id == GetMainThreadId())
                        scopedCpuMs += node.TotalMs;
                }
                s_ThreadStats[threadSlot].Samples++;
                s_FrameTree.push_back(std::move(node));
                s_TreeNodeKeys.push_back({sample.Name, sample.Module});
                stack.push_back({sample.Depth, index});
            }
        }

        // Children are linked here because the walk above visits a thread's
        // samples in reverse entry order: prepending as we go leaves every
        // sibling list in entry (timeline) order.
        for (std::size_t i = 0; i < s_FrameTree.size(); ++i)
        {
            ProfileNode& node = s_FrameTree[i];
            if (node.Parent < 0)
                continue;
            ProfileNode& parent = s_FrameTree[static_cast<std::size_t>(node.Parent)];
            node.NextSibling = parent.FirstChild;
            parent.FirstChild = static_cast<i32>(i);
        }

        // ── per-scope totals for the captured frame ─────────────────────────
        s_FrameEntries.clear();
        s_EntrySlots.clear();
        s_EntrySlots.reserve(s_Accumulators.size() + 16);

        for (std::size_t i = 0; i < s_FrameTree.size(); ++i)
        {
            const ProfileNode& node = s_FrameTree[i];
            const ScopeKey& key = s_TreeNodeKeys[i];
            const u32 slot = s_EntrySlots.try_emplace(key, static_cast<u32>(s_FrameEntries.size())).first->second;
            if (slot == s_FrameEntries.size())
            {
                FrameEntry entry;
                entry.Stats.Name = node.Name;
                entry.Stats.Module = node.Module;
                entry.Key = key;
                s_FrameEntries.push_back(std::move(entry));
            }

            FrameEntry& entry = s_FrameEntries[slot];
            entry.Stats.TotalMs += node.TotalMs;
            entry.Stats.SelfMs += node.SelfMs;
            entry.Stats.Calls++;
            if (node.Thread == GetMainThreadId())
                entry.Stats.MainThreadMs += node.TotalMs;
            if (node.Thread < 64)
            {
                const u64 bit = 1ull << node.Thread;
                if ((entry.ThreadMask & bit) == 0)
                {
                    entry.ThreadMask |= bit;
                    entry.Stats.Threads++;
                }
            }
        }

        // ── publish + fold into the rolling per-name window ─────────────────
        s_ScopeStats.clear();
        s_ScopeStats.reserve(s_FrameEntries.size());

        for (FrameEntry& entry : s_FrameEntries)
        {
            ScopeAccumulator& accumulator = s_Accumulators[entry.Key];
            accumulator.Window[accumulator.WindowNext] = entry.Stats.TotalMs;
            accumulator.WindowNext = (accumulator.WindowNext + 1) % kAveragingWindow;
            accumulator.WindowCount = std::min(accumulator.WindowCount + 1, kAveragingWindow);
            if (accumulator.MinSinceReset <= 0.0f || entry.Stats.TotalMs < accumulator.MinSinceReset)
                accumulator.MinSinceReset = entry.Stats.TotalMs;
            if (entry.Stats.TotalMs > accumulator.MaxSinceReset)
                accumulator.MaxSinceReset = entry.Stats.TotalMs;

            f32 sum = 0.0f;
            for (u32 i = 0; i < accumulator.WindowCount; ++i)
                sum += accumulator.Window[i];

            entry.Stats.AverageMs = accumulator.WindowCount > 0 ? sum / static_cast<f32>(accumulator.WindowCount) : 0.0f;
            entry.Stats.MinMs = accumulator.MinSinceReset;
            entry.Stats.MaxMs = accumulator.MaxSinceReset;
            entry.Stats.FramePercent = frameMs > 0.0f ? entry.Stats.TotalMs / frameMs : 0.0f;

            s_ScopeStats.push_back(std::move(entry.Stats));
        }

        std::sort(s_ScopeStats.begin(), s_ScopeStats.end(),
                  [](const ProfileScopeStats& a, const ProfileScopeStats& b) { return a.TotalMs > b.TotalMs; });

        // ── frame record ────────────────────────────────────────────────────
        s_FrameSampleCount = static_cast<u32>(s_MergedSamples.size());
        s_FrameDropped = s_DroppedSamples.load(std::memory_order_relaxed);
        s_LastFrameMs = frameMs;
        s_ScopedCpuMs = scopedCpuMs;
        s_CapturedFrames++;

        if (frameMs > 0.0f)
        {
            if (s_MinFrameMs <= 0.0f || frameMs < s_MinFrameMs)
                s_MinFrameMs = frameMs;
            if (frameMs > s_MaxFrameMs)
                s_MaxFrameMs = frameMs;
            const u32 window = static_cast<u32>(std::min<u64>(s_CapturedFrames, kAveragingWindow));
            s_AverageFrameMs += (frameMs - s_AverageFrameMs) / static_cast<f32>(window);
        }

        ProfileFrameRecord record;
        record.FrameMs = frameMs;
        record.Fps = frameMs > 0.0f ? 1000.0f / frameMs : 0.0f;
        record.ScopedCpuMs = s_ScopedCpuMs;
        record.SampleCount = s_FrameSampleCount;
        record.DroppedSamples = s_FrameDropped;
        s_FrameHistory.push_back(record);
        if (s_FrameHistory.size() > kFrameHistory)
            s_FrameHistory.erase(s_FrameHistory.begin(),
                                 s_FrameHistory.begin() + static_cast<std::ptrdiff_t>(s_FrameHistory.size() - kFrameHistory));
    }

    // ─── read API ───────────────────────────────────────────────────────────
    const std::vector<ProfileScopeStats>& Profiler::GetScopeStats() { return s_ScopeStats; }
    const std::vector<ProfileNode>& Profiler::GetFrameTree() { return s_FrameTree; }
    const std::vector<ProfileThreadStats>& Profiler::GetThreadStats() { return s_ThreadStats; }
    const std::vector<ProfileFrameRecord>& Profiler::GetFrameHistory() { return s_FrameHistory; }

    f32 Profiler::GetLastFrameMs() { return s_LastFrameMs; }
    f32 Profiler::GetAverageFrameMs() { return s_AverageFrameMs; }
    f32 Profiler::GetMinFrameMs() { return s_MinFrameMs; }
    f32 Profiler::GetMaxFrameMs() { return s_MaxFrameMs; }
    f32 Profiler::GetScopedCpuMs() { return s_ScopedCpuMs; }
    u32 Profiler::GetFrameSampleCount() { return s_FrameSampleCount; }
    u32 Profiler::GetDroppedSampleCount() { return s_FrameDropped; }
    u64 Profiler::GetCapturedFrameCount() { return s_CapturedFrames; }
    u32 Profiler::GetAveragingWindow() { return kAveragingWindow; }

    const char* Profiler::GetThreadName(ProfileThreadId id)
    {
        std::lock_guard<std::mutex> lock(s_BufferMutex);
        for (ThreadBuffer* buffer : s_Buffers)
            if (buffer->Id == id)
                return buffer->Name;
        return "Thread";
    }

    // ─── scope ──────────────────────────────────────────────────────────────
    ProfilerScope::ProfilerScope(const char* name, const char* module)
        : m_Name(name), m_Module(module), m_StartMs(Profiler::BeginScope())
    {
    }

    ProfilerScope::~ProfilerScope()
    {
        if (m_StartMs >= 0.0)
            Profiler::EndScope(m_Name, m_Module, m_StartMs);
    }
} // namespace UHE
