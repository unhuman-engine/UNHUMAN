#pragma once


#include "UHE/Renderer/Texture.h"
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace UHE {
	class ContentBrowserPanel
	{
	public:
		ContentBrowserPanel();

		void OnImGuiRender();
		
	private:
		void DrawDirectoryTree(const std::filesystem::path& directoryPath);
		void DrawBreadcrumbs();
		void DrawGrid(const std::vector<std::filesystem::directory_entry>& entries);
		void DrawList(const std::vector<std::filesystem::directory_entry>& entries);

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
	};
}
