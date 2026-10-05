// GPU-facing stubs for the scene test harness. Scene.cpp touches the renderer
// (2D/3D submission), fonts, and textures only through static interfaces; the
// scene logic under test never needs a real backend. Each stub mirrors the
// exact signature declared in the engine headers so the harness links the
// real Scene/Physics translation units.
#include "UHE/Renderer/Renderer2D.h"
#include "UHE/Renderer3D/Renderer3D.h"
#include "UHE/Renderer3D/LoadModel.h"
#include "UHE/Renderer3D/Animator.h"
#include "UHE/Renderer3D/LightSystem.h"
#include "UHE/Renderer/EditorCamera.h"
#include "UHE/Renderer2D/SubTexture2D.h"
#include "UHE/Renderer/Texture.h"
#include "UHE/Renderer/Font.h"

#include <mutex>

namespace UHE
{

// ---- Renderer2D (all submission paths; nothing is drawn headless) ----
void Renderer2D::Init() {}
void Renderer2D::Shutdown() {}
void Renderer2D::BeginScene(const Camera&, const glm::mat4&) {}
void Renderer2D::BeginScene(const EditorCamera&) {}
void Renderer2D::EndScene() {}
void Renderer2D::Flush() {}
void Renderer2D::DrawQuad(const glm::vec2&, const glm::vec2&, const glm::vec4&) {}
void Renderer2D::DrawQuad(const glm::vec3&, const glm::vec2&, const glm::vec4&) {}
void Renderer2D::DrawQuad(const glm::vec2&, const glm::vec2&, const Ref<Texture2D>&, float, const glm::vec4&) {}
void Renderer2D::DrawQuad(const glm::vec3&, const glm::vec2&, const Ref<Texture2D>&, float, const glm::vec4&) {}
void Renderer2D::DrawQuad(const glm::mat4&, const glm::vec4&, int) {}
void Renderer2D::DrawQuad(const glm::mat4&, const Ref<Texture2D>&, float, const glm::vec4&, int) {}
void Renderer2D::DrawQuad(const glm::mat4&, const Ref<SubTexture2D>&, float, const glm::vec4&, int) {}
void Renderer2D::DrawSprite(const glm::mat4&, SpriteRendererComponent&, int) {}
void Renderer2D::DrawString(const std::string&, Ref<Font2D>, const glm::mat4&, const glm::vec4&, float, float, int) {}
void Renderer2D::ResetStats() {}
Renderer2D::Statistics Renderer2D::GetStats() { return {}; }
void Renderer2D::StartBatch() {}
void Renderer2D::NextBatch() {}

// ---- Renderer3D ----
void Renderer3D::Init() {}
void Renderer3D::Shutdown() {}
Renderer3D::BoneBinding Renderer3D::PrepareBoneBinding(const RD3d::Animator*) { return {}; }
void Renderer3D::BeginScene(const EditorCamera&, const std::vector<RD3d::LightData>&) {}
void Renderer3D::BeginScene(const Camera&, const glm::mat4&, const std::vector<RD3d::LightData>&) {}
void Renderer3D::EndScene() {}
void Renderer3D::SubmitModel(const RD3d::Model&, const glm::mat4&, int, const RD3d::Animator*) {}
void Renderer3D::SubmitMesh(const RD3d::Mesh&, const glm::mat4&, int, const std::vector<RD3d::Material>&, int, int) {}
void Renderer3D::DrawGrid() {}
bool Renderer3D::IsLightingEnabled() { return false; }
void Renderer3D::SetLightingEnabled(bool) {}

// ---- Texture2D: pure factory in the real engine; the harness never creates
// GPU textures, so every factory returns null (callers null-check already). ----
Ref<Texture2D> Texture2D::Create(const std::string&, const RHI::SamplerDesc&) { return nullptr; }
Ref<Texture2D> Texture2D::Create(u32, u32, const RHI::SamplerDesc&) { return nullptr; }
Ref<Texture2D> Texture2D::CreateFromMemory(const void*, size_t, const RHI::SamplerDesc&) { return nullptr; }

// ---- Font2D: header declares an opaque Impl held via unique_ptr, so the
// destructor needs a complete definition of Impl in exactly one TU. ----
struct Font2D::Impl
{
};
Font2D::Font2D(const std::string&, u32, f32) {}
Font2D::~Font2D() = default;
Ref<Font2D> Font2D::Get(const std::string&, u32) { return nullptr; }
Ref<Font2D> Font2D::GetDefault() { return nullptr; }

// ---- EditorCamera: Scene only reads orientation for light-icon billboards
// (Renderer2D is stubbed, so the value is irrelevant). ----
EditorCamera::EditorCamera(float, float, float, float) {}
glm::quat EditorCamera::GetOrientation() const { return {}; }

// ---- SubTexture2D: constructed by SpriteAnimation::Tick paths. ----
SubTexture2D::SubTexture2D(const Ref<Texture2D>&, const glm::vec2&, const glm::vec2&) {}
Ref<SubTexture2D> SubTexture2D::CreateFromCoords(const Ref<Texture2D>&, const glm::vec2&, const glm::vec2, const glm::vec2&)
{
    return nullptr;
}
Ref<SubTexture2D> SubTexture2D::CreateFromPixels(const Ref<Texture2D>&, glm::vec2, glm::vec2) { return nullptr; }

} // namespace UHE

// ---- RD3d: animator/model symbols referenced from Scene's update loops.
// No models are loaded headless, so these are inert. ----
namespace UHE::RD3d
{
void Animator::UpdateAnimation(float) {}
std::vector<LightData> LightSystem::ExtractLights(entt::registry&) { return {}; }
Model::~Model()
{
    // Real dtor destroys GPU buffers via the device; headless tests never
    // load a model, so releasing the CPU-side vectors is enough.
    m_LoadedMeshes.clear();
    m_LoadedMaterials.clear();
    m_Nodes.clear();
    m_RootNodes.clear();
    m_NodeToMesh.clear();
}
} // namespace UHE::RD3d
