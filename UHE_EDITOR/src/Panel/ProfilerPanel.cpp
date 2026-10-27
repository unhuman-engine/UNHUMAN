#include "ProfilerPanel.h"

#include "EditorTheme.h"
#include "EditorUI.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>

namespace UHE
{
    namespace
    {
        enum ScopeColumn
        {
            ColumnName = 0,
            ColumnModule,
            ColumnTotal,
            ColumnSelf,
            ColumnPercent,
            ColumnCalls,
            ColumnAverage,
            ColumnMax,
            ColumnMin,
            ColumnThreads,
            ColumnCount
        };

        ImVec4 WithAlpha(const ImVec4& color, float alpha)
        {
            return ImVec4(color.x, color.y, color.z, alpha);
        }

        std::string ToLowerCopy(const char* text)
        {
            std::string value = text != nullptr ? text : "";
            std::transform(value.begin(), value.end(), value.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return value;
        }

        bool ContainsCaseInsensitive(const std::string& haystack, const std::string& needle)
        {
            if (needle.empty())
                return true;
            return ToLowerCopy(haystack.c_str()).find(needle) != std::string::npos;
        }

        // Proportional bar drawn behind a numeric cell: makes the hot scopes
        // readable at a glance instead of comparing digits.
        void DrawCellBar(float fraction, const ImVec4& color)
        {
            if (fraction <= 0.0f)
                return;
            const float width = ImGui::GetContentRegionAvail().x;
            if (width <= 1.0f)
                return;
            const ImVec2 min = ImGui::GetCursorScreenPos();
            const float height = ImGui::GetTextLineHeight();
            ImGui::GetWindowDrawList()->AddRectFilled(
                min, ImVec2(min.x + width * ImMin(fraction, 1.0f), min.y + height),
                ImGui::GetColorU32(WithAlpha(color, 0.32f)), 3.0f);
        }
    } // namespace

    ImVec4 ProfilerPanel::PercentColor(float fraction)
    {
        if (fraction >= 0.35f)
            return EditorTheme::Danger();
        if (fraction >= 0.15f)
            return EditorTheme::Warning();
        if (fraction >= 0.05f)
            return EditorTheme::Accent();
        return EditorTheme::Success();
    }

    float ProfilerPanel::ScopePercent(const ProfileScopeStats& stats) const
    {
        return stats.FramePercent;
    }

    bool ProfilerPanel::MatchesFilter(const ProfileScopeStats& stats) const
    {
        if (stats.TotalMs < m_MinMilliseconds)
            return false;
        if (m_MainThreadOnly && stats.MainThreadMs <= 0.0f)
            return false;
        const std::string needle = ToLowerCopy(m_SearchBuffer);
        return ContainsCaseInsensitive(stats.Name, needle) || ContainsCaseInsensitive(stats.Module, needle);
    }

    void ProfilerPanel::OnImGuiRender()
    {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 10));
        ImGui::Begin("Profiler");

        DrawToolbar();
        ImGui::Spacing();
        DrawSummary();
        ImGui::Spacing();
        DrawHistoryGraph();

        if (ImGui::BeginTabBar("##ProfilerTabs"))
        {
            if (ImGui::BeginTabItem("Scopes"))
            {
                DrawHotspots();
                DrawScopeTable();
                DrawScopeDetails();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Call Tree"))
            {
                DrawCallTree();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Threads"))
            {
                DrawThreads();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }

        ImGui::End();
        ImGui::PopStyleVar();
    }

    void ProfilerPanel::DrawToolbar()
    {
        const bool enabled = Profiler::IsEnabled();
        const bool capturing = Profiler::IsCapturing();

        if (EditorUI::ToolbarToggle("Capture", enabled,
                                    "Sample UHE_PROFILE_SCOPE/UHE_PROFILE_FUNCTION scopes", ImVec2(84, 0)))
            Profiler::SetEnabled(!enabled);
        ImGui::SameLine();

        if (ImGui::Button(capturing ? "Freeze" : "Resume", ImVec2(84, 0)))
            Profiler::SetCapturing(!capturing);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Stop sampling and keep the last captured frame on screen");

        ImGui::SameLine();
        if (ImGui::Button("Reset", ImVec2(72, 0)))
        {
            Profiler::ResetHistory();
            m_SelectedScope = -1;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Clear averages, min/max and the history graph");

        ImGui::SameLine();
        if (ImGui::Button("Copy", ImVec2(72, 0)))
        {
            m_CopyBuffer.clear();
            char line[256];
            std::snprintf(line, sizeof(line), "frame %.3f ms (%.0f fps) | scoped CPU %.3f ms | %u samples\n",
                          Profiler::GetLastFrameMs(),
                          Profiler::GetLastFrameMs() > 0.0f ? 1000.0f / Profiler::GetLastFrameMs() : 0.0f,
                          Profiler::GetScopedCpuMs(), Profiler::GetFrameSampleCount());
            m_CopyBuffer += line;
            for (const ProfileScopeStats& stats : Profiler::GetScopeStats())
            {
                std::snprintf(line, sizeof(line), "%8.3f ms  %6.2f%%  self %8.3f  calls %5u  avg %8.3f  %s [%s]\n",
                              stats.TotalMs, stats.FramePercent * 100.0f, stats.SelfMs, stats.Calls,
                              stats.AverageMs, stats.Name.c_str(), stats.Module.c_str());
                m_CopyBuffer += line;
            }
            ImGui::SetClipboardText(m_CopyBuffer.c_str());
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Copy the captured frame's scope table to the clipboard");

        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();
        if (capturing)
            EditorUI::Badge("LIVE", EditorTheme::Success());
        else
            EditorUI::Badge("FROZEN", EditorTheme::Warning());
        ImGui::SameLine();
        ImGui::TextDisabled("frame %llu  |  %u scopes  |  %u samples",
                            static_cast<unsigned long long>(Profiler::GetCapturedFrameCount()),
                            static_cast<u32>(Profiler::GetScopeStats().size()), Profiler::GetFrameSampleCount());
        if (Profiler::GetDroppedSampleCount() > 0)
        {
            ImGui::SameLine();
            ImGui::TextColored(EditorTheme::Warning(), "| %u dropped (raise kSamplesPerThread)",
                               Profiler::GetDroppedSampleCount());
        }
    }

    void ProfilerPanel::DrawSummary()
    {
        const f32 frameMs = Profiler::GetLastFrameMs();
        const f32 averageMs = Profiler::GetAverageFrameMs();
        const f32 cpuMs = Profiler::GetScopedCpuMs();
        const f32 targetMs = 1000.0f / static_cast<f32>(m_TargetFps > 0 ? m_TargetFps : 60);

        EditorUI::SectionHeader("Frame");
        ImGui::Text("%.1f FPS", frameMs > 0.0f ? 1000.0f / frameMs : 0.0f);
        ImGui::SameLine(0.0f, 18.0f);
        ImGui::Text("frame %.2f ms", frameMs);
        ImGui::SameLine(0.0f, 12.0f);
        ImGui::TextDisabled("avg %.2f  min %.2f  max %.2f", averageMs, Profiler::GetMinFrameMs(),
                            Profiler::GetMaxFrameMs());

        ImGui::SameLine(0.0f, 18.0f);
        ImGui::TextColored(EditorTheme::Accent(), "CPU %.2f ms", cpuMs);
        ImGui::SameLine(0.0f, 8.0f);
        ImGui::TextDisabled("(%.0f%% of frame)", frameMs > 0.0f ? cpuMs / frameMs * 100.0f : 0.0f);

        ImGui::SameLine(0.0f, 18.0f);
        ImGui::SetNextItemWidth(90.0f);
        const char* fpsTargets[] = {"30 FPS", "60 FPS", "90 FPS", "120 FPS", "144 FPS", "240 FPS"};
        const int fpsValues[] = {30, 60, 90, 120, 144, 240};
        int targetIndex = 1;
        for (int i = 0; i < IM_ARRAYSIZE(fpsValues); ++i)
            if (fpsValues[i] == m_TargetFps)
                targetIndex = i;
        if (ImGui::Combo("##budgetTarget", &targetIndex, fpsTargets, IM_ARRAYSIZE(fpsTargets)))
            m_TargetFps = fpsValues[targetIndex];
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Budget target: frame time is compared against 1000 / target");

        const float load = targetMs > 0.0f ? frameMs / targetMs : 0.0f;
        char overlay[64];
        std::snprintf(overlay, sizeof(overlay), "%.0f%% of %.2f ms budget", load * 100.0f, targetMs);
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, PercentColor(load));
        ImGui::ProgressBar(ImMin(load, 1.0f), ImVec2(-1.0f, ImGui::GetFrameHeight() * 0.6f), overlay);
        ImGui::PopStyleColor();
    }

    void ProfilerPanel::DrawHistoryGraph()
    {
        const std::vector<ProfileFrameRecord>& history = Profiler::GetFrameHistory();

        m_HistoryFrameMs.clear();
        m_HistoryCpuMs.clear();
        m_HistoryFrameMs.reserve(history.size());
        for (const ProfileFrameRecord& record : history)
            m_HistoryFrameMs.push_back(record.FrameMs);

        EditorUI::SectionHeader("History");
        if (m_HistoryFrameMs.empty())
        {
            ImGui::TextDisabled("No frames captured yet");
            return;
        }

        const f32 targetMs = 1000.0f / static_cast<f32>(m_TargetFps > 0 ? m_TargetFps : 60);
        f32 maxMs = targetMs * 1.25f;
        for (f32 value : m_HistoryFrameMs)
            maxMs = ImMax(maxMs, value * 1.1f);

        const ImVec2 plotMin = ImGui::GetCursorScreenPos();
        ImGui::PlotLines("##frameHistory", m_HistoryFrameMs.data(), static_cast<int>(m_HistoryFrameMs.size()), 0,
                         nullptr, 0.0f, maxMs, ImVec2(-1.0f, 64.0f));
        const ImVec2 plotMax = ImGui::GetItemRectMax();

        // Budget line: everything above it is a missed frame.
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const float budgetY = ImLerp(plotMax.y, plotMin.y, targetMs / maxMs);
        draw->AddLine(ImVec2(plotMin.x, budgetY), ImVec2(plotMax.x, budgetY),
                      ImGui::GetColorU32(WithAlpha(EditorTheme::Danger(), 0.75f)), 1.0f);
        draw->AddText(ImVec2(plotMin.x + 6.0f, budgetY - ImGui::GetTextLineHeight() - 2.0f),
                      ImGui::GetColorU32(EditorTheme::Danger()), "budget");

        if (ImGui::IsItemHovered())
        {
            const int count = static_cast<int>(m_HistoryFrameMs.size());
            const int hovered = ImClamp(
                static_cast<int>((ImGui::GetIO().MousePos.x - plotMin.x) / ImMax(plotMax.x - plotMin.x, 1.0f) * count),
                0, count - 1);
            const ProfileFrameRecord& record = history[static_cast<std::size_t>(hovered)];
            ImGui::BeginTooltip();
            ImGui::Text("%d frames ago", count - 1 - hovered);
            ImGui::Separator();
            ImGui::Text("%.2f ms  (%.0f FPS)", record.FrameMs, record.Fps);
            ImGui::Text("scoped CPU %.2f ms", record.ScopedCpuMs);
            ImGui::Text("%u samples", record.SampleCount);
            ImGui::EndTooltip();
        }
    }

    void ProfilerPanel::DrawHotspots()
    {
        const std::vector<ProfileScopeStats>& stats = Profiler::GetScopeStats();
        if (stats.empty())
        {
            ImGui::TextDisabled("No scopes captured yet");
            return;
        }

        if (!ImGui::CollapsingHeader("Hotspots", ImGuiTreeNodeFlags_DefaultOpen))
            return;

        const int count = ImMin(5, static_cast<int>(stats.size()));
        for (int i = 0; i < count; ++i)
        {
            const ProfileScopeStats& scope = stats[static_cast<std::size_t>(i)];
            const float fraction = scope.FramePercent;
            ImGui::PushID(i);
            ImGui::TextColored(PercentColor(fraction), "%.3f ms", scope.TotalMs);
            ImGui::SameLine(0.0f, 8.0f);
            ImGui::TextUnformatted(scope.Name.c_str());
            ImGui::SameLine(0.0f, 8.0f);
            ImGui::TextDisabled("%.1f%%", fraction * 100.0f);

            const ImVec2 barMin = ImGui::GetCursorScreenPos();
            const float width = ImGui::GetContentRegionAvail().x;
            const float height = 3.0f;
            ImGui::GetWindowDrawList()->AddRectFilled(
                barMin, ImVec2(barMin.x + width * ImMin(fraction, 1.0f), barMin.y + height),
                ImGui::GetColorU32(PercentColor(fraction)), 2.0f);
            ImGui::Dummy(ImVec2(0.0f, height + 2.0f));
            ImGui::PopID();
        }
    }

    void ProfilerPanel::DrawScopeTable()
    {
        EditorUI::SectionHeader("Scopes");
        EditorUI::SearchField("##profilerSearch", m_SearchBuffer, sizeof(m_SearchBuffer), "Filter scopes...", 220.0f);
        ImGui::SameLine(0.0f, 10.0f);
        ImGui::SetNextItemWidth(110.0f);
        ImGui::DragFloat("##profilerMinMs", &m_MinMilliseconds, 0.01f, 0.0f, 100.0f, "min %.2f ms");
        ImGui::SameLine(0.0f, 10.0f);
        ImGui::Checkbox("Main thread only", &m_MainThreadOnly);
        ImGui::SameLine(0.0f, 10.0f);
        ImGui::TextDisabled("(click a row for details)");

        RefreshVisibleScopes();

        const float detailsHeight = m_SelectedScope >= 0 ? 46.0f : 0.0f;
        const float tableHeight = ImMax(ImGui::GetContentRegionAvail().y - detailsHeight, 80.0f);

        const ImGuiTableFlags flags = ImGuiTableFlags_Sortable | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                                      ImGuiTableFlags_BordersOuterV | ImGuiTableFlags_ScrollY |
                                      ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp;

        if (ImGui::BeginTable("##profilerScopes", ColumnCount, flags, ImVec2(0.0f, tableHeight)))
        {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Scope", ImGuiTableColumnFlags_WidthStretch, 0.34f, ColumnName);
            ImGui::TableSetupColumn("Module", ImGuiTableColumnFlags_WidthStretch, 0.16f, ColumnModule);
            ImGui::TableSetupColumn("Total ms",
                                    ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_DefaultSort |
                                        ImGuiTableColumnFlags_PreferSortDescending,
                                    0.14f, ColumnTotal);
            ImGui::TableSetupColumn("Self ms", ImGuiTableColumnFlags_WidthStretch, 0.11f, ColumnSelf);
            ImGui::TableSetupColumn("% frame", ImGuiTableColumnFlags_WidthStretch, 0.08f, ColumnPercent);
            ImGui::TableSetupColumn("Calls", ImGuiTableColumnFlags_WidthStretch, 0.06f, ColumnCalls);
            ImGui::TableSetupColumn("Avg ms", ImGuiTableColumnFlags_WidthStretch, 0.10f, ColumnAverage);
            ImGui::TableSetupColumn("Max ms", ImGuiTableColumnFlags_WidthStretch, 0.09f, ColumnMax);
            ImGui::TableSetupColumn("Min ms", ImGuiTableColumnFlags_WidthStretch, 0.09f, ColumnMin);
            ImGui::TableSetupColumn("Thr", ImGuiTableColumnFlags_WidthStretch, 0.05f, ColumnThreads);
            ImGui::TableHeadersRow();

            if (ImGuiTableSortSpecs* sortSpecs = ImGui::TableGetSortSpecs())
            {
                if (sortSpecs->SpecsCount > 0)
                {
                    const ImGuiTableColumnSortSpecs& spec = sortSpecs->Specs[0];
                    m_SortColumn = static_cast<int>(spec.ColumnUserID);
                    m_SortDescending = spec.SortDirection != ImGuiSortDirection_Ascending;
                }
            }
            ApplySort();

            f32 maxTotal = 0.0f;
            for (const ProfileScopeStats* scope : m_VisibleScopes)
                maxTotal = ImMax(maxTotal, scope->TotalMs);

            for (std::size_t row = 0; row < m_VisibleScopes.size(); ++row)
            {
                const ProfileScopeStats& scope = *m_VisibleScopes[row];
                const f32 fraction = ScopePercent(scope);
                const ImVec4 color = PercentColor(fraction);

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::PushID(static_cast<int>(row));
                const bool selected = m_SelectedScope == static_cast<i32>(row);
                if (ImGui::Selectable(scope.Name.c_str(), selected, ImGuiSelectableFlags_SpanAllColumns))
                    m_SelectedScope = selected ? -1 : static_cast<i32>(row);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s\n%s\nmain thread %.3f ms of %.3f ms", scope.Name.c_str(),
                                      scope.Module.c_str(), scope.MainThreadMs, scope.TotalMs);

                ImGui::TableSetColumnIndex(1);
                ImGui::TextDisabled("%s", scope.Module.c_str());

                ImGui::TableSetColumnIndex(2);
                DrawCellBar(maxTotal > 0.0f ? scope.TotalMs / maxTotal : 0.0f, color);
                ImGui::TextColored(color, "%.3f", scope.TotalMs);

                ImGui::TableSetColumnIndex(3);
                ImGui::Text("%.3f", scope.SelfMs);

                ImGui::TableSetColumnIndex(4);
                ImGui::TextColored(color, "%.1f%%", fraction * 100.0f);

                ImGui::TableSetColumnIndex(5);
                ImGui::Text("%u", scope.Calls);

                ImGui::TableSetColumnIndex(6);
                ImGui::Text("%.3f", scope.AverageMs);

                ImGui::TableSetColumnIndex(7);
                ImGui::Text("%.3f", scope.MaxMs);

                ImGui::TableSetColumnIndex(8);
                ImGui::Text("%.3f", scope.MinMs);

                ImGui::TableSetColumnIndex(9);
                ImGui::Text("%u", scope.Threads);
                ImGui::PopID();
            }

            ImGui::EndTable();
        }
    }

    void ProfilerPanel::RefreshVisibleScopes()
    {
        m_VisibleScopes.clear();
        for (const ProfileScopeStats& stats : Profiler::GetScopeStats())
            if (MatchesFilter(stats))
                m_VisibleScopes.push_back(&stats);
        ApplySort();

        if (m_SelectedScope >= static_cast<i32>(m_VisibleScopes.size()))
            m_SelectedScope = -1;
    }

    void ProfilerPanel::ApplySort()
    {
        const int column = m_SortColumn;
        const bool descending = m_SortDescending;
        std::stable_sort(m_VisibleScopes.begin(), m_VisibleScopes.end(),
                         [column, descending](const ProfileScopeStats* a, const ProfileScopeStats* b) {
                             auto compare = [column](const ProfileScopeStats* left, const ProfileScopeStats* right) {
                                 switch (column)
                                 {
                                     case ColumnName:
                                         return left->Name < right->Name;
                                     case ColumnModule:
                                         return left->Module < right->Module;
                                     case ColumnTotal:
                                         return left->TotalMs < right->TotalMs;
                                     case ColumnSelf:
                                         return left->SelfMs < right->SelfMs;
                                     case ColumnPercent:
                                         return left->FramePercent < right->FramePercent;
                                     case ColumnCalls:
                                         return left->Calls < right->Calls;
                                     case ColumnAverage:
                                         return left->AverageMs < right->AverageMs;
                                     case ColumnMax:
                                         return left->MaxMs < right->MaxMs;
                                     case ColumnMin:
                                         return left->MinMs < right->MinMs;
                                     case ColumnThreads:
                                         return left->Threads < right->Threads;
                                     default:
                                         return left->TotalMs < right->TotalMs;
                                 }
                             };
                             return descending ? compare(b, a) : compare(a, b);
                         });
    }

    void ProfilerPanel::DrawScopeDetails()
    {
        if (m_SelectedScope < 0 || m_SelectedScope >= static_cast<i32>(m_VisibleScopes.size()))
            return;

        const ProfileScopeStats& scope = *m_VisibleScopes[static_cast<std::size_t>(m_SelectedScope)];
        ImGui::Separator();
        ImGui::TextColored(PercentColor(ScopePercent(scope)), "%s", scope.Name.c_str());
        ImGui::SameLine(0.0f, 10.0f);
        ImGui::TextDisabled("[%s]  %u calls  |  %u threads  |  main thread %.3f ms", scope.Module.c_str(),
                            scope.Calls, scope.Threads, scope.MainThreadMs);
        ImGui::TextWrapped("%.3f ms total  |  %.3f ms self  |  %.1f%% of frame  |  avg %.3f ms  |  min %.3f ms  |  "
                           "max %.3f ms",
                           scope.TotalMs, scope.SelfMs, scope.FramePercent * 100.0f, scope.AverageMs, scope.MinMs,
                           scope.MaxMs);
    }

    void ProfilerPanel::DrawCallTree()
    {
        EditorUI::SectionHeader("Call Tree");
        ImGui::SetNextItemWidth(110.0f);
        ImGui::DragFloat("##treeMinMs", &m_TreeMinMilliseconds, 0.01f, 0.0f, 100.0f, "min %.2f ms");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Prune branches below this duration");
        ImGui::SameLine(0.0f, 10.0f);
        EditorUI::SearchField("##treeSearch", m_SearchBuffer, sizeof(m_SearchBuffer), "Filter scopes...", 200.0f);
        ImGui::SameLine(0.0f, 10.0f);
        if (ImGui::Button("Expand all"))
            m_TreeForceOpen = 1;
        ImGui::SameLine(0.0f, 6.0f);
        if (ImGui::Button("Collapse all"))
            m_TreeForceOpen = -1;

        const std::vector<ProfileNode>& tree = Profiler::GetFrameTree();
        if (tree.empty())
        {
            ImGui::TextDisabled("No scopes captured yet");
            return;
        }

        ImGui::BeginChild("##treeScroll", ImVec2(0.0f, 0.0f), false);
        for (std::size_t i = 0; i < tree.size(); ++i)
        {
            if (tree[i].Parent >= 0)
                continue; // drawn by its parent
            DrawTreeNode(static_cast<i32>(i));
        }
        ImGui::EndChild();
        m_TreeForceOpen = 0;
    }

    void ProfilerPanel::DrawTreeNode(i32 index)
    {
        const std::vector<ProfileNode>& tree = Profiler::GetFrameTree();
        if (index < 0 || index >= static_cast<i32>(tree.size()))
            return;

        const ProfileNode& node = tree[static_cast<std::size_t>(index)];
        if (node.TotalMs < m_TreeMinMilliseconds)
            return;

        const f32 frameMs = Profiler::GetLastFrameMs();
        const f32 fraction = frameMs > 0.0f ? node.TotalMs / frameMs : 0.0f;
        const ImVec4 color = PercentColor(fraction);

        ImGui::PushID(node.Name.c_str());
        ImGui::PushID(static_cast<int>(node.Depth));
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_FramePadding;
        const bool leaf = node.FirstChild < 0;
        if (leaf)
            flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
        if (m_TreeForceOpen != 0)
            ImGui::SetNextItemOpen(m_TreeForceOpen > 0, ImGuiCond_Always);

        const bool open = ImGui::TreeNodeEx("##node", flags, "%s  %.3f ms (self %.3f)  %.1f%%  [%s]",
                                            node.Name.c_str(), node.TotalMs, node.SelfMs, fraction * 100.0f,
                                            node.Module.c_str());
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s\nstart +%.3f ms  duration %.3f ms\nthread %s", node.Name.c_str(), node.StartMs,
                              node.TotalMs, Profiler::GetThreadName(node.Thread));

        if (open && !leaf)
        {
            for (i32 child = node.FirstChild; child >= 0; child = tree[static_cast<std::size_t>(child)].NextSibling)
                DrawTreeNode(child);
            ImGui::TreePop();
        }
        ImGui::PopID();
        ImGui::PopID();
    }

    void ProfilerPanel::DrawThreads()
    {
        EditorUI::SectionHeader("Threads");
        const std::vector<ProfileThreadStats>& threads = Profiler::GetThreadStats();
        if (threads.empty())
        {
            ImGui::TextDisabled("No scopes captured yet");
            return;
        }

        f32 busiest = 0.0f;
        for (const ProfileThreadStats& thread : threads)
            busiest = ImMax(busiest, thread.BusyMs);

        const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                                      ImGuiTableFlags_SizingStretchProp;
        if (ImGui::BeginTable("##profilerThreads", 4, flags))
        {
            ImGui::TableSetupColumn("Thread", ImGuiTableColumnFlags_WidthStretch, 0.35f);
            ImGui::TableSetupColumn("Busy ms", ImGuiTableColumnFlags_WidthStretch, 0.20f);
            ImGui::TableSetupColumn("Samples", ImGuiTableColumnFlags_WidthStretch, 0.15f);
            ImGui::TableSetupColumn("Load", ImGuiTableColumnFlags_WidthStretch, 0.30f);
            ImGui::TableHeadersRow();

            for (const ProfileThreadStats& thread : threads)
            {
                const f32 fraction = busiest > 0.0f ? thread.BusyMs / busiest : 0.0f;
                const ImVec4 color = PercentColor(fraction);

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(thread.Name.c_str());
                ImGui::TableSetColumnIndex(1);
                ImGui::TextColored(color, "%.3f", thread.BusyMs);
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%u", thread.Samples);
                ImGui::TableSetColumnIndex(3);
                DrawCellBar(fraction, color);
                ImGui::TextDisabled("%.0f%%", fraction * 100.0f);
            }

            ImGui::EndTable();
        }

        ImGui::Spacing();
        ImGui::TextDisabled("Busy time counts top-level scopes per thread. Threads only appear here once they run "
                            "instrumented code (UHE_PROFILE_SCOPE/FUNCTION).");
    }
} // namespace UHE
