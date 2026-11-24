#pragma once

#include <filesystem>
#include <string>
#include "UHE/Core/Core.h"

namespace UHE
{
/**
 * @brief Copies a file from outside the project into an asset folder.
 *
 * Dropping a file from the OS file explorer and the Content Browser's
 * "Import..." button both go through here, so a project only ever references
 * files it owns. The destination keeps the source file name; when that name
 * is taken the copy is numbered (`wall.png` -> `wall_1.png`) instead of
 * overwriting existing content.
 *
 * @return The destination path, or an empty path on failure (`error` set).
 */
UHE_API std::filesystem::path ImportAssetFile(const std::filesystem::path& sourceFile,
                                              const std::filesystem::path& destinationFolder,
                                              std::string* error = nullptr);

/**
 * @brief Path of `file` relative to `base` with '/' separators, so it can be
 * stored in a component or scene file. Returns the input when it is not under
 * `base` (the caller decides whether that is acceptable).
 */
UHE_API std::string MakeAssetRelativePath(const std::filesystem::path& file, const std::filesystem::path& base);
} // namespace UHE
