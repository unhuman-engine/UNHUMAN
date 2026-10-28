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
	};
}
