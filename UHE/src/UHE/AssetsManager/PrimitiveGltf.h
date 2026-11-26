#pragma once

#include <array>
#include <filesystem>
#include <string>
#include "UHE/Core/Core.h"

namespace UHE
{
/// @brief The shapes WritePrimitiveGltf can write.
enum class PrimitiveShape : u8
{
    Plane, ///< 1 x 1 on XZ, facing +Y
    Cube,  ///< 1 x 1 x 1, centred on the origin
};

/**
 * @brief Writes a unit shape as `<path>` (.gltf) plus a `.bin` beside it.
 *
 * Positions, normals, UVs and 16-bit indices, with one material of colour
 * \p baseColor (linear RGBA) and the given roughness. Used for the starter
 * content of a new project, so it has something lit to look at without
 * depending on the engine's sample models.
 *
 * @return false (with \p error set) when a file cannot be written.
 */
UHE_API bool WritePrimitiveGltf(const std::filesystem::path& path, PrimitiveShape shape,
                                const std::array<float, 4>& baseColor, float roughness, std::string* error = nullptr);
} // namespace UHE
