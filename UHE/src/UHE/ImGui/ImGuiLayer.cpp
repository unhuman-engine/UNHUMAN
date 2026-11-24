#include "uhepch.h"
#include "ImGuiLayer.h"
// clang-format off
// ImGuizmo.h is not self-contained: it uses ImGui types without including imgui.h.
#include "imgui.h"
#include <ImGuizmo.h>
// clang-format on
#include <GLFW/glfw3.h>
#include "Platform/Vulkan/UI/VulkanImGuiBackend.h"
#include "Platform/Vulkan/VulkanDevice.h"
#include "UHE/AssetsManager/VfsSystem.h"
#include "UHE/Core/Application.h"
#include "UHE/Events/Event.h"
#include "UHE/Renderer/Renderer.h"

namespace UHE
{
ImGuiLayer::ImGuiLayer() : Layer("ImGuiLayer") {}
ImGuiLayer::~ImGuiLayer() {}
void ImGuiLayer::OnAttach()
{
    UHE_PROFILE_FUNCTION();
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    // ImGui::StyleColorsDark();

    ImGuiIO& io = ImGui::GetIO();
    (void)io;

    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    // A captured frame must not depend on a layout saved on this machine.
    if (Application::Get().GetCaptureSettings().CaptureEnabled())
        io.IniFilename = nullptr;
    // io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;

    //
    io.FontDefault =
        io.Fonts->AddFontFromFileTTF(FileSystem::Get().Resolve("Inter/static/Inter_18pt-Regular.ttf").c_str(), 16.0f);
    io.Fonts->AddFontFromFileTTF(FileSystem::Get().Resolve("Inter/static/Inter_18pt-Bold.ttf").c_str(), 18.0f);

    ImGui::StyleColorsDark();

    ImGuiStyle& style = ImGui::GetStyle();
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
    {
        style.WindowRounding = 0.0f;
        style.Colors[ImGuiCol_WindowBg].w = 1.0f;
    }

    SetDarkThemeColor();
}

void ImGuiLayer::OnDetach()
{
    UHE_PROFILE_FUNCTION();
    // Derived class handles backend shutdown
    ImGui::DestroyContext();
}

void ImGuiLayer::OnEvent(Event& e)
{

    if (m_BlockEvents)
    {
        ImGuiIO& io = ImGui::GetIO();
        e.Handled |= e.IsInCategory(EventCategoryMouse) & io.WantCaptureMouse;
        e.Handled |= e.IsInCategory(EventCategoryKeyboard) & io.WantCaptureKeyboard;
    }
}

/*void ImGuiLayer::OnImGuiRender()
{
    static bool show = true;
    ImGui::ShowDemoWindow(&show);
}*/

class StubImGuiLayer : public ImGuiLayer
{
public:
    virtual void Begin() override {}
    virtual void End() override {}
};

ImGuiLayer* ImGuiLayer::Create()
{
    auto* device = dynamic_cast<RHI::VULKAN::VulkanDevice*>(&Renderer::GetDevice());
    if (device)
        return new RHI::VULKAN::VulkanImGuiLayer(device);
    return new StubImGuiLayer();
}

void ImGuiLayer::SetDarkThemeColor()
{
    auto& style = ImGui::GetStyle();
    auto& colors = style.Colors;

    style.WindowPadding = ImVec2(10, 10);
    style.FramePadding = ImVec2(6, 4);
    style.CellPadding = ImVec2(4, 3);
    style.ItemSpacing = ImVec2(8, 5);
    style.ItemInnerSpacing = ImVec2(6, 4);
    style.IndentSpacing = 22.0f;
    style.ScrollbarSize = 13.0f;
    style.GrabMinSize = 10.0f;

    style.WindowRounding = 0.0f;
    style.ChildRounding = 3.0f;
    style.FrameRounding = 3.0f;
    style.PopupRounding = 4.0f;
    style.ScrollbarRounding = 6.0f;
    style.GrabRounding = 3.0f;
    style.TabRounding = 3.0f;

    style.WindowBorderSize = 1.0f;
    style.ChildBorderSize = 1.0f;
    style.FrameBorderSize = 1.0f;
    style.PopupBorderSize = 1.0f;
    style.TabBorderSize = 0.0f;

    // --- Color Palette ---
    // Base:   refined dark black/charcoal
    // Accent: soft violet #6C63FF  (0.424, 0.388, 1.0)

    // Text
    colors[ImGuiCol_Text] = ImVec4{0.92f, 0.92f, 0.92f, 1.00f}; // bright off-white
    colors[ImGuiCol_TextDisabled] = ImVec4{0.42f, 0.42f, 0.42f, 1.00f};

    // Backgrounds
    colors[ImGuiCol_WindowBg] = ImVec4{0.00f, 0.00f, 0.00f, 1.00f}; // Pure Black
    colors[ImGuiCol_ChildBg] = ImVec4{0.00f, 0.00f, 0.00f, 1.00f};  // Pure Black
    colors[ImGuiCol_PopupBg] = ImVec4{0.02f, 0.02f, 0.02f, 0.98f};  // Almost Black

    // Borders
    colors[ImGuiCol_Border] = ImVec4{0.10f, 0.10f, 0.10f, 0.80f};
    colors[ImGuiCol_BorderShadow] = ImVec4{0.00f, 0.00f, 0.00f, 0.00f};

    // Frame BG (inputs, checkboxes, sliders)
    colors[ImGuiCol_FrameBg] = ImVec4{0.10f, 0.10f, 0.10f, 1.00f}; // #1A1A1A
    colors[ImGuiCol_FrameBgHovered] = ImVec4{0.15f, 0.15f, 0.15f, 1.00f};
    colors[ImGuiCol_FrameBgActive] = ImVec4{0.20f, 0.20f, 0.20f, 1.00f};

    // Title bar
    colors[ImGuiCol_TitleBg] = ImVec4{0.02f, 0.02f, 0.02f, 1.00f};
    colors[ImGuiCol_TitleBgActive] = ImVec4{0.04f, 0.04f, 0.04f, 1.00f};
    colors[ImGuiCol_TitleBgCollapsed] = ImVec4{0.00f, 0.00f, 0.00f, 0.75f};

    // Menu bar
    colors[ImGuiCol_MenuBarBg] = ImVec4{0.02f, 0.02f, 0.02f, 1.00f};

    // Scrollbar
    colors[ImGuiCol_ScrollbarBg] = ImVec4{0.05f, 0.05f, 0.05f, 0.60f};
    colors[ImGuiCol_ScrollbarGrab] = ImVec4{0.20f, 0.20f, 0.20f, 1.00f};
    colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4{0.30f, 0.30f, 0.30f, 1.00f};
    colors[ImGuiCol_ScrollbarGrabActive] = ImVec4{0.424f, 0.388f, 1.000f, 1.00f}; // accent

    // Check / Slider
    colors[ImGuiCol_CheckMark] = ImVec4{0.424f, 0.388f, 1.000f, 1.00f}; // #6C63FF
    colors[ImGuiCol_SliderGrab] = ImVec4{0.424f, 0.388f, 1.000f, 0.85f};
    colors[ImGuiCol_SliderGrabActive] = ImVec4{0.525f, 0.490f, 1.000f, 1.00f};

    // Buttons
    colors[ImGuiCol_Button] = ImVec4{0.13f, 0.13f, 0.13f, 1.00f};
    colors[ImGuiCol_ButtonHovered] = ImVec4{0.20f, 0.20f, 0.20f, 1.00f};
    colors[ImGuiCol_ButtonActive] = ImVec4{0.28f, 0.28f, 0.28f, 1.00f};

    // Headers (collapsing headers, tree nodes, selectable)
    colors[ImGuiCol_Header] = ImVec4{0.12f, 0.12f, 0.12f, 1.00f};
    colors[ImGuiCol_HeaderHovered] = ImVec4{0.18f, 0.18f, 0.18f, 1.00f};
    colors[ImGuiCol_HeaderActive] = ImVec4{0.25f, 0.25f, 0.25f, 1.00f};

    // Separator
    colors[ImGuiCol_Separator] = ImVec4{0.14f, 0.14f, 0.14f, 0.80f};
    colors[ImGuiCol_SeparatorHovered] = ImVec4{0.424f, 0.388f, 1.000f, 0.60f};
    colors[ImGuiCol_SeparatorActive] = ImVec4{0.424f, 0.388f, 1.000f, 1.00f};

    // Resize grip
    colors[ImGuiCol_ResizeGrip] = ImVec4{0.424f, 0.388f, 1.000f, 0.20f};
    colors[ImGuiCol_ResizeGripHovered] = ImVec4{0.424f, 0.388f, 1.000f, 0.50f};
    colors[ImGuiCol_ResizeGripActive] = ImVec4{0.424f, 0.388f, 1.000f, 0.90f};

    // Tabs
    colors[ImGuiCol_Tab] = ImVec4{0.08f, 0.08f, 0.08f, 1.00f};
    colors[ImGuiCol_TabHovered] = ImVec4{0.20f, 0.20f, 0.20f, 1.00f};
    colors[ImGuiCol_TabActive] = ImVec4{0.14f, 0.14f, 0.14f, 1.00f};
    colors[ImGuiCol_TabUnfocused] = ImVec4{0.06f, 0.06f, 0.06f, 1.00f};
    colors[ImGuiCol_TabUnfocusedActive] = ImVec4{0.10f, 0.10f, 0.10f, 1.00f};

    // Docking
    colors[ImGuiCol_DockingPreview] = ImVec4{0.424f, 0.388f, 1.000f, 0.70f}; // accent
    colors[ImGuiCol_DockingEmptyBg] = ImVec4{0.04f, 0.04f, 0.04f, 1.00f};

    // Plot (graphs)
    colors[ImGuiCol_PlotLines] = ImVec4{0.424f, 0.388f, 1.000f, 1.00f};
    colors[ImGuiCol_PlotLinesHovered] = ImVec4{0.525f, 0.490f, 1.000f, 1.00f};
    colors[ImGuiCol_PlotHistogram] = ImVec4{0.424f, 0.388f, 1.000f, 1.00f};
    colors[ImGuiCol_PlotHistogramHovered] = ImVec4{0.525f, 0.490f, 1.000f, 1.00f};

    // Table
    colors[ImGuiCol_TableHeaderBg] = ImVec4{0.10f, 0.10f, 0.10f, 1.00f};
    colors[ImGuiCol_TableBorderStrong] = ImVec4{0.14f, 0.14f, 0.14f, 1.00f};
    colors[ImGuiCol_TableBorderLight] = ImVec4{0.10f, 0.10f, 0.10f, 1.00f};
    colors[ImGuiCol_TableRowBg] = ImVec4{0.00f, 0.00f, 0.00f, 0.00f};
    colors[ImGuiCol_TableRowBgAlt] = ImVec4{1.00f, 1.00f, 1.00f, 0.02f};

    // Text selection / input cursor
    colors[ImGuiCol_TextSelectedBg] = ImVec4{0.424f, 0.388f, 1.000f, 0.35f};
    colors[ImGuiCol_DragDropTarget] = ImVec4{0.424f, 0.388f, 1.000f, 0.90f};
    colors[ImGuiCol_NavHighlight] = ImVec4{0.424f, 0.388f, 1.000f, 1.00f};
    colors[ImGuiCol_NavWindowingHighlight] = ImVec4{1.00f, 1.00f, 1.00f, 0.70f};
    colors[ImGuiCol_NavWindowingDimBg] = ImVec4{0.80f, 0.80f, 0.80f, 0.20f};
    colors[ImGuiCol_ModalWindowDimBg] = ImVec4{0.00f, 0.00f, 0.00f, 0.55f};
}

} // namespace UHE