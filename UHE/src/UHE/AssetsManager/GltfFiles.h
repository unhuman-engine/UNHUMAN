#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>
#include "UHE/Core/Core.h"

namespace UHE
{

/**
 * @brief The JSON a glTF file holds: the whole .gltf, or the first chunk of a .glb.
 * @return nothing for a .glb whose header does not fit the file (truncated or not a glTF).
 */
UHE_API std::optional<std::string> GltfJson(const std::filesystem::path& file);

/// @brief A glTF URI ("%20" and friends) decoded to the file name on disk.
UHE_API std::string DecodeGltfUri(const std::string& uri);

/**
 * @brief The files a glTF reads besides itself: every buffer and image `uri` that is not a data URI.
 *
 * Decoded and relative to the model's folder, in the order the file names
 * them, duplicates removed. The exporter ships them with the model (issue #45)
 * and a cook records them, so it goes stale when one changes.
 *
 * @return nothing when the file is not a readable glTF.
 */
UHE_API std::optional<std::vector<std::filesystem::path>> GltfExternalFiles(const std::filesystem::path& file);

} // namespace UHE
