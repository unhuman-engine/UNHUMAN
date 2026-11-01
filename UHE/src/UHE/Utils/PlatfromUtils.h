#pragma once
#include "UHE/Core/Core.h"

#include <string>


namespace UHE {
	class UHE_API FileDialogs
	{
	public:
		// All three take an optional starting directory (a project asset folder,
		// for example). An empty string keeps the platform default.
		static std::string OpenFile(const char* filter, const std::string& defaultPath = "");
		static std::string SaveFile(const char* filter, const std::string& defaultPath = "");
		static std::string SelectFolder(const char* title = "Select Folder",
		                                const std::string& defaultPath = "");

		// Hands a file to whatever the desktop opens it with, so the editor can
		// offer "edit this script" without embedding a text editor. Runs the
		// opener directly (no shell), so a path with quotes or spaces is not a
		// command injection. Returns false when the opener could not be started.
		static bool OpenInDefaultApp(const std::string& path);
	};
}
