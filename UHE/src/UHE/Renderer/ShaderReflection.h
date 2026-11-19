#pragma once
#include "UHE/Core/Core.h"
#include "UHE/RHI/RHITypes.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace UHE
{
/**
 * \brief What a compiled SPIR-V module declares about itself.
 *
 * The engine needs to know a shader's interface -- which vertex attributes exist
 * and at which locations, how big the push constant block is, which resources it
 * binds, what the workgroup size is -- and it needs that without the shader
 * source and without a compiler: a build that only ships `.spv` still has to
 * build a matching pipeline layout. Reading it back out of the module is what
 * makes the compiled artifact self-describing, so the vertex layout and the push
 * constant size stop being hand-maintained copies of what the shader says.
 *
 * Nothing here talks to a device; it is a parser over a byte array.
 */

/// How a resource a shader declares is bound.
enum class ShaderResourceKind : u8
{
    UniformBuffer,
    StorageBuffer,
    SampledImage,
    StorageImage,
    Sampler,
    PushConstant,
    Other,
};

struct ShaderBinding
{
    u32 Set = 0;
    u32 Binding = 0;
    ShaderResourceKind Kind = ShaderResourceKind::Other;
    std::string Name;
    /// Element count of an array binding; 0 when it is not an array, 1 when it
    /// is a runtime-sized one.
    u32 ArraySize = 0;
};

struct ShaderVertexInput
{
    u32 Location = 0;
    RHI::ShaderDataType Type = RHI::ShaderDataType::None;
    std::string Name;
};

struct ShaderReflection
{
    /// False when the blob is not SPIR-V this reader understands; `Error` says why.
    bool Valid = false;
    std::string Error;

    RHI::ShaderStage Stage = RHI::ShaderStage::Vertex;
    /// The module's entry point name. Slang emits `main` for every stage.
    std::string EntryPoint;

    /// Workgroup size of a compute shader, zero for graphics stages.
    u32 LocalSize[3] = {0, 0, 0};

    /// Bytes the push constant block occupies; 0 when the shader takes none.
    u32 PushConstantSize = 0;

    std::vector<ShaderBinding> Bindings;
    /// Vertex attributes in declaration order (vertex stage only); empty for
    /// stages whose inputs are not a vertex buffer.
    std::vector<ShaderVertexInput> VertexInputs;

    /// \returns the binding at \p set / \p binding, or nullptr.
    [[nodiscard]] const ShaderBinding* FindBinding(u32 set, u32 binding) const;
};

/// Reads a single-entry-point SPIR-V module.
///
/// \param data must be 4-byte aligned, as `vk::ShaderModuleCreateInfo` requires.
/// \returns a reflection with `Valid == false` and `Error` set when the blob is
///          not readable SPIR-V -- never throws, never reads out of bounds.
[[nodiscard]] ShaderReflection ReflectSPIRV(const void* data, std::size_t size);
[[nodiscard]] ShaderReflection ReflectSPIRV(const std::vector<u8>& spirv);
} // namespace UHE
