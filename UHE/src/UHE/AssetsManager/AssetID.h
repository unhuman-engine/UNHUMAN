#pragma once
#include "UHE/Core/UUID.h"

namespace UHE
{

/**
 * @brief Persistent, stable identifier for registered assets.
 *
 * Built upon 64-bit UUID. AssetManager maps persistent AssetID to mutable source paths,
 * ensuring references survive file moves and renames.
 */
using AssetID = UUID;

} // namespace UHE
