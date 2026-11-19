#pragma once
#include "UHE/Core/Core.h"
#include "UHE/RHI/RHIDevice.h"
#include "UHE/RHI/RHITypes.h"
#include "UHE/Renderer/ShaderCache.h"

namespace UHE
{
/**
 * \brief Turning a loaded shader into the RHI objects a pipeline needs.
 *
 * Shared by the renderers so the three answers a pipeline builder needs from a
 * shader -- its module, its vertex layout and its push constant range -- come out
 * of the module the same way everywhere, instead of each renderer keeping its own
 * hand-written copy of what the shader says.
 */

/// Creates one stage's shader module from \p shader.
///
/// \returns nullptr (with an error logged) when the shader has no such stage.
[[nodiscard]] RHI::ShaderHandle CreateStageShader(RHI::RHIDevice& device, const ShaderCache::Shader& shader,
                                                  RHI::ShaderStage stage, const char* debugName);

/// The vertex layout the module declares, in location order.
///
/// Empty for a shader whose vertices are generated (the grid uses
/// `gl_VertexIndex`), and for stages that take no vertex buffer.
[[nodiscard]] RHI::BufferLayout ReflectedVertexLayout(const ShaderCache::Shader& shader);

/// Push constant bytes the module expects: the largest block any stage declares.
[[nodiscard]] u32 ReflectedPushConstantSize(const ShaderCache::Shader& shader);
} // namespace UHE
