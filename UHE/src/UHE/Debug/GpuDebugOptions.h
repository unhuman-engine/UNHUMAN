#pragma once

#include "UHE/Core/Core.h"

namespace UHE
{

/**
 * @brief GPU debugging choices that apply when the device is created.
 *
 * Validation features and RenderDoc's hooks are fixed when the Vulkan
 * instance is created, so these are read once at startup. A composition root
 * (the editor's CreateApplication) sets the requests before it constructs the
 * Application; the command line (`-sync-validation`, `-renderdoc`) and the
 * environment (`UHE_SYNC_VALIDATION`, `UHE_RENDERDOC`) turn them on as well.
 * The `...Active` fields report what this run actually got.
 */
struct GpuDebugOptions
{
    bool RequestSyncValidation = false; ///< the layer's hazard tracking (needs validation layers)
    bool RequestRenderDoc = false;      ///< load RenderDoc at startup

    bool ValidationActive = false;     ///< the validation layer is on in this run
    bool SyncValidationActive = false; ///< synchronization validation is on in this run
};

/// @brief The process's options; set requests before the Application is constructed.
UHE_API GpuDebugOptions& GetGpuDebugOptions();

} // namespace UHE
