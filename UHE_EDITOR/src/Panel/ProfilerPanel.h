#pragma once

#include "UHE/Debug/Profiler.h"

#include "imgui.h"

#include <string>
#include <vector>

namespace UHE
{
    // Frame profiler panel: frame budget, history graph, per-scope table with
    // sort/filter, the captured frame's call tree and a per-thread breakdown.
    // All data comes from UHE::Profiler (fed by UHE_PROFILE_SCOPE/FUNCTION).
    class ProfilerPanel
    {
    public:
        void OnImGuiRender();

    private:
        void DrawToolbar();
        void DrawSummary();
        void DrawHistoryGraph();
        void DrawHotspots();
        void DrawScopeTable();
        void DrawScopeDetails();
        void DrawCallTree();
        void DrawThreads();

        void RefreshVisibleScopes();
        void ApplySort();
        void ClearSelection();
        bool MatchesFilter(const ProfileScopeStats& stats) const;
        void DrawTreeNode(i32 index);
        float ScopePercent(const ProfileScopeStats& stats) const;
        static ImVec4 PercentColor(float fraction);

        // Filtering / sorting
        char m_SearchBuffer[128] = {};
        float m_MinMilliseconds = 0.0f;
        bool m_MainThreadOnly = false;
        int m_SortColumn = 2; // 0 total... see ScopeColumn; 2 == Total ms (default)
        bool m_SortDescending = true;

        // Selection identity, not a row index: Profiler::EndFrame rebuilds and
        // re-sorts the stats every frame, so an index would silently point at a
        // different scope. Resolved back to a row by RefreshVisibleScopes().
        std::string m_SelectedName;
        std::string m_SelectedModule;
        i32 m_SelectedRow = -1;

        // Tree tab
        int m_TreeForceOpen = 1; // 1 = expand all, -1 = collapse all (applied on the next frame)
        float m_TreeMinMilliseconds = 0.0f;

        // Frame budget target
        int m_TargetFps = 60;

        // Scratch (kept between frames to avoid reallocation)
        std::vector<const ProfileScopeStats*> m_VisibleScopes;
        std::vector<float> m_HistoryFrameMs;
        std::vector<float> m_HistoryCpuMs;
        std::string m_CopyBuffer;
    };
} // namespace UHE
