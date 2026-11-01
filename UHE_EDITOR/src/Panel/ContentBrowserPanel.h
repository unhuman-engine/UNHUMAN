#pragma once


#include "UHE/Renderer/Texture.h"
#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace UHE {
	class ContentBrowserPanel
	{
	public:
		ContentBrowserPanel();

		// Re-roots the browser (and its breadcrumbs) at a project's asset folder.
		void SetBaseDirectory(const std::filesystem::path& directory);

		// Folder the user is browsing; imports and OS drops land here.
		const std::filesystem::path& GetCurrentDirectory() const { return m_CurrentDirectory; }

		// Root of the open project's assets: what a stored path is relative to.
		const std::filesystem::path& GetBaseDirectory() const { return m_BaseDirectory; }

		// Copies external files into the browsed folder (OS drops call this).
		// Returns the imported paths; failures are reported in the panel.
		std::vector<std::filesystem::path> ImportFiles(const std::vector<std::string>& sources);

		// File-picker import into the browsed folder.
		void ImportFromDialog();

		// Re-scans the current folder (after an import).
		void Refresh() { m_TextureCache.clear(); }

		// Called when a script is attached from here ("Attach to selected
		// entity"). The editor decides what "selected" means; the panel does not
		// reach into the hierarchy.
		void SetOnAttachScript(std::function<void(const std::filesystem::path&)> callback)
		{
			m_OnAttachScript = std::move(callback);
		}

		void OnImGuiRender();
		
	private:
		void DrawDirectoryTree(const std::filesystem::path& directoryPath);
		void DrawBreadcrumbs();
		void DrawGrid(const std::vector<std::filesystem::directory_entry>& entries);
		void DrawList(const std::vector<std::filesystem::directory_entry>& entries);

		// Writes a starter .lua into the browsed folder under a free name.
		void CreateScript();
		// Context menu shared by both views for a file entry.
		void DrawFileContextMenu(const std::filesystem::path& path);

		std::filesystem::path m_BaseDirectory;
		std::filesystem::path m_CurrentDirectory;
		std::filesystem::path m_SelectedPath;

		Ref<Texture2D> m_DirectoryIcon;
		Ref<Texture2D> m_FileIcon;
		Ref<Texture2D> m_PngIcon;
		Ref<Texture2D> m_modelPng;

		char m_SearchBuffer[128] = {};
		bool m_ListMode = false;
		float m_ThumbnailSize = 72.0f;

		// Dynamic texture cache for image previews
		std::unordered_map<std::string, Ref<Texture2D>> m_TextureCache;
		std::function<void(const std::filesystem::path&)> m_OnAttachScript;
	};
}
