#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>
#include "UHE/Renderer/Texture.h"

namespace UHE
{
class ContentBrowserPanel
{
public:
    ContentBrowserPanel();

    /// @brief Re-roots the browser (and its breadcrumbs) at a project's asset folder.
    void SetBaseDirectory(const std::filesystem::path& directory);

    /// @brief Folder the user is browsing; imports and OS drops land here.
    const std::filesystem::path& GetCurrentDirectory() const { return m_CurrentDirectory; }

    /// @brief Root of the open project's assets: what a stored path is relative to.
    const std::filesystem::path& GetBaseDirectory() const { return m_BaseDirectory; }

    /// @brief Copies external files into the browsed folder (OS drops call this).
    /// @return The imported paths; failures are reported in the log.
    std::vector<std::filesystem::path> ImportFiles(const std::vector<std::string>& sources);

    /// @brief File-picker import into the browsed folder.
    void ImportFromDialog();

    /// @brief Re-scans the current folder (after an import).
    void Refresh() { m_TextureCache.clear(); }

    /// @brief Called when a script is attached from here ("Attach to selected entity").
    ///
    /// The editor decides what "selected" means; the panel does not reach into the hierarchy.
    void SetOnAttachScript(std::function<void(const std::filesystem::path&)> callback)
    {
        m_OnAttachScript = std::move(callback);
    }

    void OnImGuiRender();

private:
    /// @brief The folder tree in the left column, kept in step with the browsed folder.
    void DrawFolderTree();
    void DrawDirectoryTree(const std::filesystem::path& directoryPath);
    /// @brief Toolbar, item view and status footer: everything right of the tree.
    void DrawContentArea();
    void DrawToolbar();
    void DrawGrid(const std::vector<std::filesystem::directory_entry>& entries);
    void DrawGridTile(const std::filesystem::directory_entry& entry, float tileWidth, float tileHeight, float padding);
    void DrawList(const std::vector<std::filesystem::directory_entry>& entries);
    void DrawFooter(std::size_t itemCount, float width);

    /// @brief The texture that stands for \p path; image previews only when \p allowPreview.
    Ref<Texture2D> IconFor(const std::filesystem::path& path, bool isDirectory, bool allowPreview);

    /// @brief Writes a starter .lua into the browsed folder under a free name.
    void CreateScript();
    /// @brief Context menu shared by both views for a file entry.
    void DrawFileContextMenu(const std::filesystem::path& path);

    std::filesystem::path m_BaseDirectory;
    std::filesystem::path m_CurrentDirectory;
    std::filesystem::path m_SelectedPath;
    /// Folder the tree last expanded and scrolled to; differs from m_CurrentDirectory
    /// for one frame after navigating from the grid, breadcrumbs or Up.
    std::filesystem::path m_TreeRevealedDirectory;

    Ref<Texture2D> m_DirectoryIcon;
    Ref<Texture2D> m_FileIcon;
    Ref<Texture2D> m_PngIcon;
    Ref<Texture2D> m_modelPng;

    char m_SearchBuffer[128] = {};
    bool m_ListMode = false;
    float m_ThumbnailSize = 64.0f;

    // Dynamic texture cache for image previews
    std::unordered_map<std::string, Ref<Texture2D>> m_TextureCache;
    std::function<void(const std::filesystem::path&)> m_OnAttachScript;
};
} // namespace UHE
