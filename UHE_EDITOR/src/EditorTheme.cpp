#include "EditorTheme.h"
#include <filesystem>
#include <fstream>

namespace EditorTheme
{
    namespace
    {
        // ---------- small color helpers ----------
        ImVec4 Shade(const ImVec4& c, float t)
        {
            // t > 0: toward white, t < 0: toward black.
            ImVec4 r = c;
            if (t >= 0.0f)
            {
                r.x += (1.0f - r.x) * t;
                r.y += (1.0f - r.y) * t;
                r.z += (1.0f - r.z) * t;
            }
            else
            {
                r.x *= (1.0f + t);
                r.y *= (1.0f + t);
                r.z *= (1.0f + t);
            }
            return r;
        }

        ImVec4 Mix(const ImVec4& a, const ImVec4& b, float t)
        {
            return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w);
        }

        ImVec4 WithAlpha(const ImVec4& c, float a) { return ImVec4(c.x, c.y, c.z, a); }

        // ---------- theme definitions ----------
        struct ThemeDef
        {
            const char* Name;
            bool Light;
            ImVec4 Accent;   // brand color (checkmarks, sliders, selection...)
            ImVec4 Bg;       // window background
            ImVec4 Text;     // primary text
            ImVec4 TextDim;  // disabled text
            float TitleTint; // accent mixed into the focused title bar
        };

        const ThemeDef kThemes[] = {
            {"Dark Violet", false, {0.424f, 0.388f, 1.000f, 1.0f}, {0.095f, 0.095f, 0.115f, 1.0f}, {0.920f, 0.920f, 0.940f, 1.0f}, {0.480f, 0.480f, 0.540f, 1.0f}, 0.07f},
            {"Midnight",    false, {0.260f, 0.780f, 0.870f, 1.0f}, {0.070f, 0.085f, 0.105f, 1.0f}, {0.880f, 0.910f, 0.940f, 1.0f}, {0.430f, 0.470f, 0.520f, 1.0f}, 0.07f},
            {"Graphite",    false, {0.960f, 0.640f, 0.240f, 1.0f}, {0.110f, 0.110f, 0.110f, 1.0f}, {0.900f, 0.890f, 0.870f, 1.0f}, {0.470f, 0.460f, 0.445f, 1.0f}, 0.06f},
            {"Light",       true,  {0.180f, 0.420f, 0.880f, 1.0f}, {0.940f, 0.942f, 0.948f, 1.0f}, {0.100f, 0.105f, 0.115f, 1.0f}, {0.550f, 0.560f, 0.580f, 1.0f}, 0.03f},
            {"Emerald",     false, {0.160f, 0.780f, 0.470f, 1.0f}, {0.075f, 0.105f, 0.090f, 1.0f}, {0.880f, 0.930f, 0.900f, 1.0f}, {0.450f, 0.520f, 0.480f, 1.0f}, 0.06f},
            {"Rose",        false, {0.940f, 0.350f, 0.500f, 1.0f}, {0.105f, 0.085f, 0.095f, 1.0f}, {0.930f, 0.890f, 0.910f, 1.0f}, {0.520f, 0.460f, 0.490f, 1.0f}, 0.07f},
            {"Ocean",       false, {0.300f, 0.520f, 0.960f, 1.0f}, {0.080f, 0.090f, 0.115f, 1.0f}, {0.880f, 0.900f, 0.940f, 1.0f}, {0.450f, 0.480f, 0.540f, 1.0f}, 0.07f},
            {"Sunset",      false, {0.980f, 0.480f, 0.280f, 1.0f}, {0.105f, 0.085f, 0.075f, 1.0f}, {0.940f, 0.900f, 0.870f, 1.0f}, {0.530f, 0.470f, 0.440f, 1.0f}, 0.07f},
            {"Nord",        false, {0.460f, 0.620f, 0.780f, 1.0f}, {0.180f, 0.204f, 0.251f, 1.0f}, {0.847f, 0.871f, 0.914f, 1.0f}, {0.480f, 0.510f, 0.560f, 1.0f}, 0.05f},
            {"Dracula",     false, {0.745f, 0.580f, 0.986f, 1.0f}, {0.133f, 0.129f, 0.173f, 1.0f}, {0.968f, 0.968f, 0.952f, 1.0f}, {0.550f, 0.530f, 0.600f, 1.0f}, 0.08f},
            {"Paper",       true,  {0.720f, 0.450f, 0.200f, 1.0f}, {0.965f, 0.955f, 0.935f, 1.0f}, {0.150f, 0.130f, 0.110f, 1.0f}, {0.550f, 0.520f, 0.470f, 1.0f}, 0.03f},
            {"Carbon",      false, {0.900f, 0.920f, 0.960f, 1.0f}, {0.045f, 0.045f, 0.048f, 1.0f}, {0.920f, 0.930f, 0.950f, 1.0f}, {0.440f, 0.450f, 0.470f, 1.0f}, 0.05f},
        };
        static_assert(sizeof(kThemes) / sizeof(kThemes[0]) == (int)EditorThemeId::COUNT,
                      "Theme table out of sync with EditorThemeId");

        // ---------- state ----------
        EditorThemeId s_Selected = EditorThemeId::DarkViolet;
        bool s_Applied = false;
        bool s_CustomEnabled = false;
        ImVec4 s_CustomAccent{0.424f, 0.388f, 1.000f, 1.0f};
        ImVec4 s_ConsoleInfo{1, 1, 1, 1}, s_ConsoleWarn{1, 1, 0, 1}, s_ConsoleError{1, 0, 0, 1}, s_ConsoleCritical{1, 0, 1, 1};

        // ---------- style builder ----------
        void BuildStyle(ImGuiStyle& s, const ThemeDef& t)
        {
            ImVec4* c = s.Colors;
            const ImVec4& A = t.Accent;
            const ImVec4& bg = t.Bg;
            const bool light = t.Light;

            // Shading direction: dark themes raise surfaces toward white, light
            // themes lower them toward gray, so inputs/panels keep contrast.
            const float dir = light ? -1.0f : 1.0f;
            auto raise = [&](float amount) { return Shade(bg, dir * amount); };

            c[ImGuiCol_Text]                  = t.Text;
            c[ImGuiCol_TextDisabled]          = t.TextDim;
            c[ImGuiCol_WindowBg]              = bg;
            c[ImGuiCol_ChildBg]               = raise(0.007f);
            c[ImGuiCol_PopupBg]               = light ? Shade(bg, 0.030f) : raise(0.016f);
            c[ImGuiCol_PopupBg].w             = 0.985f;
            c[ImGuiCol_Border]                = WithAlpha(raise(0.105f), light ? 0.80f : 0.60f);
            c[ImGuiCol_BorderShadow]          = ImVec4(0, 0, 0, 0);
            c[ImGuiCol_FrameBg]               = raise(light ? 0.050f : 0.045f);
            c[ImGuiCol_FrameBgHovered]        = raise(light ? 0.090f : 0.075f);
            c[ImGuiCol_FrameBgActive]         = raise(light ? 0.125f : 0.105f);
            c[ImGuiCol_TitleBg]               = Shade(bg, light ? -0.060f : -0.015f);
            c[ImGuiCol_TitleBgActive]         = Mix(Shade(bg, light ? -0.045f : 0.010f), A, t.TitleTint);
            c[ImGuiCol_TitleBgCollapsed]      = Shade(bg, light ? -0.060f : -0.020f);
            c[ImGuiCol_MenuBarBg]             = raise(light ? 0.035f * -1.0f : 0.012f);
            c[ImGuiCol_ScrollbarBg]           = Shade(bg, light ? -0.030f : -0.002f);
            c[ImGuiCol_ScrollbarGrab]         = raise(0.150f);
            c[ImGuiCol_ScrollbarGrabHovered]  = raise(0.215f);
            c[ImGuiCol_ScrollbarGrabActive]   = A;
            c[ImGuiCol_CheckMark]             = A;
            c[ImGuiCol_SliderGrab]            = WithAlpha(A, 0.80f);
            c[ImGuiCol_SliderGrabActive]      = Shade(A, 0.150f);
            c[ImGuiCol_Button]                = raise(0.080f);
            c[ImGuiCol_ButtonHovered]         = Mix(raise(0.080f), A, light ? 0.14f : 0.20f);
            c[ImGuiCol_ButtonActive]          = WithAlpha(A, 0.85f);
            c[ImGuiCol_Header]                = Mix(bg, A, 0.10f);
            c[ImGuiCol_HeaderHovered]         = WithAlpha(A, 0.35f);
            c[ImGuiCol_HeaderActive]          = WithAlpha(A, 0.55f);
            c[ImGuiCol_Separator]             = WithAlpha(raise(0.110f), 0.70f);
            c[ImGuiCol_SeparatorHovered]      = WithAlpha(A, 0.60f);
            c[ImGuiCol_SeparatorActive]       = WithAlpha(A, 0.90f);
            c[ImGuiCol_ResizeGrip]            = WithAlpha(raise(0.120f), 0.50f);
            c[ImGuiCol_ResizeGripHovered]     = WithAlpha(A, 0.60f);
            c[ImGuiCol_ResizeGripActive]      = WithAlpha(A, 0.90f);
            c[ImGuiCol_InputTextCursor]       = A;
            c[ImGuiCol_TabHovered]            = WithAlpha(A, 0.40f);
            c[ImGuiCol_Tab]                   = raise(light ? 0.055f * -1.0f : 0.030f);
            c[ImGuiCol_TabSelected]           = Mix(bg, A, light ? 0.05f : 0.10f);
            c[ImGuiCol_TabSelectedOverline]   = A;
            c[ImGuiCol_TabDimmed]             = Shade(bg, light ? -0.085f : -0.004f);
            c[ImGuiCol_TabDimmedSelected]     = raise(light ? 0.040f * -1.0f : 0.045f);
            c[ImGuiCol_TabDimmedSelectedOverline] = Mix(A, bg, 0.45f);
            c[ImGuiCol_DockingPreview]        = WithAlpha(A, 0.45f);
            c[ImGuiCol_DockingEmptyBg]        = Shade(bg, light ? -0.045f : -0.012f);
            c[ImGuiCol_PlotLines]             = A;
            c[ImGuiCol_PlotLinesHovered]      = Shade(A, 0.250f);
            c[ImGuiCol_PlotHistogram]         = WithAlpha(A, 0.85f);
            c[ImGuiCol_PlotHistogramHovered]  = Shade(A, 0.150f);
            c[ImGuiCol_TableHeaderBg]         = raise(0.050f);
            c[ImGuiCol_TableBorderStrong]     = raise(0.140f);
            c[ImGuiCol_TableBorderLight]      = raise(0.080f);
            c[ImGuiCol_TableRowBg]            = ImVec4(0, 0, 0, 0);
            c[ImGuiCol_TableRowBgAlt]         = light ? ImVec4(0, 0, 0, 0.030f) : ImVec4(1, 1, 1, 0.025f);
            c[ImGuiCol_TextLink]              = A;
            c[ImGuiCol_TextSelectedBg]        = WithAlpha(A, 0.35f);
            c[ImGuiCol_TreeLines]             = raise(0.170f);
            c[ImGuiCol_DragDropTarget]        = WithAlpha(A, 0.95f);
            c[ImGuiCol_DragDropTargetBg]      = WithAlpha(A, 0.10f);
            c[ImGuiCol_NavCursor]             = WithAlpha(A, 0.90f);
            c[ImGuiCol_NavWindowingHighlight] = WithAlpha(light ? ImVec4(0, 0, 0, 1) : ImVec4(1, 1, 1, 1), 0.70f);
            c[ImGuiCol_NavWindowingDimBg]     = ImVec4(0, 0, 0, light ? 0.45f : 0.55f);
            c[ImGuiCol_ModalWindowDimBg]      = ImVec4(0, 0, 0, light ? 0.45f : 0.55f);
        }

        // Overrides every accent slot with the user's custom color.
        void ApplyAccentOverride(ImGuiStyle& s, const ImVec4& A)
        {
            ImVec4* c = s.Colors;
            c[ImGuiCol_ScrollbarGrabActive]   = A;
            c[ImGuiCol_CheckMark]             = A;
            c[ImGuiCol_SliderGrab]            = WithAlpha(A, 0.80f);
            c[ImGuiCol_SliderGrabActive]      = Shade(A, 0.150f);
            c[ImGuiCol_ButtonActive]          = WithAlpha(A, 0.85f);
            c[ImGuiCol_ButtonHovered]         = Mix(c[ImGuiCol_Button], A, 0.20f);
            c[ImGuiCol_Header]                = Mix(c[ImGuiCol_WindowBg], A, 0.10f);
            c[ImGuiCol_HeaderHovered]         = WithAlpha(A, 0.35f);
            c[ImGuiCol_HeaderActive]          = WithAlpha(A, 0.55f);
            c[ImGuiCol_SeparatorHovered]      = WithAlpha(A, 0.60f);
            c[ImGuiCol_SeparatorActive]       = WithAlpha(A, 0.90f);
            c[ImGuiCol_ResizeGripHovered]     = WithAlpha(A, 0.60f);
            c[ImGuiCol_ResizeGripActive]      = WithAlpha(A, 0.90f);
            c[ImGuiCol_InputTextCursor]       = A;
            c[ImGuiCol_TabHovered]            = WithAlpha(A, 0.40f);
            c[ImGuiCol_TabSelected]           = Mix(c[ImGuiCol_WindowBg], A, 0.10f);
            c[ImGuiCol_TabSelectedOverline]   = A;
            c[ImGuiCol_TabDimmedSelectedOverline] = Mix(A, c[ImGuiCol_WindowBg], 0.45f);
            c[ImGuiCol_DockingPreview]        = WithAlpha(A, 0.45f);
            c[ImGuiCol_PlotLines]             = A;
            c[ImGuiCol_PlotLinesHovered]      = Shade(A, 0.250f);
            c[ImGuiCol_PlotHistogram]         = WithAlpha(A, 0.85f);
            c[ImGuiCol_PlotHistogramHovered]  = Shade(A, 0.150f);
            c[ImGuiCol_TextLink]              = A;
            c[ImGuiCol_TextSelectedBg]        = WithAlpha(A, 0.35f);
            c[ImGuiCol_TreeLines]             = Mix(c[ImGuiCol_TreeLines], A, 0.35f);
            c[ImGuiCol_DragDropTarget]        = WithAlpha(A, 0.95f);
            c[ImGuiCol_DragDropTargetBg]      = WithAlpha(A, 0.10f);
            c[ImGuiCol_NavCursor]             = WithAlpha(A, 0.90f);
            c[ImGuiCol_TitleBgActive]         = Mix(c[ImGuiCol_TitleBgActive], A, 0.35f);
        }

        void ApplyMetrics(ImGuiStyle& s)
        {
            s.WindowRounding    = 4.0f;
            s.ChildRounding     = 4.0f;
            s.FrameRounding     = 4.0f;
            s.PopupRounding     = 4.0f;
            s.GrabRounding      = 4.0f;
            s.TabRounding       = 5.0f;
            s.ScrollbarRounding = 9.0f;
            s.WindowBorderSize  = 1.0f;
            s.ChildBorderSize   = 1.0f;
            s.PopupBorderSize   = 1.0f;
            s.FrameBorderSize   = 0.0f;
            s.FramePadding      = ImVec2(7, 4);
            s.ItemSpacing       = ImVec2(8, 5);
            s.ItemInnerSpacing  = ImVec2(6, 5);
            s.IndentSpacing     = 20.0f;
            s.ScrollbarSize     = 13.0f;
            s.GrabMinSize       = 9.0f;
            s.WindowPadding     = ImVec2(8, 8);
            s.DockingSeparatorSize = 2.0f;
        }

        void ComputeConsolePalette(bool light)
        {
            if (light)
            {
                s_ConsoleInfo     = {0.120f, 0.130f, 0.150f, 1.0f};
                s_ConsoleWarn     = {0.760f, 0.550f, 0.000f, 1.0f};
                s_ConsoleError    = {0.800f, 0.150f, 0.150f, 1.0f};
                s_ConsoleCritical = {0.640f, 0.130f, 0.700f, 1.0f};
            }
            else
            {
                s_ConsoleInfo     = {0.920f, 0.920f, 0.940f, 1.0f};
                s_ConsoleWarn     = {1.000f, 0.750f, 0.250f, 1.0f};
                s_ConsoleError    = {0.960f, 0.360f, 0.360f, 1.0f};
                s_ConsoleCritical = {0.920f, 0.470f, 0.960f, 1.0f};
            }
        }

        // The converted string must itself be static: returning a temporary
        // path.string().c_str() handed fopen a dangling pointer (ASan
        // heap-use-after-free in LoadSelected).
        const std::string& SettingsPath()
        {
            static const std::string pathStr =
                (std::filesystem::current_path() / "editor_theme.ini").string();
            return pathStr;
        }

        void Persist()
        {
            // std::ofstream, no raw FILE* handle to leak or misuse.
            std::ofstream out(SettingsPath());
            if (!out)
                return; // read-only dir shouldn't break the UI
            out << (int)s_Selected << '\n'
                << (s_CustomEnabled ? 1 : 0) << '\n'
                << s_CustomAccent.x << ' ' << s_CustomAccent.y << ' ' << s_CustomAccent.z << '\n';
        }
    } // namespace

    void Apply(EditorThemeId id)
    {
        ImGuiStyle& st = ImGui::GetStyle();

        int i = (int)id;
        if (i < 0 || i >= (int)EditorThemeId::COUNT)
            i = 0;
        const ThemeDef& def = kThemes[i];

        if (def.Light)
            ImGui::StyleColorsLight(&st);
        else
            ImGui::StyleColorsDark(&st);
        BuildStyle(st, def);
        ApplyMetrics(st);

        if (s_CustomEnabled)
            ApplyAccentOverride(st, s_CustomAccent);

        ComputeConsolePalette(def.Light);

        s_Selected = (EditorThemeId)i;
        s_Applied = true;
    }

    void EnsureApplied()
    {
        if (!s_Applied)
            Apply(s_Selected);
    }

    EditorThemeId GetSelected() { return s_Selected; }

    const char* GetName(EditorThemeId id)
    {
        int i = (int)id;
        if (i < 0 || i >= (int)EditorThemeId::COUNT)
            return "Unknown";
        return kThemes[i].Name;
    }

    int GetThemeCount() { return (int)EditorThemeId::COUNT; }

    void SetSelected(EditorThemeId id)
    {
        Apply(id);
        Persist();
    }

    void LoadSelected()
    {
        s_CustomEnabled = false;
        s_CustomAccent = ImVec4{0.424f, 0.388f, 1.000f, 1.0f};

        std::ifstream in(SettingsPath());
        if (in)
        {
            int value = -1;
            if (in >> value && value >= 0 && value < (int)EditorThemeId::COUNT)
                s_Selected = (EditorThemeId)value;

            int custom = 0;
            if (in >> custom)
                s_CustomEnabled = (custom != 0);

            float r = 0.0f, g = 0.0f, b = 0.0f;
            if (in >> r >> g >> b)
            {
                // Accepted as-is (including black): SetCustomAccent persists
                // whatever the user picked, so loading must round-trip it.
                s_CustomAccent = ImVec4(r, g, b, 1.0f);
            }
        }
        s_Applied = false; // re-apply on first frame with the loaded settings
    }

    // ---- custom accent picker ----

    bool IsCustomAccentEnabled() { return s_CustomEnabled; }

    ImVec4 GetCustomAccent() { return s_CustomAccent; }

    void SetCustomAccentEnabled(bool enabled)
    {
        s_CustomEnabled = enabled;
        Apply(s_Selected);
        Persist();
    }

    void SetCustomAccent(const ImVec4& rgb)
    {
        s_CustomEnabled = true;
        s_CustomAccent = ImVec4(rgb.x, rgb.y, rgb.z, 1.0f);
        Apply(s_Selected);
        Persist();
    }

    // ---- console / text palette ----

    ImVec4 ConsoleInfo()     { return s_ConsoleInfo; }
    ImVec4 ConsoleWarn()     { return s_ConsoleWarn; }
    ImVec4 ConsoleError()    { return s_ConsoleError; }
    ImVec4 ConsoleCritical() { return s_ConsoleCritical; }

    // ---- palette accessors ----

    namespace
    {
        ImVec4 GetCol(ImGuiCol idx) { return ImGui::GetStyle().Colors[idx]; }
    }

    ImVec4 Accent()        { return GetCol(ImGuiCol_PlotLines); }
    ImVec4 AccentHover()   { return WithAlpha(Accent(), 0.85f); }
    ImVec4 AccentActive()  { return WithAlpha(Accent(), 0.70f); }
    ImVec4 AccentMuted()   { return WithAlpha(Accent(), 0.15f); }
    ImVec4 ToolbarBg()     { return GetCol(ImGuiCol_TitleBgActive); }

    ImVec4 AccentForeground()
    {
        // Relative luminance (Rec. 709): light accents need dark text, dark
        // accents need white. Fixes e.g. Carbon (near-white accent) rendering
        // white-on-white button labels.
        const ImVec4 a = Accent();
        float lum = 0.2126f * a.x + 0.7152f * a.y + 0.0722f * a.z;
        return lum > 0.55f ? ImVec4(0.080f, 0.080f, 0.090f, 1.0f) : ImVec4(0.960f, 0.960f, 0.960f, 1.0f);
    }
} // namespace EditorTheme
