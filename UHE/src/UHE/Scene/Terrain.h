#pragma once

#include <memory>
#include <string>
#include <vector>
#include "UHE/Core/Core.h"
#include "UHE/Renderer3D/LoadModel.h"

namespace UHE
{
struct TerrainComponent;

/**
 * @brief The CPU side of TerrainComponent: heights, the mesh and lookups.
 *
 * No GPU and no scene: Scene calls these to rebuild a terrain whose fields
 * changed (BuildKey), and tests/scene checks them directly.
 */
namespace Terrain
{
/// @brief A square grid of heights in metres, Samples x Samples, row-major along +Z.
struct Heightfield
{
    u32 Samples = 0;
    float Size = 0.0f; ///< metres from the first sample to the last, on each axis
    std::vector<float> Heights;
};

/// @brief Fingerprint of every field that shapes or colours the terrain (the caller adds the heightmap's file time).
[[nodiscard]] UHE_API u64 BuildKey(const TerrainComponent& terrain);

/**
 * @brief The terrain's heights: the heightmap image resampled to the grid,
 *        or seeded fractal noise.
 *
 * @param resolvedHeightmap the heightmap's path on disk; empty for noise.
 *        A heightmap that does not load falls back to noise (logged).
 */
[[nodiscard]] UHE_API Heightfield BuildHeightfield(const TerrainComponent& terrain,
                                                   const std::string& resolvedHeightmap = {});

/// @brief Height at local (\p x, \p z) metres from the terrain's centre, bilinear; edges clamp.
[[nodiscard]] UHE_API float HeightAt(const Heightfield& field, float x, float z);

/**
 * @brief The drawable surface: one primitive with normals, UVs (one repeat
 *        per 4 m) and vertex colours from the layers, centred on the origin.
 */
[[nodiscard]] UHE_API RD3d::Primitive BuildPrimitive(const Heightfield& field, const TerrainComponent& terrain);

/// @brief \p field resampled to \p samples per side (Jolt wants a count its block size divides).
[[nodiscard]] UHE_API Heightfield Resample(const Heightfield& field, u32 samples);
} // namespace Terrain
} // namespace UHE
