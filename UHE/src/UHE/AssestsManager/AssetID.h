#pragma once
#include "UHE/Core/UUID.h"

namespace UHE
{

// Issue #38: stable asset identity. Reuses the engine's 64-bit UUID; the
// registry maps an ID to its current source path so renaming a file updates
// the path, never the ID.
using AssetID = UUID;

} // namespace UHE
