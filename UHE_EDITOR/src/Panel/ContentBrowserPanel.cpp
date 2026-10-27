#include "ContentBrowserPanel.h"
#include "EditorTheme.h"
#include "EditorUI.h"
#include "UHE/AssestsManager/VfsSystem.h"
#include "imgui.h"
#include "uhepch.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <system_error>

namespace UHE {
namespace {

std::string ToLower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

bool IsImage(const std::filesystem::path &path) {
  const std::string ext = ToLower(path.extension().string());
  return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp" ||
         ext == ".tga" || ext == ".hdr" || ext == ".ktx2";
}

bool IsModel(const std::filesystem::path &path) {
  const std::string ext = ToLower(path.extension().string());
  return ext == ".glb" || ext == ".gltf" || ext == ".fbx" || ext == ".obj";
}

std::string TypeLabel(const std::filesystem::directory_entry &entry) {
  if (entry.is_directory())
    return "Folder";
  std::string ext = entry.path().extension().string();
  if (!ext.empty() && ext.front() == '.')
    ext.erase(ext.begin());
  return ext.empty() ? "File" : ToLower(ext) + " file";
}

std::string FormatBytes(std::uintmax_t bytes) {
  const char *units[] = {"B", "KB", "MB", "GB"};
  double value = static_cast<double>(bytes);
  int unit = 0;
  while (value >= 1024.0 && unit < 3) {
    value /= 1024.0;
    unit++;
  }
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "%.1f %s", value, units[unit]);
  return buffer;
}

} // namespace

ContentBrowserPanel::ContentBrowserPanel() {
  m_BaseDirectory = FileSystem::Get().GetRootPath() / "assets";
  m_CurrentDirectory = m_BaseDirectory;
  auto rootPath = FileSystem::Get().GetRootPath();
  m_DirectoryIcon = Texture2D::Create((rootPath / "assets/icon/folder.png").string());
  m_FileIcon = Texture2D::Create((rootPath / "assets/icon/file.png").string());
  m_PngIcon = Texture2D::Create((rootPath / "assets/icon/png.png").string());
  m_modelPng = Texture2D::Create((rootPath / "assets/icon/3dmodel.png").string());
}

void ContentBrowserPanel::DrawDirectoryTree(
    const std::filesystem::path &directoryPath) {
  std::error_code ec;
  for (const auto &directoryEntry : std::filesystem::directory_iterator(
           directoryPath, std::filesystem::directory_options::skip_permission_denied, ec)) {
    if (!directoryEntry.is_directory())
      continue;
    const auto &path = directoryEntry.path();
    std::string pathString = path.string();
    std::string filenameString = path.filename().string();
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow |
                               ImGuiTreeNodeFlags_OpenOnDoubleClick |
                               ImGuiTreeNodeFlags_SpanAvailWidth;
    if (m_CurrentDirectory == path) {
      flags |= ImGuiTreeNodeFlags_Selected;
    }
    bool opened = ImGui::TreeNodeEx(pathString.c_str(), flags, "%s",
                                    filenameString.c_str());
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
      m_CurrentDirectory = path;
    }
    if (opened) {
      DrawDirectoryTree(path);
      ImGui::TreePop();
    }
  }
}

void ContentBrowserPanel::DrawBreadcrumbs() {
  if (ImGui::SmallButton("Assets"))
    m_CurrentDirectory = m_BaseDirectory;

  const std::filesystem::path relative = m_CurrentDirectory.lexically_relative(m_BaseDirectory);
  std::filesystem::path accumulated = m_BaseDirectory;
  for (const auto &part : relative) {
    if (part.empty() || part == ".")
      continue;
    accumulated /= part;
    ImGui::SameLine(0.0f, 2.0f);
    ImGui::TextDisabled("/");
    ImGui::SameLine(0.0f, 2.0f);
    if (ImGui::SmallButton(part.string().c_str()))
      m_CurrentDirectory = accumulated;
  }
}

void ContentBrowserPanel::DrawGrid(
    const std::vector<std::filesystem::directory_entry> &entries) {
  const float padding = 14.0f;
  const float cellSize = m_ThumbnailSize + padding;
  const float panelWidth = ImGui::GetContentRegionAvail().x;
  int columnCount = static_cast<int>(panelWidth / cellSize);
  if (columnCount < 1)
    columnCount = 1;

  ImGui::Columns(columnCount, nullptr, false);
  const ImVec4 transparent(0.0f, 0.0f, 0.0f, 0.0f);

  for (const auto &entry : entries) {
    const auto &path = entry.path();
    const std::string filenameString = path.filename().string();
    const bool isDirectory = entry.is_directory();
    const bool selected = (m_SelectedPath == path);

    ImGui::PushID(filenameString.c_str());

    ImTextureID iconTexture = 0;
    if (isDirectory) {
      iconTexture = m_DirectoryIcon ? (ImTextureID)(intptr_t)m_DirectoryIcon->GetImGuiTextureID() : 0;
    } else if (IsImage(path)) {
      const std::string absolutePath = path.string();
      auto it = m_TextureCache.find(absolutePath);
      if (it == m_TextureCache.end()) {
        m_TextureCache[absolutePath] = Texture2D::Create(absolutePath);
        it = m_TextureCache.find(absolutePath);
      }
      if (it != m_TextureCache.end() && it->second)
        iconTexture = (ImTextureID)(intptr_t)it->second->GetImGuiTextureID();
      if (!iconTexture && m_PngIcon)
        iconTexture = (ImTextureID)(intptr_t)m_PngIcon->GetImGuiTextureID();
    } else if (IsModel(path)) {
      iconTexture = m_modelPng ? (ImTextureID)(intptr_t)m_modelPng->GetImGuiTextureID() : 0;
    } else {
      iconTexture = m_FileIcon ? (ImTextureID)(intptr_t)m_FileIcon->GetImGuiTextureID() : 0;
    }

    const ImVec4 background = selected ? EditorTheme::AccentMuted() : transparent;
    const bool clicked = iconTexture &&
        ImGui::ImageButton(filenameString.c_str(), iconTexture,
                           ImVec2(m_ThumbnailSize, m_ThumbnailSize), ImVec2(0, 1),
                           ImVec2(1, 0), background);

    if (clicked)
      m_SelectedPath = path;

    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("%s\n%s", path.filename().string().c_str(), path.string().c_str());
      if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && isDirectory)
        m_CurrentDirectory = path;
    }

    if (!isDirectory && ImGui::BeginDragDropSource()) {
      const std::string pathStr = path.string();
      ImGui::SetDragDropPayload("Content_Browser_item", pathStr.c_str(),
                                pathStr.length() + 1, ImGuiCond_Once);
      ImGui::TextUnformatted(filenameString.c_str());
      ImGui::EndDragDropSource();
    }

    float textWidth = ImGui::CalcTextSize(filenameString.c_str()).x;
    float textOffset = (m_ThumbnailSize - textWidth) * 0.5f;
    if (textOffset > 0.0f)
      ImGui::SetCursorPosX(ImGui::GetCursorPosX() + textOffset);
    if (selected)
      ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Accent());
    ImGui::TextWrapped("%s", filenameString.c_str());
    if (selected)
      ImGui::PopStyleColor();

    ImGui::NextColumn();
    ImGui::PopID();
  }

  ImGui::Columns(1);
}

void ContentBrowserPanel::DrawList(
    const std::vector<std::filesystem::directory_entry> &entries) {
  if (!ImGui::BeginTable("ContentList", 3,
                         ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                             ImGuiTableFlags_ScrollY)) {
    return;
  }

  ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
  ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 90.0f);
  ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 80.0f);
  ImGui::TableHeadersRow();

  for (const auto &entry : entries) {
    const auto &path = entry.path();
    const std::string filenameString = path.filename().string();
    const bool isDirectory = entry.is_directory();
    const bool selected = (m_SelectedPath == path);

    ImGui::PushID(filenameString.c_str());
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);

    if (ImGui::Selectable(filenameString.c_str(), selected,
                          ImGuiSelectableFlags_SpanAllColumns |
                              ImGuiSelectableFlags_AllowDoubleClick)) {
      m_SelectedPath = path;
      if (isDirectory && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
        m_CurrentDirectory = path;
    }
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("%s", path.string().c_str());

    if (!isDirectory && ImGui::BeginDragDropSource()) {
      const std::string pathStr = path.string();
      ImGui::SetDragDropPayload("Content_Browser_item", pathStr.c_str(),
                                pathStr.length() + 1, ImGuiCond_Once);
      ImGui::TextUnformatted(filenameString.c_str());
      ImGui::EndDragDropSource();
    }

    ImGui::TableSetColumnIndex(1);
    ImGui::TextDisabled("%s", TypeLabel(entry).c_str());

    ImGui::TableSetColumnIndex(2);
    if (isDirectory) {
      ImGui::TextDisabled("-");
    } else {
      std::error_code sizeError;
      const auto size = entry.file_size(sizeError);
      if (sizeError)
        ImGui::TextDisabled("?");
      else
        ImGui::TextDisabled("%s", FormatBytes(size).c_str());
    }

    ImGui::PopID();
  }

  ImGui::EndTable();
}

void ContentBrowserPanel::OnImGuiRender() {
  UHE_PROFILE_FUNCTION();
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 8));
  ImGui::Begin("Content Browser");

  if (ImGui::BeginTable("ContentBrowserLayout_v2", 2,
                        ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {
    ImGui::TableSetupColumn("Tree", ImGuiTableColumnFlags_WidthFixed, 170.0f);
    ImGui::TableSetupColumn("Content", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);

    ImGuiTreeNodeFlags rootFlags = ImGuiTreeNodeFlags_OpenOnArrow |
                                   ImGuiTreeNodeFlags_SpanAvailWidth |
                                   ImGuiTreeNodeFlags_DefaultOpen;
    if (m_CurrentDirectory == m_BaseDirectory)
      rootFlags |= ImGuiTreeNodeFlags_Selected;

    const std::string baseDirString = m_BaseDirectory.string();
    bool rootOpened = ImGui::TreeNodeEx(baseDirString.c_str(), rootFlags, "Assets");
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
      m_CurrentDirectory = m_BaseDirectory;
    if (rootOpened) {
      DrawDirectoryTree(m_BaseDirectory);
      ImGui::TreePop();
    }

    ImGui::TableSetColumnIndex(1);

    if (m_CurrentDirectory != m_BaseDirectory) {
      if (ImGui::SmallButton("Up"))
        m_CurrentDirectory = m_CurrentDirectory.parent_path();
      ImGui::SameLine(0.0f, 8.0f);
    }
    DrawBreadcrumbs();

    ImGui::Spacing();
    const float availW = ImGui::GetContentRegionAvail().x;
    const float listButtonW = 62.0f;
    EditorUI::SearchField("##ContentSearch", m_SearchBuffer, sizeof(m_SearchBuffer),
                          "Search assets...", std::max(80.0f, availW - listButtonW - 6.0f));
    ImGui::SameLine(0.0f, 6.0f);
    if (ImGui::Button(m_ListMode ? "Grid" : "List", ImVec2(listButtonW, 0.0f)))
      m_ListMode = !m_ListMode;

    // Collect, filter and sort entries once per frame.
    const std::string needle = ToLower(m_SearchBuffer);
    std::vector<std::filesystem::directory_entry> entries;
    std::error_code ec;
    for (const auto &entry : std::filesystem::directory_iterator(
             m_CurrentDirectory, std::filesystem::directory_options::skip_permission_denied, ec)) {
      if (!needle.empty() &&
          ToLower(entry.path().filename().string()).find(needle) == std::string::npos)
        continue;
      entries.push_back(entry);
    }
    std::sort(entries.begin(), entries.end(),
              [](const std::filesystem::directory_entry &a,
                 const std::filesystem::directory_entry &b) {
                const bool aDir = a.is_directory();
                const bool bDir = b.is_directory();
                if (aDir != bDir)
                  return aDir;
                return ToLower(a.path().filename().string()) <
                       ToLower(b.path().filename().string());
              });

    ImGui::Spacing();
    if (!m_ListMode) {
      ImGui::SetNextItemWidth(std::min(140.0f, availW));
      ImGui::SliderFloat("##ThumbSize", &m_ThumbnailSize, 40.0f, 128.0f, "");
      ImGui::SameLine(0.0f, 8.0f);
      ImGui::TextDisabled("%.0f px", m_ThumbnailSize);
      ImGui::SameLine(0.0f, 12.0f);
    }
    ImGui::TextDisabled("%zu items", entries.size());
    ImGui::Separator();

    ImGui::BeginChild("ContentGrid");
    if (m_ListMode)
      DrawList(entries);
    else
      DrawGrid(entries);
    ImGui::EndChild();

    ImGui::EndTable();
  }

  ImGui::End();
  ImGui::PopStyleVar();
}
} // namespace UHE
