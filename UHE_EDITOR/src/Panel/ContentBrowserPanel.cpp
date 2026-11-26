#include "uhepch.h"
#include "ContentBrowserPanel.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <system_error>
#include "EditorTheme.h"
#include "EditorUI.h"
#include "UHE/AssetsManager/AssetImport.h"
#include "UHE/AssetsManager/VfsSystem.h"
#include "UHE/Audio/AudioEngine.h"
#include "UHE/Utils/PlatfromUtils.h"
#include "imgui.h"

namespace UHE
{
namespace
{

/// Below this panel width the folder tree is hidden: the grid needs the room more,
/// and the breadcrumbs and Up button still reach every folder.
constexpr float kTreeMinPanelWidth = 380.0f;
/// Default tree column width; the table remembers what the user drags it to.
constexpr float kTreeDefaultWidth = 150.0f;
/// Horizontal room a breadcrumb chevron takes between two segments.
constexpr float kChevronWidth = 12.0f;
/// Tag pills only read once the thumbnail is this large; smaller tiles get a colour strip.
constexpr float kTagMinThumbnail = 56.0f;

/// @brief ORs ImGui flags as unsigned.
///
/// ImGui's flag typedefs are int, and OR-ing signed ints is what bugprone-signed-bitwise rejects.
template <typename... Flags> int CombineFlags(Flags... flags)
{
    unsigned combined = 0;
    ((combined |= static_cast<unsigned>(flags)), ...);
    return static_cast<int>(combined);
}

std::string ToLower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::string ToUpper(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return value;
}

bool IsImage(const std::filesystem::path& path)
{
    const std::string ext = ToLower(path.extension().string());
    return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp" || ext == ".tga" || ext == ".hdr" ||
           ext == ".ktx2";
}

bool IsModel(const std::filesystem::path& path)
{
    const std::string ext = ToLower(path.extension().string());
    return ext == ".glb" || ext == ".gltf" || ext == ".fbx" || ext == ".obj";
}

std::string TypeLabel(const std::filesystem::directory_entry& entry)
{
    if (entry.is_directory())
        return "Folder";
    std::string ext = entry.path().extension().string();
    if (!ext.empty() && ext.front() == '.')
        ext.erase(ext.begin());
    return ext.empty() ? "File" : ToLower(ext) + " file";
}

std::string FormatBytes(std::uintmax_t bytes)
{
    const char* units[] = {"B", "KB", "MB", "GB"};
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < 3)
    {
        value /= 1024.0;
        unit++;
    }
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.1f %s", value, units[unit]);
    return buffer;
}

/// @brief A short label and colour that tell asset kinds apart at a glance.
struct TypeTag
{
    std::string Text;
    ImVec4 Color;
};

/// @brief Picks the tag for a file, or returns false for kinds that get none.
///
/// The colours are mid-saturation sRGB values (as a colour picker shows them) so the
/// same tag reads on both the dark and the light themes.
bool GetTypeTag(const std::filesystem::path& path, TypeTag& out)
{
    const std::string ext = ToLower(path.extension().string());
    if (ext.size() < 2)
        return false;
    std::string label = ToUpper(ext.substr(1));
    if (label.size() > 5)
        label.resize(5);

    if (ext == ".lua")
        out = {label, ImVec4(0.36f, 0.55f, 0.94f, 1.0f)};
    else if (IsModel(path))
        out = {label, ImVec4(0.91f, 0.54f, 0.23f, 1.0f)};
    else if (ext == ".unhuman")
        out = {"SCENE", ImVec4(0.25f, 0.71f, 0.56f, 1.0f)};
    else if (IsImage(path))
        out = {label, ImVec4(0.78f, 0.43f, 0.84f, 1.0f)};
    else if (ext == ".wav" || ext == ".ogg" || ext == ".mp3" || ext == ".flac")
        out = {label, ImVec4(0.85f, 0.66f, 0.23f, 1.0f)};
    else if (ext == ".slang" || ext == ".glsl" || ext == ".hlsl" || ext == ".spv")
        out = {label, ImVec4(0.23f, 0.70f, 0.77f, 1.0f)};
    else
        return false;
    return true;
}

/// @brief Dark text on light fills and white on dark ones (Rec. 709 luminance).
ImU32 ContrastingText(const ImVec4& fill)
{
    const float luminance = 0.2126f * fill.x + 0.7152f * fill.y + 0.0722f * fill.z;
    return ImGui::ColorConvertFloat4ToU32(luminance > 0.6f ? ImVec4(0.08f, 0.08f, 0.09f, 1.0f)
                                                           : ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
}

/// @brief The ellipsis glyph when the font has one, so a truncated name loses as few characters as possible.
const char* EllipsisText()
{
    return ImGui::GetFont()->IsGlyphInFont(0x2026) ? "\xE2\x80\xA6" : "...";
}

float TextWidth(const char* begin, const char* end)
{
    return ImGui::CalcTextSize(begin, end).x;
}

/// @brief End of the longest prefix of [begin, end) no wider than \p maxWidth.
///
/// Binary search keeps this to a handful of measurements per name; the result is moved
/// back onto a code point boundary so a UTF-8 file name is never cut inside a character.
const char* FitPrefix(const char* begin, const char* end, float maxWidth)
{
    std::size_t low = 0;
    std::size_t high = static_cast<std::size_t>(end - begin);
    while (low < high)
    {
        const std::size_t mid = (low + high + 1) / 2;
        if (TextWidth(begin, begin + mid) <= maxWidth)
            low = mid;
        else
            high = mid - 1;
    }
    const char* cut = begin + low;
    while (cut > begin && cut < end && (static_cast<unsigned int>(static_cast<unsigned char>(*cut)) & 0xC0U) == 0x80U)
        --cut;
    return cut;
}

/// @brief \p text cut to \p maxWidth with a trailing ellipsis, or unchanged when it fits.
std::string Ellipsize(const std::string& text, float maxWidth)
{
    const char* begin = text.c_str();
    const char* end = begin + text.size();
    if (TextWidth(begin, end) <= maxWidth)
        return text;
    const char* ellipsis = EllipsisText();
    const float room = std::max(0.0f, maxWidth - ImGui::CalcTextSize(ellipsis).x);
    return std::string(begin, FitPrefix(begin, end, room)) + ellipsis;
}

/// @brief Lays a file name out on at most two lines under a tile.
///
/// Wrapping by width broke "NewScript.lua" into "NewScript." and "lua". Here a name only
/// breaks at a word boundary inside its stem (space, '_', '-', a camelCase or letter-to-digit
/// step), so the extension stays with the word before it; a name with no such boundary is
/// shortened with an ellipsis on one line instead of being split mid-word.
/// @return The number of lines written to \p lines (1 or 2).
int LayoutTileName(const std::string& name, float maxWidth, std::array<std::string, 2>& lines)
{
    const char* begin = name.c_str();
    if (TextWidth(begin, begin + name.size()) <= maxWidth)
    {
        lines[0] = name;
        return 1;
    }

    std::size_t stemEnd = name.rfind('.');
    if (stemEnd == std::string::npos || stemEnd == 0)
        stemEnd = name.size();

    std::size_t best = 0;
    for (std::size_t i = 1; i < stemEnd; ++i)
    {
        const unsigned char prev = static_cast<unsigned char>(name[i - 1]);
        const unsigned char cur = static_cast<unsigned char>(name[i]);
        const bool boundary = prev == ' ' || prev == '_' || prev == '-' || (std::islower(prev) && std::isupper(cur)) ||
                              (std::isalpha(prev) && std::isdigit(cur));
        if (!boundary)
            continue;
        if (TextWidth(begin, begin + i) > maxWidth)
            break;
        best = i;
    }

    if (best == 0)
    {
        lines[0] = Ellipsize(name, maxWidth);
        return 1;
    }

    std::string first = name.substr(0, best);
    while (!first.empty() && first.back() == ' ')
        first.pop_back();
    lines[0] = first;
    lines[1] = Ellipsize(name.substr(best), maxWidth);
    return 2;
}

/// @brief True when \p ancestor contains \p path somewhere below it.
///
/// lexically_relative ignores a trailing separator on the base, which a project path may have.
bool IsStrictAncestor(const std::filesystem::path& ancestor, const std::filesystem::path& path)
{
    const std::filesystem::path relative = path.lexically_relative(ancestor);
    return !relative.empty() && relative != "." && *relative.begin() != "..";
}

ImTextureID ToTextureID(const Ref<Texture2D>& texture)
{
    return texture ? (ImTextureID)(intptr_t)texture->GetImGuiTextureID() : ImTextureID{};
}

/// @brief Offers a file to the hierarchy, inspector and viewport.
///
/// The payload type name is a contract: Editor.cpp and the inspector accept "Content_Browser_item".
void FileDragSource(const std::filesystem::path& path, const std::string& filename, ImTextureID icon)
{
    if (!ImGui::BeginDragDropSource())
        return;
    const std::string pathStr = path.string();
    ImGui::SetDragDropPayload("Content_Browser_item", pathStr.c_str(), pathStr.length() + 1, ImGuiCond_Once);
    if (icon)
    {
        const float size = ImGui::GetFrameHeight();
        ImGui::Image(icon, ImVec2(size, size), ImVec2(0, 1), ImVec2(1, 0));
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
    }
    ImGui::TextUnformatted(filename.c_str());
    ImGui::EndDragDropSource();
}

/// @brief Dim, centred text for an empty view; the panel says why it is empty instead of showing nothing.
void DrawCenteredHint(const char* title, const char* detail)
{
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const ImVec2 start = ImGui::GetCursorPos();
    const float lineHeight = ImGui::GetTextLineHeightWithSpacing();
    const float blockHeight = detail ? lineHeight * 2.0f : lineHeight;
    const float top = start.y + std::max(0.0f, (avail.y - blockHeight) * 0.5f);

    const auto centred = [&](const char* text, float y)
    {
        const float width = ImGui::CalcTextSize(text).x;
        ImGui::SetCursorPos(ImVec2(start.x + std::max(0.0f, (avail.x - width) * 0.5f), y));
        ImGui::TextDisabled("%s", text);
    };
    centred(title, top);
    if (detail)
        centred(detail, top + lineHeight);
}

/// @brief One half of the grid/list segmented toggle, with a drawn glyph (the font has no icons).
bool ViewModeButton(const char* id, bool gridGlyph, bool active, const char* tooltip, ImDrawFlags corners)
{
    const float size = ImGui::GetFrameHeight();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(size, size));
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();
    ImGui::SetItemTooltip("%s", tooltip);

    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 max(pos.x + size, pos.y + size);
    const float rounding = ImGui::GetStyle().FrameRounding;
    ImU32 background = ImGui::GetColorU32(ImGuiCol_Button);
    if (held)
        background = ImGui::GetColorU32(ImGuiCol_ButtonActive);
    else if (hovered)
        background = ImGui::GetColorU32(ImGuiCol_ButtonHovered);
    draw->AddRectFilled(pos, max, background, rounding, corners);
    if (active)
    {
        ImVec4 fill = EditorTheme::Accent();
        fill.w = 0.30f;
        draw->AddRectFilled(pos, max, ImGui::GetColorU32(fill), rounding, corners);
    }

    const ImU32 glyph = active ? ImGui::GetColorU32(EditorTheme::Accent()) : ImGui::GetColorU32(ImGuiCol_Text);
    const float box = std::floor(size * 0.44f);
    const float x0 = std::floor(pos.x + (size - box) * 0.5f);
    const float y0 = std::floor(pos.y + (size - box) * 0.5f);
    if (gridGlyph)
    {
        const float cell = std::floor((box - 2.0f) * 0.5f);
        for (int row = 0; row < 2; ++row)
            for (int col = 0; col < 2; ++col)
            {
                const ImVec2 min(x0 + static_cast<float>(col) * (cell + 2.0f),
                                 y0 + static_cast<float>(row) * (cell + 2.0f));
                draw->AddRectFilled(min, ImVec2(min.x + cell, min.y + cell), glyph, 1.0f);
            }
    }
    else
    {
        const float step = std::floor(box / 3.0f);
        for (int row = 0; row < 3; ++row)
        {
            const float y = y0 + static_cast<float>(row) * step + 1.0f;
            draw->AddRectFilled(ImVec2(x0, y), ImVec2(x0 + 2.0f, y + 2.0f), glyph);
            draw->AddRectFilled(ImVec2(x0 + 4.0f, y), ImVec2(x0 + box, y + 2.0f), glyph);
        }
    }
    return clicked;
}

/// @brief One breadcrumb segment: what it shows, where it leads, and its button width.
struct Crumb
{
    std::string Label;
    std::filesystem::path Path;
    float Width = 0.0f;
};

std::vector<Crumb> MakeCrumbs(const std::filesystem::path& base, const std::filesystem::path& current, float padX)
{
    std::vector<Crumb> crumbs;
    crumbs.push_back({"Assets", base, 0.0f});
    const std::filesystem::path relative = current.lexically_relative(base);
    std::filesystem::path accumulated = base;
    for (const auto& part : relative)
    {
        if (part.empty() || part == ".")
            continue;
        accumulated /= part;
        crumbs.push_back({part.string(), accumulated, 0.0f});
    }
    for (Crumb& crumb : crumbs)
        crumb.Width = ImGui::CalcTextSize(crumb.Label.c_str()).x + padX * 2.0f;
    return crumbs;
}

float CrumbsWidth(const std::vector<Crumb>& crumbs)
{
    float width = 0.0f;
    for (const Crumb& crumb : crumbs)
        width += crumb.Width;
    return width + kChevronWidth * static_cast<float>(crumbs.size() - 1);
}

void Chevron()
{
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float height = ImGui::GetFrameHeight();
    ImGui::Dummy(ImVec2(kChevronWidth, height));
    const float cx = pos.x + kChevronWidth * 0.5f;
    const float cy = pos.y + height * 0.5f;
    const ImVec2 points[3] = {ImVec2(cx - 1.5f, cy - 3.5f), ImVec2(cx + 2.0f, cy), ImVec2(cx - 1.5f, cy + 3.5f)};
    ImGui::GetWindowDrawList()->AddPolyline(points, 3, ImGui::GetColorU32(ImGuiCol_TextDisabled), ImDrawFlags_None,
                                            1.5f);
}

/// @brief Draws the path as flat clickable segments within \p maxWidth.
///
/// When the path is too long the leading folders fold into a "..." button whose popup
/// lists them, so the folder being browsed is always the one left visible.
/// @return True when a segment was clicked; \p target is then the folder to open.
bool DrawBreadcrumbBar(const std::vector<Crumb>& crumbs, float maxWidth, float padX, std::filesystem::path& target)
{
    const ImGuiStyle& style = ImGui::GetStyle();
    const int count = static_cast<int>(crumbs.size());
    const float overflowWidth = ImGui::CalcTextSize("...").x + padX * 2.0f;

    int first = count - 1;
    float used = crumbs.back().Width;
    while (first > 0)
    {
        const float withPrevious = used + kChevronWidth + crumbs[first - 1].Width;
        const float overflowCost = (first - 1 > 0) ? overflowWidth + kChevronWidth : 0.0f;
        if (withPrevious + overflowCost > maxWidth)
            break;
        used = withPrevious;
        --first;
    }

    bool navigated = false;
    ImGui::PushID("Breadcrumbs");
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(padX, style.FramePadding.y));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, style.ItemSpacing.y));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));

    float lead = 0.0f;
    if (first > 0)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        if (ImGui::Button("..."))
            ImGui::OpenPopup("HiddenCrumbs");
        ImGui::PopStyleColor();
        ImGui::SetItemTooltip("Parent folders");
        if (ImGui::BeginPopup("HiddenCrumbs"))
        {
            for (int i = 0; i < first; ++i)
            {
                ImGui::PushID(i);
                if (ImGui::Selectable(crumbs[i].Label.c_str()))
                {
                    target = crumbs[i].Path;
                    navigated = true;
                }
                ImGui::PopID();
            }
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        Chevron();
        ImGui::SameLine();
        lead = overflowWidth + kChevronWidth;
    }

    for (int i = first; i < count; ++i)
    {
        const bool last = i == count - 1;
        std::string label = crumbs[i].Label;
        if (last)
            label = Ellipsize(label, maxWidth - lead - padX * 2.0f);
        else
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));

        ImGui::PushID(i);
        if (ImGui::Button(label.c_str()))
        {
            target = crumbs[i].Path;
            navigated = true;
        }
        ImGui::PopID();
        if (label != crumbs[i].Label)
            ImGui::SetItemTooltip("%s", crumbs[i].Label.c_str());

        if (!last)
        {
            ImGui::PopStyleColor();
            ImGui::SameLine();
            Chevron();
            ImGui::SameLine();
            lead += crumbs[i].Width + kChevronWidth;
        }
    }

    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
    ImGui::PopID();
    return navigated;
}

} // namespace

ContentBrowserPanel::ContentBrowserPanel()
{
    m_BaseDirectory = FileSystem::Get().GetRootPath() / "assets";
    m_CurrentDirectory = m_BaseDirectory;
    auto rootPath = FileSystem::Get().GetRootPath();
    m_DirectoryIcon = Texture2D::Create((rootPath / "assets/icon/folder.png").string());
    m_FileIcon = Texture2D::Create((rootPath / "assets/icon/file.png").string());
    m_PngIcon = Texture2D::Create((rootPath / "assets/icon/png.png").string());
    m_modelPng = Texture2D::Create((rootPath / "assets/icon/3dmodel.png").string());
}

void ContentBrowserPanel::SetBaseDirectory(const std::filesystem::path& directory)
{
    // Icons stay engine-owned; only the browsed tree moves into the project.
    m_BaseDirectory = directory;
    m_CurrentDirectory = directory;
    m_SelectedPath.clear();
}

std::vector<std::filesystem::path> ContentBrowserPanel::ImportFiles(const std::vector<std::string>& sources)
{
    std::vector<std::filesystem::path> imported;
    imported.reserve(sources.size());

    for (const std::string& source : sources)
    {
        std::string error;
        const std::filesystem::path destination = ImportAssetFile(source, m_CurrentDirectory, &error);
        if (destination.empty())
        {
            UHE_CORE_WARN("Import failed for '{0}': {1}", source, error);
            continue;
        }
        imported.push_back(destination);
    }

    if (!imported.empty())
    {
        // Drop cached previews: the folder gained files.
        Refresh();
        m_SelectedPath = imported.front();
    }
    return imported;
}

void ContentBrowserPanel::ImportFromDialog()
{
    // One filter string covering everything the editor can reference today.
    const std::string file = FileDialogs::OpenFile(
        "Assets (*.png;*.jpg;*.jpeg;*.bmp;*.tga;*.hdr;*.ktx2;*.glb;*.gltf;*.fbx;*.obj;*.unhuman)\0"
        "*.png;*.jpg;*.jpeg;*.bmp;*.tga;*.hdr;*.ktx2;*.glb;*.gltf;*.fbx;*.obj;*.unhuman\0"
        "All files (*.*)\0*.*\0",
        m_CurrentDirectory.string());
    if (file.empty())
        return;

    ImportFiles({file});
}

Ref<Texture2D> ContentBrowserPanel::IconFor(const std::filesystem::path& path, bool isDirectory, bool allowPreview)
{
    if (isDirectory)
        return m_DirectoryIcon;
    if (IsImage(path))
    {
        if (allowPreview)
        {
            const std::string absolutePath = path.string();
            auto it = m_TextureCache.find(absolutePath);
            if (it == m_TextureCache.end())
                it = m_TextureCache.emplace(absolutePath, Texture2D::Create(absolutePath)).first;
            if (it->second && it->second->GetImGuiTextureID())
                return it->second;
        }
        return m_PngIcon;
    }
    if (IsModel(path))
        return m_modelPng;
    return m_FileIcon;
}

void ContentBrowserPanel::DrawFolderTree()
{
    // A pending reveal opens every folder on the way to the browsed one, so navigating
    // from the grid or the breadcrumbs keeps the tree showing where the user is.
    const bool reveal = m_TreeRevealedDirectory != m_CurrentDirectory;
    ImGui::PushStyleVar(ImGuiStyleVar_IndentSpacing, 12.0f);

    ImGuiTreeNodeFlags rootFlags =
        CombineFlags(ImGuiTreeNodeFlags_OpenOnArrow, ImGuiTreeNodeFlags_SpanAvailWidth, ImGuiTreeNodeFlags_DefaultOpen);
    if (m_CurrentDirectory == m_BaseDirectory)
        rootFlags = CombineFlags(rootFlags, ImGuiTreeNodeFlags_Selected);
    if (reveal && IsStrictAncestor(m_BaseDirectory, m_CurrentDirectory))
        ImGui::SetNextItemOpen(true);

    const std::string baseDirString = m_BaseDirectory.string();
    const bool rootOpened = ImGui::TreeNodeEx(baseDirString.c_str(), rootFlags, "Assets");
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
        m_CurrentDirectory = m_BaseDirectory;
    if (rootOpened)
    {
        DrawDirectoryTree(m_BaseDirectory);
        ImGui::TreePop();
    }

    ImGui::PopStyleVar();
    if (reveal)
        m_TreeRevealedDirectory = m_CurrentDirectory;
}

void ContentBrowserPanel::DrawDirectoryTree(const std::filesystem::path& directoryPath)
{
    // Sorted like the grid; directory_iterator order is whatever the file system returns.
    std::vector<std::filesystem::path> folders;
    std::error_code ec;
    for (const auto& directoryEntry : std::filesystem::directory_iterator(
             directoryPath, std::filesystem::directory_options::skip_permission_denied, ec))
    {
        if (directoryEntry.is_directory())
            folders.push_back(directoryEntry.path());
    }
    std::sort(folders.begin(), folders.end(), [](const std::filesystem::path& a, const std::filesystem::path& b)
              { return ToLower(a.filename().string()) < ToLower(b.filename().string()); });

    const bool reveal = m_TreeRevealedDirectory != m_CurrentDirectory;
    for (const auto& path : folders)
    {
        std::string pathString = path.string();
        std::string filenameString = path.filename().string();
        ImGuiTreeNodeFlags flags = CombineFlags(ImGuiTreeNodeFlags_OpenOnArrow, ImGuiTreeNodeFlags_OpenOnDoubleClick,
                                                ImGuiTreeNodeFlags_SpanAvailWidth);
        const bool isCurrent = m_CurrentDirectory == path;
        if (isCurrent)
            flags = CombineFlags(flags, ImGuiTreeNodeFlags_Selected);
        if (reveal && IsStrictAncestor(path, m_CurrentDirectory))
            ImGui::SetNextItemOpen(true);

        bool opened = ImGui::TreeNodeEx(pathString.c_str(), flags, "%s", filenameString.c_str());
        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
            m_CurrentDirectory = path;
        if (reveal && isCurrent && !ImGui::IsItemVisible())
            ImGui::SetScrollHereY(0.5f);
        if (opened)
        {
            DrawDirectoryTree(path);
            ImGui::TreePop();
        }
    }
}

void ContentBrowserPanel::DrawToolbar()
{
    const ImGuiStyle& style = ImGui::GetStyle();
    const float frameHeight = ImGui::GetFrameHeight();
    const float gap = 4.0f;
    const float crumbPadX = 4.0f;
    const float rowX = ImGui::GetCursorScreenPos().x;
    const float rowWidth = ImGui::GetContentRegionAvail().x;

    const char* importLabel = "Import";
    const char* scriptLabel = "+ Script";
    const float importWidth = ImGui::CalcTextSize(importLabel).x + style.FramePadding.x * 2.0f;
    const float scriptWidth = ImGui::CalcTextSize(scriptLabel).x + style.FramePadding.x * 2.0f;
    // Segmented grid/list toggle (two square halves), then the two text buttons.
    const float rightWidth = frameHeight * 2.0f + gap + importWidth + gap + scriptWidth;
    const float minSearchWidth = 100.0f;
    const float maxSearchWidth = 240.0f;
    const float minCrumbWidth = 110.0f;

    const std::vector<Crumb> crumbs = MakeCrumbs(m_BaseDirectory, m_CurrentDirectory, crumbPadX);
    const float crumbsNatural = CrumbsWidth(crumbs);

    // Up | crumbs | search | buttons on one row when the search keeps a usable width;
    // a narrow dock gets the path on its own row instead of a row that wraps by itself.
    const float freeWidth = rowWidth - frameHeight - rightWidth - gap * 3.0f;
    const bool oneRow = freeWidth >= minCrumbWidth + minSearchWidth;

    const bool atRoot = m_CurrentDirectory == m_BaseDirectory;
    ImGui::BeginDisabled(atRoot);
    if (ImGui::ArrowButton("##Up", ImGuiDir_Up))
        m_CurrentDirectory = m_CurrentDirectory.parent_path();
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Parent folder");
    ImGui::SameLine(0.0f, gap);

    std::filesystem::path target;
    float searchWidth = 0.0f;
    if (oneRow)
    {
        searchWidth = std::clamp(freeWidth - crumbsNatural, minSearchWidth, maxSearchWidth);
        if (DrawBreadcrumbBar(crumbs, freeWidth - searchWidth, crumbPadX, target))
            m_CurrentDirectory = target;
        // Right-align the search and buttons so they do not shift as the path changes length.
        ImGui::SameLine();
        ImGui::SetCursorScreenPos(
            ImVec2(rowX + rowWidth - rightWidth - gap - searchWidth, ImGui::GetCursorScreenPos().y));
    }
    else
    {
        if (DrawBreadcrumbBar(crumbs, rowWidth - frameHeight - gap, crumbPadX, target))
            m_CurrentDirectory = target;
        searchWidth = std::max(60.0f, rowWidth - rightWidth - gap);
    }

    EditorUI::SearchField("##ContentSearch", m_SearchBuffer, sizeof(m_SearchBuffer), "Search assets...", searchWidth);
    ImGui::SameLine(0.0f, gap);

    if (ViewModeButton("##GridView", true, !m_ListMode, "Grid view", ImDrawFlags_RoundCornersLeft))
        m_ListMode = false;
    ImGui::SameLine(0.0f, 0.0f);
    if (ViewModeButton("##ListView", false, m_ListMode, "List view", ImDrawFlags_RoundCornersRight))
        m_ListMode = true;
    ImGui::SameLine(0.0f, gap);

    if (ImGui::Button(importLabel, ImVec2(importWidth, 0.0f)))
        ImportFromDialog();
    ImGui::SetItemTooltip("Copy a file from outside the project into this folder");
    ImGui::SameLine(0.0f, gap);
    if (ImGui::Button(scriptLabel, ImVec2(scriptWidth, 0.0f)))
        CreateScript();
    ImGui::SetItemTooltip("Creates a .lua with the OnCreate/OnUpdate/OnDestroy hooks in this folder.");
}

void ContentBrowserPanel::DrawGrid(const std::vector<std::filesystem::directory_entry>& entries)
{
    const float padding = 6.0f;
    const float gap = 4.0f;
    const float tileWidth = m_ThumbnailSize + padding * 2.0f;
    // Two name lines are always reserved so every tile, and so every row, has one height.
    const float tileHeight = padding + m_ThumbnailSize + 4.0f + ImGui::GetTextLineHeight() * 2.0f + padding;
    const float panelWidth = ImGui::GetContentRegionAvail().x;
    const int columnCount = std::max(1, static_cast<int>((panelWidth + gap) / (tileWidth + gap)));
    const int rowCount = (static_cast<int>(entries.size()) + columnCount - 1) / columnCount;

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, gap));
    // Rows off screen are skipped, so a large folder neither draws nor loads their previews.
    ImGuiListClipper clipper;
    clipper.Begin(rowCount, tileHeight + gap);
    while (clipper.Step())
    {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row)
        {
            for (int column = 0; column < columnCount; ++column)
            {
                const std::size_t index = static_cast<std::size_t>(row) * static_cast<std::size_t>(columnCount) +
                                          static_cast<std::size_t>(column);
                if (index >= entries.size())
                    break;
                if (column > 0)
                    ImGui::SameLine();
                DrawGridTile(entries[index], tileWidth, tileHeight, padding);
            }
        }
    }
    ImGui::PopStyleVar();
}

void ContentBrowserPanel::DrawGridTile(const std::filesystem::directory_entry& entry, float tileWidth, float tileHeight,
                                       float padding)
{
    const auto& path = entry.path();
    const std::string filenameString = path.filename().string();
    const bool isDirectory = entry.is_directory();
    const bool selected = (m_SelectedPath == path);
    const float thumbnail = m_ThumbnailSize;

    ImGui::PushID(filenameString.c_str());

    const ImVec2 tileMin = ImGui::GetCursorScreenPos();
    const ImVec2 tileMax(tileMin.x + tileWidth, tileMin.y + tileHeight);
    // One item covers the whole tile, so clicks, drags and the context menu work on the name too.
    if (ImGui::InvisibleButton("##tile", ImVec2(tileWidth, tileHeight)))
        m_SelectedPath = path;
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
        m_SelectedPath = path;
    if (hovered && isDirectory && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
        m_CurrentDirectory = path;

    const Ref<Texture2D> icon = IconFor(path, isDirectory, true);
    const ImTextureID iconTexture = ToTextureID(icon);

    if (!isDirectory)
        FileDragSource(path, filenameString, iconTexture);
    // The drag preview is a tooltip too; a hover tooltip would replace it.
    if (!ImGui::GetDragDropPayload())
        ImGui::SetItemTooltip("%s\n%s", filenameString.c_str(), path.string().c_str());
    if (!isDirectory)
        DrawFileContextMenu(path);

    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float rounding = std::max(4.0f, ImGui::GetStyle().FrameRounding);
    if (selected)
    {
        ImVec4 fill = EditorTheme::Accent();
        ImVec4 border = fill;
        fill.w = 0.28f;
        border.w = 0.75f;
        draw->AddRectFilled(tileMin, tileMax, ImGui::GetColorU32(fill), rounding);
        draw->AddRect(tileMin, tileMax, ImGui::GetColorU32(border), rounding, ImDrawFlags_None, 1.0f);
    }
    else if (hovered || held)
    {
        draw->AddRectFilled(tileMin, tileMax, ImGui::GetColorU32(held ? ImGuiCol_ButtonActive : ImGuiCol_ButtonHovered),
                            rounding);
    }

    const ImVec2 iconMin(tileMin.x + padding, tileMin.y + padding);
    const ImVec2 iconMax(iconMin.x + thumbnail, iconMin.y + thumbnail);
    if (iconTexture)
    {
        // Previews keep their aspect ratio inside the square; the bundled icons are square anyway.
        ImVec2 imageMin = iconMin;
        ImVec2 imageMax = iconMax;
        const float width = static_cast<float>(icon->GetWidth());
        const float height = static_cast<float>(icon->GetHeight());
        if (width > 0.0f && height > 0.0f && width != height)
        {
            const float scale = thumbnail / std::max(width, height);
            const ImVec2 size(width * scale, height * scale);
            imageMin = ImVec2(iconMin.x + (thumbnail - size.x) * 0.5f, iconMin.y + (thumbnail - size.y) * 0.5f);
            imageMax = ImVec2(imageMin.x + size.x, imageMin.y + size.y);
        }
        draw->AddImage(iconTexture, imageMin, imageMax, ImVec2(0, 1), ImVec2(1, 0));
        const bool isPreview = !isDirectory && IsImage(path) && icon != m_PngIcon;
        if (isPreview)
            draw->AddRect(imageMin, imageMax, ImGui::GetColorU32(ImGuiCol_Border));
    }
    else
    {
        // The icon bitmap failed to load: a plain outline still marks the tile's slot.
        draw->AddRect(ImVec2(iconMin.x + thumbnail * 0.2f, iconMin.y + thumbnail * 0.1f),
                      ImVec2(iconMax.x - thumbnail * 0.2f, iconMax.y - thumbnail * 0.1f),
                      ImGui::GetColorU32(ImGuiCol_TextDisabled), 3.0f, ImDrawFlags_None, 1.5f);
    }

    TypeTag tag;
    if (!isDirectory && GetTypeTag(path, tag))
    {
        if (thumbnail >= kTagMinThumbnail)
        {
            // A smaller size of the UI font; the dynamic atlas bakes it on first use.
            ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 0.72f);
            const ImVec2 textSize = ImGui::CalcTextSize(tag.Text.c_str());
            const ImVec2 pillMax(iconMax.x, iconMax.y);
            const ImVec2 pillMin(pillMax.x - textSize.x - 8.0f, pillMax.y - textSize.y - 2.0f);
            draw->AddRectFilled(pillMin, pillMax, ImGui::GetColorU32(tag.Color), 3.0f);
            draw->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(pillMin.x + 4.0f, pillMin.y + 1.0f),
                          ContrastingText(tag.Color), tag.Text.c_str());
            ImGui::PopFont();
        }
        else
        {
            draw->AddRectFilled(ImVec2(iconMin.x, iconMax.y + 1.0f), ImVec2(iconMax.x, iconMax.y + 3.0f),
                                ImGui::GetColorU32(tag.Color), 1.0f);
        }
    }

    std::array<std::string, 2> lines;
    const int lineCount = LayoutTileName(filenameString, tileWidth - 4.0f, lines);
    const float lineHeight = ImGui::GetTextLineHeight();
    const ImU32 textColor = ImGui::GetColorU32(ImGuiCol_Text);
    for (int i = 0; i < lineCount; ++i)
    {
        const float width = ImGui::CalcTextSize(lines[i].c_str()).x;
        const ImVec2 textPos(std::floor(tileMin.x + (tileWidth - width) * 0.5f),
                             iconMax.y + 4.0f + lineHeight * static_cast<float>(i));
        draw->AddText(textPos, textColor, lines[i].c_str());
    }

    ImGui::PopID();
}

void ContentBrowserPanel::DrawList(const std::vector<std::filesystem::directory_entry>& entries)
{
    if (!ImGui::BeginTable("ContentList", 3,
                           CombineFlags(ImGuiTableFlags_RowBg, ImGuiTableFlags_BordersInnerV, ImGuiTableFlags_ScrollY)))
    {
        return;
    }

    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 90.0f);
    ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 80.0f);
    ImGui::TableHeadersRow();

    const float iconSize = ImGui::GetTextLineHeight();
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(entries.size()));
    while (clipper.Step())
    {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row)
        {
            const auto& entry = entries[static_cast<std::size_t>(row)];
            const auto& path = entry.path();
            const std::string filenameString = path.filename().string();
            const bool isDirectory = entry.is_directory();
            const bool selected = (m_SelectedPath == path);

            ImGui::PushID(filenameString.c_str());
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);

            // Generic icons only: loading every image's preview for a one-line row is not worth it.
            const ImTextureID icon = ToTextureID(IconFor(path, isDirectory, false));
            if (icon)
            {
                ImGui::Image(icon, ImVec2(iconSize, iconSize), ImVec2(0, 1), ImVec2(1, 0));
                ImGui::SameLine(0.0f, 6.0f);
            }

            if (ImGui::Selectable(
                    filenameString.c_str(), selected,
                    CombineFlags(ImGuiSelectableFlags_SpanAllColumns, ImGuiSelectableFlags_AllowDoubleClick)))
            {
                m_SelectedPath = path;
                if (isDirectory && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                    m_CurrentDirectory = path;
            }
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
                m_SelectedPath = path;

            if (!isDirectory)
                FileDragSource(path, filenameString, icon);
            if (!ImGui::GetDragDropPayload())
                ImGui::SetItemTooltip("%s", path.string().c_str());
            if (!isDirectory)
                DrawFileContextMenu(path);

            ImGui::TableSetColumnIndex(1);
            TypeTag tag;
            if (!isDirectory && GetTypeTag(path, tag))
            {
                // The same colour as the grid's tag, as a dot, so the two views agree.
                const ImVec2 pos = ImGui::GetCursorScreenPos();
                const float radius = 3.0f;
                ImGui::GetWindowDrawList()->AddCircleFilled(
                    ImVec2(pos.x + radius, pos.y + ImGui::GetTextLineHeight() * 0.5f), radius,
                    ImGui::GetColorU32(tag.Color));
                ImGui::Dummy(ImVec2(radius * 2.0f, ImGui::GetTextLineHeight()));
                ImGui::SameLine(0.0f, 5.0f);
            }
            ImGui::TextDisabled("%s", TypeLabel(entry).c_str());

            ImGui::TableSetColumnIndex(2);
            if (isDirectory)
            {
                ImGui::TextDisabled("-");
            }
            else
            {
                std::error_code sizeError;
                const auto size = entry.file_size(sizeError);
                if (sizeError)
                    ImGui::TextDisabled("?");
                else
                    ImGui::TextDisabled("%s", FormatBytes(size).c_str());
            }

            ImGui::PopID();
        }
    }

    ImGui::EndTable();
}

void ContentBrowserPanel::DrawFileContextMenu(const std::filesystem::path& path)
{
    if (!ImGui::BeginPopupContextItem())
        return;

    const bool isScript = path.extension() == ".lua";
    if (isScript)
    {
        if (ImGui::MenuItem("Attach to selected entity"))
        {
            if (m_OnAttachScript)
                m_OnAttachScript(path);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Adds (or retargets) a Script component on the entity selected in the hierarchy.");
        if (ImGui::MenuItem("Open in editor"))
        {
            FileDialogs::OpenInDefaultApp(path.string());
        }
        ImGui::Separator();
    }

    const std::string extension = ToLower(path.extension().string());
    const bool isAudio = extension == ".wav" || extension == ".ogg";
    if (isAudio)
    {
        if (ImGui::MenuItem("Play (2D)"))
        {
            Audio::SoundOptions options;
            Audio::AudioEngine::Play(path.string(), options);
        }
        if (ImGui::MenuItem("Play (3D at origin)"))
        {
            Audio::SoundOptions options;
            options.Spatial = true;
            Audio::AudioEngine::Play(path.string(), options);
        }
        ImGui::Separator();
    }

    if (ImGui::MenuItem("Reveal path"))
    {
        ImGui::SetClipboardText(path.string().c_str());
    }
    ImGui::EndPopup();
}

void ContentBrowserPanel::CreateScript()
{
    // A starter file with the three hooks, under a name that is not taken.
    static constexpr const char* kTemplate = "-- Attached to an entity with a Script component.\n"
                                             "-- The engine runs these three; `self` is the entity.\n"
                                             "\n"
                                             "function OnCreate(self)\n"
                                             "    -- once, before the first update\n"
                                             "end\n"
                                             "\n"
                                             "function OnUpdate(self, dt)\n"
                                             "    -- every step; dt is the frame's seconds\n"
                                             "end\n"
                                             "\n"
                                             "function OnDestroy(self)\n"
                                             "    -- when the entity goes away\n"
                                             "end\n";

    std::error_code ec;
    std::filesystem::path target = m_CurrentDirectory / "NewScript.lua";
    for (int index = 1; std::filesystem::exists(target, ec) && index < 1000; ++index)
    {
        target = m_CurrentDirectory / ("NewScript" + std::to_string(index) + ".lua");
    }

    std::ofstream file(target, std::ios::trunc);
    if (!file)
    {
        UHE_CORE_WARN("Could not create '{0}'", target.string());
        return;
    }
    file << kTemplate;
    file.close();

    m_SelectedPath = target;
    m_TextureCache.clear();
    UHE_CORE_INFO("Created script '{0}'", target.string());
}

void ContentBrowserPanel::DrawFooter(std::size_t itemCount, float width)
{
    const ImVec2 rowMin = ImGui::GetCursorScreenPos();
    const float gap = 8.0f;
    // A hairline instead of ImGui::Separator: it costs no vertical room the item view could use.
    ImGui::GetWindowDrawList()->AddLine(ImVec2(rowMin.x, rowMin.y - 2.0f), ImVec2(rowMin.x + width, rowMin.y - 2.0f),
                                        ImGui::GetColorU32(ImGuiCol_Separator));

    // Unity keeps the icon size slider bottom-right; it stays off the toolbar so that row never wraps.
    const bool showSlider = !m_ListMode && width >= 200.0f;
    const float sliderWidth = showSlider ? std::min(110.0f, width * 0.35f) : 0.0f;

    char countText[32];
    std::snprintf(countText, sizeof(countText), itemCount == 1 ? "%zu item" : "%zu items", itemCount);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", countText);

    if (!m_SelectedPath.empty() && m_SelectedPath.parent_path() == m_CurrentDirectory)
    {
        const float room = width - ImGui::CalcTextSize(countText).x - sliderWidth - gap * 3.0f;
        if (room > 40.0f)
        {
            ImGui::SameLine(0.0f, gap);
            ImGui::TextUnformatted(Ellipsize(m_SelectedPath.filename().string(), room).c_str());
        }
    }

    if (showSlider)
    {
        ImGui::SameLine();
        ImGui::SetCursorScreenPos(ImVec2(rowMin.x + width - sliderWidth, ImGui::GetCursorScreenPos().y));
        ImGui::SetNextItemWidth(sliderWidth);
        ImGui::SliderFloat("##ThumbSize", &m_ThumbnailSize, 40.0f, 128.0f, "");
        ImGui::SetItemTooltip("Icon size: %.0f px", m_ThumbnailSize);
    }
}

void ContentBrowserPanel::DrawContentArea()
{
    DrawToolbar();

    // Collect, filter and sort entries once per frame.
    const std::string needle = ToLower(m_SearchBuffer);
    std::vector<std::filesystem::directory_entry> entries;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(
             m_CurrentDirectory, std::filesystem::directory_options::skip_permission_denied, ec))
    {
        if (!needle.empty() && ToLower(entry.path().filename().string()).find(needle) == std::string::npos)
            continue;
        entries.push_back(entry);
    }
    std::sort(entries.begin(), entries.end(),
              [](const std::filesystem::directory_entry& a, const std::filesystem::directory_entry& b)
              {
                  const bool aDir = a.is_directory();
                  const bool bDir = b.is_directory();
                  if (aDir != bDir)
                      return aDir;
                  return ToLower(a.path().filename().string()) < ToLower(b.path().filename().string());
              });

    const float width = ImGui::GetContentRegionAvail().x;
    const float footerHeight = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.y;

    // The grid gets a small inset so the first tile's hover background does not touch the edge;
    // the list's table already has its own cell padding.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4.0f, 4.0f));
    ImGui::BeginChild("ContentGrid", ImVec2(0.0f, -footerHeight),
                      m_ListMode ? ImGuiChildFlags_None : ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleVar();

    if (entries.empty())
    {
        if (!needle.empty())
            DrawCenteredHint("No matching assets", "Nothing in this folder matches the search.");
        else
            DrawCenteredHint("This folder is empty", "Drop files here or use Import.");
    }
    else if (m_ListMode)
    {
        DrawList(entries);
    }
    else
    {
        DrawGrid(entries);
    }

    // Child windows count too: in list mode the table scrolls in a child of its own.
    const bool backgroundHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && !ImGui::IsAnyItemHovered();
    if (backgroundHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        m_SelectedPath.clear();
    if (backgroundHovered && ImGui::IsMouseReleased(ImGuiMouseButton_Right))
        ImGui::OpenPopup("ContentBackgroundMenu");
    if (ImGui::BeginPopup("ContentBackgroundMenu"))
    {
        if (ImGui::MenuItem("New Script"))
            CreateScript();
        if (ImGui::MenuItem("Import..."))
            ImportFromDialog();
        ImGui::Separator();
        if (ImGui::MenuItem("Copy folder path"))
            ImGui::SetClipboardText(m_CurrentDirectory.string().c_str());
        ImGui::EndPopup();
    }
    ImGui::EndChild();

    DrawFooter(entries.size(), width);
}

void ContentBrowserPanel::OnImGuiRender()
{
    UHE_PROFILE_FUNCTION();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6, 6));
    // The tree and the item view scroll on their own; the window itself never needs to.
    const bool visible = ImGui::Begin("Content Browser", nullptr,
                                      CombineFlags(ImGuiWindowFlags_NoScrollbar, ImGuiWindowFlags_NoScrollWithMouse));
    ImGui::PopStyleVar();
    if (!visible)
    {
        // Docked behind another tab: skip the per-frame directory scans.
        ImGui::End();
        return;
    }

    std::error_code ec;
    if (!std::filesystem::is_directory(m_BaseDirectory, ec))
    {
        DrawCenteredHint("No project assets", "Open or create a project to browse its assets.");
        ImGui::End();
        return;
    }
    // The browsed folder can be deleted or renamed outside the editor.
    if (!std::filesystem::is_directory(m_CurrentDirectory, ec))
        m_CurrentDirectory = m_BaseDirectory;

    const bool showTree = ImGui::GetContentRegionAvail().x >= kTreeMinPanelWidth;
    if (!showTree)
    {
        DrawContentArea();
    }
    else if (ImGui::BeginTable("ContentBrowserLayout_v3", 2,
                               CombineFlags(ImGuiTableFlags_Resizable, ImGuiTableFlags_BordersInnerV),
                               ImVec2(0.0f, -FLT_MIN)))
    {
        ImGui::TableSetupColumn("Tree", ImGuiTableColumnFlags_WidthFixed, kTreeDefaultWidth);
        ImGui::TableSetupColumn("Content", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableNextRow();

        ImGui::TableSetColumnIndex(0);
        ImGui::BeginChild("FolderTree");
        DrawFolderTree();
        ImGui::EndChild();

        ImGui::TableSetColumnIndex(1);
        DrawContentArea();

        ImGui::EndTable();
    }

    ImGui::End();
}
} // namespace UHE
