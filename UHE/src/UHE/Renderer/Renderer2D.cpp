#include "Renderer2D.h"
#include <array>
#include <glm/gtc/matrix_transform.hpp>
#include <vector>
#include "UHE/AssestsManager/VfsSystem.h"
#include "UHE/RHI/RHICommandBuffer.h"
#include "UHE/RHI/RHIDevice.h"
#include "UHE/Renderer/Font.h"
#include "UHE/Renderer/Renderer.h"
#include "UHE/Renderer/ShaderCache.h"
#include "UHE/Renderer/ShaderResources.h"
#include "UHE/Scene/Components.h"

namespace UHE
{

struct QuadVertex
{
    glm::vec3 Position;
    glm::vec4 Color;
    glm::vec2 TexCoord;
    float TexIndex; // Changed from uint32_t to float to avoid integer attribute bugs
    float TilingFactor;
    float EntityID; // Changed from int to float to avoid integer attribute bugs
};

struct TextVertex
{
    glm::vec3 Position;
    glm::vec4 Color;
    glm::vec2 TexCoord;
    float TexIndex;
    float EntityID;
};

struct Renderer2DPushConstants
{
    glm::mat4 viewProjection;
    int textureIndices[32];
};

struct Renderer2DData
{
    static const uint32_t MaxQuads = 20000;
    static const uint32_t MaxVertices = MaxQuads * 4;
    static const uint32_t MaxIndices = MaxQuads * 6;
    static const uint32_t MaxTextureSlots = 32;

    RHI::PipelineHandle QuadPipeline;
    RHI::ShaderHandle QuadVertexShader;
    RHI::ShaderHandle QuadFragmentShader;
    RHI::BufferHandle QuadVertexBuffers[2];
    RHI::BufferHandle QuadIndexBuffer;

    RHI::PipelineHandle TextPipeline;
    RHI::ShaderHandle TextVertexShader;
    RHI::ShaderHandle TextFragmentShader;
    RHI::BufferHandle TextVertexBuffers[2];

    uint32_t TextIndexCount = 0;
    TextVertex* TextVertexBufferBase = nullptr;
    TextVertex* TextVertexBufferPtr = nullptr;

    uint32_t QuadIndexCount = 0;
    QuadVertex* QuadVertexBufferBase = nullptr;
    QuadVertex* QuadVertexBufferPtr = nullptr;

    glm::vec4 QuadVertexPositions[4];

    Renderer2D::Statistics Stats;

    RHI::TextureHandle WhiteTexture;

    uint32_t TextureSlots[MaxTextureSlots];
    uint32_t TextureSlotIndex = 1; // 0 is white texture
    glm::mat4 ViewProjectionMatrix;
};

static Renderer2DData s_Data;

void Renderer2D::Init()
{
    UHE_PROFILE_FUNCTION();
    auto& device = Renderer::GetDevice();

    // Create Vertex Buffers for double buffering
    RHI::BufferDesc vboDesc{};
    vboDesc.size = s_Data.MaxVertices * sizeof(QuadVertex);
    vboDesc.usage = RHI::BufferUsage::Vertex;
    vboDesc.hostVisible = true;
    vboDesc.debugName = "QuadVertexBuffer";
    for (int i = 0; i < 2; i++)
    {
        s_Data.QuadVertexBuffers[i] = device.CreateBuffer(vboDesc);
    }

    // Create Index Buffer
    uint32_t* quadIndices = new uint32_t[s_Data.MaxIndices];
    uint32_t offset = 0;
    for (uint32_t i = 0; i < s_Data.MaxIndices; i += 6)
    {
        quadIndices[i + 0] = offset + 0;
        quadIndices[i + 1] = offset + 1;
        quadIndices[i + 2] = offset + 2;

        quadIndices[i + 3] = offset + 2;
        quadIndices[i + 4] = offset + 3;
        quadIndices[i + 5] = offset + 0;
        offset += 4;
    }

    RHI::BufferDesc iboDesc{};
    iboDesc.size = s_Data.MaxIndices * sizeof(uint32_t);
    iboDesc.usage = RHI::BufferUsage::Index;
    iboDesc.hostVisible = true; // For simple upload, though typically staging buffer is used
    iboDesc.debugName = "QuadIndexBuffer";
    s_Data.QuadIndexBuffer = device.CreateBuffer(iboDesc);

    device.GetCurrentCommandBuffer().UpdateBuffer(s_Data.QuadIndexBuffer, quadIndices, iboDesc.size, 0);

    s_Data.QuadVertexBufferBase = new QuadVertex[s_Data.MaxVertices];

    // Create Vertex Buffers for Text
    RHI::BufferDesc textVboDesc{};
    textVboDesc.size = s_Data.MaxVertices * sizeof(TextVertex);
    textVboDesc.usage = RHI::BufferUsage::Vertex;
    textVboDesc.hostVisible = true;
    textVboDesc.debugName = "TextVertexBuffer";
    for (int i = 0; i < 2; i++)
    {
        s_Data.TextVertexBuffers[i] = device.CreateBuffer(textVboDesc);
    }
    s_Data.TextVertexBufferBase = new TextVertex[s_Data.MaxVertices];

    RHI::TextureDesc whiteTexDesc{};
    whiteTexDesc.width = 1;
    whiteTexDesc.height = 1;
    whiteTexDesc.format = RHI::TextureFormat::RGBA8_UNORM;
    whiteTexDesc.usage = RHI::TextureUsage::Sampled | RHI::TextureUsage::TransferDst;
    s_Data.WhiteTexture = device.CreateTexture(whiteTexDesc);
    u32 whiteTextureData = 0xffffffff;
    device.GetCurrentCommandBuffer().UpdateTexture(
        s_Data.WhiteTexture, std::span<const u8>(reinterpret_cast<const u8*>(&whiteTextureData), sizeof(u32)));
    s_Data.TextureSlots[0] = reinterpret_cast<RHI::RHITexture*>(s_Data.WhiteTexture)->GetTextureIndex();

    // Shaders and the pipelines built from them: created together, and re-created
    // together when a shader changes on disk.
    CreateShaderResources();

    static bool listenerRegistered = false;
    if (!listenerRegistered)
    {
        listenerRegistered = true;
        ShaderCache::Get().AddReloadListener([](ShaderCache::Handle) { Renderer2D::ReloadShaders(); });
    }

    s_Data.QuadVertexPositions[0] = {-0.5f, -0.5f, 0.0f, 1.0f};
    s_Data.QuadVertexPositions[1] = {0.5f, -0.5f, 0.0f, 1.0f};
    s_Data.QuadVertexPositions[2] = {0.5f, 0.5f, 0.0f, 1.0f};
    s_Data.QuadVertexPositions[3] = {-0.5f, 0.5f, 0.0f, 1.0f};

}

void Renderer2D::CreateShaderResources()
{
    auto& device = Renderer::GetDevice();
    ShaderCache& library = ShaderCache::Get();

    const std::filesystem::path shaderDir = FileSystem::Get().GetRootPath() / "assets/shaders";
    const ShaderCache::Handle quadHandle = library.Load((shaderDir / "Texture.slang").string());
    const ShaderCache::Handle textHandle = library.Load((shaderDir / "Text.slang").string());

    const ShaderCache::Shader* quad = library.Get(quadHandle);
    const ShaderCache::Shader* text = library.Get(textHandle);
    if (quad == nullptr || text == nullptr)
    {
        UHE_CORE_ERROR("Renderer2D: a sprite shader failed to load; 2D will not draw");
        return;
    }

    s_Data.QuadVertexShader = CreateStageShader(device, *quad, RHI::ShaderStage::Vertex, "Texture.vertex");
    s_Data.QuadFragmentShader = CreateStageShader(device, *quad, RHI::ShaderStage::Fragment, "Texture.fragment");
    s_Data.TextVertexShader = CreateStageShader(device, *text, RHI::ShaderStage::Vertex, "Text.vertex");
    s_Data.TextFragmentShader = CreateStageShader(device, *text, RHI::ShaderStage::Fragment, "Text.fragment");

    RHI::GraphicsPipelineDesc pipeDesc{};
    pipeDesc.vertexShader = s_Data.QuadVertexShader;
    pipeDesc.fragmentShader = s_Data.QuadFragmentShader;
    // The vertex layout and the push constant range come from the module itself,
    // not from a list here that has to be kept in step with the shader.
    pipeDesc.vertexLayout = ReflectedVertexLayout(*quad);
    pipeDesc.pushConstantSize = ReflectedPushConstantSize(*quad);

    // The pipeline's range came from the shader's reflection, so this is where a
    // host struct that drifted from the Slang one shows up -- before an upload
    // writes past the block the pipeline was built with.
    if (pipeDesc.pushConstantSize != sizeof(Renderer2DPushConstants))
    {
        UHE_CORE_ERROR("Renderer2D: push constants are {0} bytes in the shader and {1} in the host struct",
                       pipeDesc.pushConstantSize, sizeof(Renderer2DPushConstants));
    }
    pipeDesc.blendMode = RHI::BlendMode::Alpha;
    pipeDesc.depthTest = true;
    pipeDesc.depthWrite = true;

    // This should match the Framebuffer color format
    pipeDesc.colorAttachmentCount = 2;
    pipeDesc.colorFormats[0] = RHI::TextureFormat::RGBA8_SRGB;
    pipeDesc.colorFormats[1] = RHI::TextureFormat::R32_SINT;

    s_Data.QuadPipeline = device.CreateGraphicsPipeline(pipeDesc);
    UHE_CORE_ASSERT(s_Data.QuadPipeline, "Failed to create QuadPipeline!");

    RHI::GraphicsPipelineDesc textPipeDesc{};
    textPipeDesc.vertexShader = s_Data.TextVertexShader;
    textPipeDesc.fragmentShader = s_Data.TextFragmentShader;
    textPipeDesc.vertexLayout = ReflectedVertexLayout(*text);
    textPipeDesc.pushConstantSize = ReflectedPushConstantSize(*text);
    textPipeDesc.blendMode = RHI::BlendMode::Alpha;
    textPipeDesc.depthTest = true;
    textPipeDesc.depthWrite = true;
    textPipeDesc.colorAttachmentCount = 2;
    textPipeDesc.colorFormats[0] = RHI::TextureFormat::RGBA8_SRGB;
    textPipeDesc.colorFormats[1] = RHI::TextureFormat::R32_SINT;

    s_Data.TextPipeline = device.CreateGraphicsPipeline(textPipeDesc);
    UHE_CORE_ASSERT(s_Data.TextPipeline, "Failed to create TextPipeline!");
}

void Renderer2D::DestroyShaderResources()
{
    auto& device = Renderer::GetDevice();

    device.DestroyGraphicsPipeline(s_Data.QuadPipeline);
    device.DestroyShader(s_Data.QuadVertexShader);
    device.DestroyShader(s_Data.QuadFragmentShader);
    device.DestroyGraphicsPipeline(s_Data.TextPipeline);
    device.DestroyShader(s_Data.TextVertexShader);
    device.DestroyShader(s_Data.TextFragmentShader);

    // Nulled, not left dangling: a second destroy of the same handle would free
    // someone else's object.
    s_Data.QuadPipeline = nullptr;
    s_Data.QuadVertexShader = nullptr;
    s_Data.QuadFragmentShader = nullptr;
    s_Data.TextPipeline = nullptr;
    s_Data.TextVertexShader = nullptr;
    s_Data.TextFragmentShader = nullptr;
}

void Renderer2D::ReloadShaders()
{
    // Frames already queued reference the modules and pipelines being replaced.
    Renderer::GetDevice().WaitIdle();

    DestroyShaderResources();
    CreateShaderResources();
    UHE_CORE_INFO("Renderer2D: pipelines rebuilt from the reloaded shaders");
}

void Renderer2D::Shutdown()
{
    UHE_PROFILE_FUNCTION();
    auto& device = Renderer::GetDevice();
    Font2D::Shutdown();
    delete[] s_Data.QuadVertexBufferBase;

    delete[] s_Data.TextVertexBufferBase;
    DestroyShaderResources();
    for (int i = 0; i < 2; i++)
    {
        device.DestroyBuffer(s_Data.TextVertexBuffers[i]);
    }

    for (int i = 0; i < 2; i++)
    {
        device.DestroyBuffer(s_Data.QuadVertexBuffers[i]);
    }
    device.DestroyBuffer(s_Data.QuadIndexBuffer);
    device.DestroyTexture(s_Data.WhiteTexture);
}

void Renderer2D::BeginScene(const Camera& camera, const glm::mat4& transform)
{
    UHE_PROFILE_FUNCTION();
    s_Data.ViewProjectionMatrix = camera.GetProjection() * glm::inverse(transform);
    auto& cmd = Renderer::GetDevice().GetCurrentCommandBuffer();

    StartBatch();
}

void Renderer2D::BeginScene(const EditorCamera& camera)
{
    UHE_PROFILE_FUNCTION();
    s_Data.ViewProjectionMatrix = camera.GetViewProjection();
    auto& cmd = Renderer::GetDevice().GetCurrentCommandBuffer();

    StartBatch();
}

void Renderer2D::EndScene()
{
    UHE_PROFILE_FUNCTION();
    Flush();
}

void Renderer2D::StartBatch()
{
    UHE_PROFILE_FUNCTION();
    s_Data.QuadIndexCount = 0;
    s_Data.QuadVertexBufferPtr = s_Data.QuadVertexBufferBase;

    s_Data.TextIndexCount = 0;
    s_Data.TextVertexBufferPtr = s_Data.TextVertexBufferBase;

    s_Data.TextureSlotIndex = 1;
}

void Renderer2D::Flush()
{
    UHE_PROFILE_FUNCTION();
    if (s_Data.QuadIndexCount == 0 && s_Data.TextIndexCount == 0)
        return;

    auto& device = Renderer::GetDevice();
    auto& cmd = device.GetCurrentCommandBuffer();
    uint32_t frameIndex = device.GetCurrentFrameIndex();

    if (s_Data.QuadIndexCount > 0)
    {
        uint32_t dataSize = (uint32_t)((uint8_t*)s_Data.QuadVertexBufferPtr - (uint8_t*)s_Data.QuadVertexBufferBase);
        cmd.UpdateBuffer(s_Data.QuadVertexBuffers[frameIndex], s_Data.QuadVertexBufferBase, dataSize, 0);

        cmd.BindPipeline(s_Data.QuadPipeline);
        cmd.BindVertexBuffer(s_Data.QuadVertexBuffers[frameIndex], 0);
        cmd.BindIndexBuffer(s_Data.QuadIndexBuffer, 0);

        Renderer2DPushConstants pc;
        pc.viewProjection = s_Data.ViewProjectionMatrix;
        for (uint32_t i = 0; i < 32; i++)
        {
            pc.textureIndices[i] = (i < s_Data.TextureSlotIndex) ? s_Data.TextureSlots[i] : 0;
        }
        cmd.PushConstants(RHI::ShaderStage::AllGraphics, &pc, sizeof(Renderer2DPushConstants), 0);

        cmd.DrawIndexed(s_Data.QuadIndexCount, 0, 0);
        s_Data.Stats.DrawCalls++;
    }

    if (s_Data.TextIndexCount > 0)
    {
        uint32_t dataSize = (uint32_t)((uint8_t*)s_Data.TextVertexBufferPtr - (uint8_t*)s_Data.TextVertexBufferBase);
        cmd.UpdateBuffer(s_Data.TextVertexBuffers[frameIndex], s_Data.TextVertexBufferBase, dataSize, 0);

        cmd.BindPipeline(s_Data.TextPipeline);
        cmd.BindVertexBuffer(s_Data.TextVertexBuffers[frameIndex], 0);
        cmd.BindIndexBuffer(s_Data.QuadIndexBuffer, 0);

        Renderer2DPushConstants pc;
        pc.viewProjection = s_Data.ViewProjectionMatrix;
        for (uint32_t i = 0; i < 32; i++)
        {
            pc.textureIndices[i] = (i < s_Data.TextureSlotIndex) ? s_Data.TextureSlots[i] : 0;
        }
        cmd.PushConstants(RHI::ShaderStage::AllGraphics, &pc, sizeof(Renderer2DPushConstants), 0);

        cmd.DrawIndexed(s_Data.TextIndexCount, 0, 0);
        s_Data.Stats.DrawCalls++;
    }
}

void Renderer2D::NextBatch()
{
    UHE_PROFILE_FUNCTION();
    Flush();
    StartBatch();
}

void Renderer2D::DrawQuad(const glm::vec2& position, const glm::vec2& size, const glm::vec4& color)
{
    UHE_PROFILE_FUNCTION();
    DrawQuad({position.x, position.y, 0.0f}, size, color);
}

void Renderer2D::DrawQuad(const glm::vec3& position, const glm::vec2& size, const glm::vec4& color)
{
    UHE_PROFILE_FUNCTION();
    glm::mat4 transform =
        glm::translate(glm::mat4(1.0f), position) * glm::scale(glm::mat4(1.0f), {size.x, size.y, 1.0f});

    DrawQuad(transform, color);
}

void Renderer2D::DrawQuad(const glm::vec2& position, const glm::vec2& size, const Ref<Texture2D>& texture,
                          float tilingFactor, const glm::vec4& tintColor)
{
    UHE_PROFILE_FUNCTION();
    DrawQuad({position.x, position.y, 0.0f}, size, texture, tilingFactor, tintColor);
}

void Renderer2D::DrawQuad(const glm::vec3& position, const glm::vec2& size, const Ref<Texture2D>& texture,
                          float tilingFactor, const glm::vec4& tintColor)
{
    UHE_PROFILE_FUNCTION();
    glm::mat4 transform =
        glm::translate(glm::mat4(1.0f), position) * glm::scale(glm::mat4(1.0f), {size.x, size.y, 1.0f});

    DrawQuad(transform, texture, tilingFactor, tintColor);
}

void Renderer2D::DrawQuad(const glm::mat4& transform, const glm::vec4& color, int entityID)
{
    UHE_PROFILE_FUNCTION();
    constexpr size_t quadVertexCount = 4;
    const float textureIndex = 0.0f; // 0 maps to WhiteTexture in TextureSlots
    constexpr glm::vec2 textureCoords[] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};
    const float tilingFactor = 1.0f;

    if (s_Data.QuadIndexCount >= Renderer2DData::MaxIndices)
        NextBatch();

    for (size_t i = 0; i < quadVertexCount; i++)
    {
        s_Data.QuadVertexBufferPtr->Position = transform * s_Data.QuadVertexPositions[i];
        s_Data.QuadVertexBufferPtr->Color = color;
        s_Data.QuadVertexBufferPtr->TexCoord = textureCoords[i];
        s_Data.QuadVertexBufferPtr->TexIndex = textureIndex;
        s_Data.QuadVertexBufferPtr->TilingFactor = tilingFactor;
        s_Data.QuadVertexBufferPtr->EntityID = entityID;
        s_Data.QuadVertexBufferPtr++;
    }

    s_Data.QuadIndexCount += 6;
    s_Data.Stats.QuadCount++;
}

void Renderer2D::DrawQuad(const glm::mat4& transform, const Ref<Texture2D>& texture, float tilingFactor,
                          const glm::vec4& tintColor, int entityID)
{
    UHE_PROFILE_FUNCTION();
    constexpr size_t quadVertexCount = 4;
    constexpr glm::vec2 textureCoords[] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};

    if (s_Data.QuadIndexCount >= Renderer2DData::MaxIndices)
        NextBatch();

    float textureIndex = 0.0f;
    uint32_t globalTexIndex = texture->GetTextureIndex();
    for (uint32_t i = 1; i < s_Data.TextureSlotIndex; i++)
    {
        if (s_Data.TextureSlots[i] == globalTexIndex)
        {
            textureIndex = (float)i;
            break;
        }
    }

    if (textureIndex == 0.0f)
    {
        if (s_Data.TextureSlotIndex >= Renderer2DData::MaxTextureSlots)
            NextBatch();

        textureIndex = (float)s_Data.TextureSlotIndex;
        s_Data.TextureSlots[s_Data.TextureSlotIndex] = globalTexIndex;
        s_Data.TextureSlotIndex++;
    }

    for (size_t i = 0; i < quadVertexCount; i++)
    {
        s_Data.QuadVertexBufferPtr->Position = transform * s_Data.QuadVertexPositions[i];
        s_Data.QuadVertexBufferPtr->Color = tintColor;
        s_Data.QuadVertexBufferPtr->TexCoord = textureCoords[i];
        s_Data.QuadVertexBufferPtr->TexIndex = textureIndex;
        s_Data.QuadVertexBufferPtr->TilingFactor = tilingFactor;
        s_Data.QuadVertexBufferPtr->EntityID = entityID;
        s_Data.QuadVertexBufferPtr++;
    }

    s_Data.QuadIndexCount += 6;
    s_Data.Stats.QuadCount++;
}

void Renderer2D::DrawQuad(const glm::mat4& transform, const Ref<SubTexture2D>& subtexture, float tilingFactor,
                          const glm::vec4& tintColor, int entityID)
{
    UHE_PROFILE_FUNCTION();
    constexpr size_t quadVertexCount = 4;
    const glm::vec2* textureCoords = subtexture->GetTexCoords();
    const Ref<Texture2D> texture = subtexture->GetTexture();

    if (s_Data.QuadIndexCount >= Renderer2DData::MaxIndices)
        NextBatch();

    float textureIndex = 0.0f;
    uint32_t globalTexIndex = texture->GetTextureIndex();
    for (uint32_t i = 1; i < s_Data.TextureSlotIndex; i++)
    {
        if (s_Data.TextureSlots[i] == globalTexIndex)
        {
            textureIndex = (float)i;
            break;
        }
    }

    if (textureIndex == 0.0f)
    {
        if (s_Data.TextureSlotIndex >= Renderer2DData::MaxTextureSlots)
            NextBatch();

        textureIndex = (float)s_Data.TextureSlotIndex;
        s_Data.TextureSlots[s_Data.TextureSlotIndex] = globalTexIndex;
        s_Data.TextureSlotIndex++;
    }

    for (size_t i = 0; i < quadVertexCount; i++)
    {
        s_Data.QuadVertexBufferPtr->Position = transform * s_Data.QuadVertexPositions[i];
        s_Data.QuadVertexBufferPtr->Color = tintColor;
        s_Data.QuadVertexBufferPtr->TexCoord = textureCoords[i];
        s_Data.QuadVertexBufferPtr->TexIndex = textureIndex;
        s_Data.QuadVertexBufferPtr->TilingFactor = tilingFactor;
        s_Data.QuadVertexBufferPtr->EntityID = entityID;
        s_Data.QuadVertexBufferPtr++;
    }

    s_Data.QuadIndexCount += 6;
    s_Data.Stats.QuadCount++;
}

void Renderer2D::DrawString(const std::string& text, Ref<Font2D> font, const glm::mat4& transform,
                            const glm::vec4& color, f32 kerning, f32 lineSpacing, i32 entityID)
{
    UHE_PROFILE_FUNCTION();
    if (!font || !font->IsValid())
        return;

    const auto& fontGeometry = font->GetAtlas();
    if (!fontGeometry)
        return;

    f32 textureIndex = 0.0f;
    u32 globalTexIndex = reinterpret_cast<RHI::RHITexture*>(fontGeometry)->GetTextureIndex();
    for (u32 i = 1; i < s_Data.TextureSlotIndex; i++)
    {
        if (s_Data.TextureSlots[i] == globalTexIndex)
        {
            textureIndex = (f32)i;
            break;
        }
    }

    if (textureIndex == 0.0f)
    {
        if (s_Data.TextureSlotIndex >= Renderer2DData::MaxTextureSlots)
            NextBatch();

        textureIndex = (f32)s_Data.TextureSlotIndex;
        s_Data.TextureSlots[s_Data.TextureSlotIndex] = globalTexIndex;
        s_Data.TextureSlotIndex++;
    }

    f64 x = 0.0;
    f64 y = 0.0;

    f64 scale = 1.0 / (f64)font->GetLineHeight();

    for (size_t i = 0; i < text.size(); i++)
    {
        if (s_Data.TextIndexCount >= Renderer2DData::MaxIndices)
        {
            NextBatch();

            // Re-resolve texture index as NextBatch resets texture slots
            textureIndex = 0.0f;
            for (u32 j = 1; j < s_Data.TextureSlotIndex; j++)
            {
                if (s_Data.TextureSlots[j] == globalTexIndex)
                {
                    textureIndex = (f32)j;
                    break;
                }
            }

            if (textureIndex == 0.0f)
            {
                if (s_Data.TextureSlotIndex >= Renderer2DData::MaxTextureSlots)
                    NextBatch();

                textureIndex = (f32)s_Data.TextureSlotIndex;
                s_Data.TextureSlots[s_Data.TextureSlotIndex] = globalTexIndex;
                s_Data.TextureSlotIndex++;
            }
        }

        unsigned char c0 = text[i];
        char32_t character = 0;
        int seqLen = 1;

        if ((c0 & 0x80) == 0)
        {
            character = c0;
        }
        else if ((c0 & 0xE0) == 0xC0)
        {
            if (i + 1 < text.length())
            {
                character = ((c0 & 0x1F) << 6) | (text[i + 1] & 0x3F);
                seqLen = 2;
            }
            else
            {
                character = 0xFFFD;
            }
        }
        else if ((c0 & 0xF0) == 0xE0)
        {
            if (i + 2 < text.length())
            {
                character = ((c0 & 0x0F) << 12) | ((text[i + 1] & 0x3F) << 6) | (text[i + 2] & 0x3F);
                seqLen = 3;
            }
            else
            {
                character = 0xFFFD;
            }
        }
        else if ((c0 & 0xF8) == 0xF0)
        {
            if (i + 3 < text.length())
            {
                character = ((c0 & 0x07) << 18) | ((text[i + 1] & 0x3F) << 12) | ((text[i + 2] & 0x3F) << 6) |
                            (text[i + 3] & 0x3F);
                seqLen = 4;
            }
            else
            {
                character = 0xFFFD;
            }
        }
        else
        {
            character = 0xFFFD; // Invalid byte, use replacement character
        }

        i += seqLen - 1;
        if (character == '\n')
        {
            x = 0.0;
            y -= 1.0 + lineSpacing;
            continue;
        }

        const FontGlyph* glyph = font->GetGlyph((u32)character);
        if (!glyph)
            continue;

        f32 planeL = (f32)(glyph->PlaneBoundsMin.x * scale + x);
        f32 planeB = (f32)(glyph->PlaneBoundsMin.y * scale + y);
        f32 planeR = (f32)(glyph->PlaneBoundsMax.x * scale + x);
        f32 planeT = (f32)(glyph->PlaneBoundsMax.y * scale + y);

        glm::vec2 texCoords[4] = {
            {glyph->UVMin.x, glyph->UVMax.y}, // Bottom Left
            {glyph->UVMax.x, glyph->UVMax.y}, // Bottom Right
            {glyph->UVMax.x, glyph->UVMin.y}, // Top Right
            {glyph->UVMin.x, glyph->UVMin.y}  // Top Left
        };

        glm::vec4 vertexPositions[4] = {{planeL, planeB, 0.0f, 1.0f},
                                        {planeR, planeB, 0.0f, 1.0f},
                                        {planeR, planeT, 0.0f, 1.0f},
                                        {planeL, planeT, 0.0f, 1.0f}};

        for (i32 v = 0; v < 4; v++)
        {
            s_Data.TextVertexBufferPtr->Position = transform * vertexPositions[v];
            s_Data.TextVertexBufferPtr->Color = color;
            s_Data.TextVertexBufferPtr->TexCoord = texCoords[v];
            s_Data.TextVertexBufferPtr->TexIndex = textureIndex;
            s_Data.TextVertexBufferPtr->EntityID = (f32)entityID;
            s_Data.TextVertexBufferPtr++;
        }

        s_Data.TextIndexCount += 6;
        s_Data.Stats.QuadCount++;

        x += (glyph->Advance * scale) + kerning;
    }
}

void Renderer2D::DrawSprite(const glm::mat4& transform, SpriteRendererComponent& src, int entityID)
{
    if (src.Texture)
        DrawQuad(transform, src.Texture, src.TilingFactor, src.Color, entityID);
    else
        DrawQuad(transform, src.Color, entityID);
}

void Renderer2D::ResetStats()
{
    memset(&s_Data.Stats, 0, sizeof(Statistics));
}

Renderer2D::Statistics Renderer2D::GetStats()
{
    return s_Data.Stats;
}
} // namespace UHE
