#pragma once

#include <glm/glm.hpp>
#include "UHE/Core/Core.h"
#include "UHE/RHI/RHIDevice.h"
#include "UHE/Renderer/Camera.h"
#include "UHE/Renderer/EditorCamera.h"
#include "UHE/Renderer/Font.h"
#include "UHE/Renderer/Texture.h"
#include "UHE/Renderer2D/SubTexture2D.h"

namespace UHE
{

class UHE_API Renderer2D
{
public:
    static void Init();
    static void Shutdown();

    /// Rebuilds every shader module and pipeline from the library's current bytes.
    ///
    /// The shader library calls this when a source changes on disk, and the
    /// editor's Reload Shaders command calls it directly. Waits for the device
    /// to be idle first: frames already queued reference the objects being
    /// replaced.
    static void ReloadShaders();

    static void BeginScene(const Camera& camera, const glm::mat4& transform);
    static void BeginScene(const EditorCamera& camera);
    static void EndScene();
    static void Flush();

    // Primitives
    static void DrawQuad(const glm::vec2& position, const glm::vec2& size, const glm::vec4& color);
    static void DrawQuad(const glm::vec3& position, const glm::vec2& size, const glm::vec4& color);
    static void DrawQuad(const glm::vec2& position, const glm::vec2& size, const Ref<Texture2D>& texture,
                         float tilingFactor = 1.0f, const glm::vec4& tintColor = glm::vec4(1.0f));
    static void DrawQuad(const glm::vec3& position, const glm::vec2& size, const Ref<Texture2D>& texture,
                         float tilingFactor = 1.0f, const glm::vec4& tintColor = glm::vec4(1.0f));

    static void DrawQuad(const glm::mat4& transform, const glm::vec4& color, int entityID = -1);
    static void DrawQuad(const glm::mat4& transform, const Ref<Texture2D>& texture, float tilingFactor = 1.0f,
                         const glm::vec4& tintColor = glm::vec4(1.0f), int entityID = -1);
    static void DrawQuad(const glm::mat4& transform, const Ref<SubTexture2D>& subtexture, float tilingFactor = 1.0f,
                         const glm::vec4& tintColor = glm::vec4(1.0f), int entityID = -1);

    static void DrawSprite(const glm::mat4& transform, struct SpriteRendererComponent& src, int entityID);

    static void DrawString(const std::string& text, Ref<Font2D> font, const glm::mat4& transform,
                           const glm::vec4& color, float kerning = 0.0f, float lineSpacing = 0.0f, int entityID = -1);

    // Stats
    struct Statistics
    {
        uint32_t DrawCalls = 0;
        uint32_t QuadCount = 0;

        uint32_t GetTotalVertexCount() const { return QuadCount * 4; }
        uint32_t GetTotalIndexCount() const { return QuadCount * 6; }
    };
    static void ResetStats();
    static Statistics GetStats();

private:
    static void StartBatch();
    static void NextBatch();

    // Creates and destroys the shader modules and the pipelines built from
    // them; the two are re-created together on reload.
    static void CreateShaderResources();
    static void DestroyShaderResources();
};
} // namespace UHE
