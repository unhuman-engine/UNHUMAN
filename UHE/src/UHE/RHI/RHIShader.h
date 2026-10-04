#pragma once
#include <string>
#include <unordered_map>
#include "UHE/Core/Core.h"

namespace UHE::RHI
{

/**
 * \brief A compiled shader program owned by the RHI.
 *
 * \todo This interface is currently unused — the engine path goes through
 *       UHE::Shader (Renderer/Shader.h). Either wire it up or delete it so the
 *       RHI stops advertising a capability it does not implement.
 */
class RHIShader
{
public:
    virtual ~RHIShader() = default;

    /// Makes this program current for subsequent draw/dispatch calls.
    virtual void Bind() const = 0;
    /// Releases this program as the current one.
    virtual void Unbind() const = 0;

    [[nodiscard]] virtual const std::string& GetName() const = 0;

    [[nodiscard]] static Ref<RHIShader> Create(const std::string& filepath);
    [[nodiscard]] static Ref<RHIShader> Create(const std::string& name, const std::string& vertexSrc,
                                               const std::string& fragmentSrc);
};

/// Name → shader registry used to look programs up by identifier.
class ShaderLibrary
{
public:
    [[nodiscard]] static std::unique_ptr<ShaderLibrary> Create();

    /// \returns the shader registered under \p name, or nullptr when absent.
    RHIShader* GetShader(const std::string& name);
    void AddShader(const std::string& name, std::unique_ptr<RHIShader> shader);

private:
    ShaderLibrary() = default;

    std::unordered_map<std::string, std::unique_ptr<RHIShader>> m_Shaders;
};

} // namespace UHE::RHI
