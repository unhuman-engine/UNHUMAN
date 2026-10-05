#include "AimLabLayer.h"
#include <Platform/Vulkan/RenderGraph/VulkanRenderGraph.h>
#include <Platform/Vulkan/RenderGraph/VulkanRenderGraphExecutor.h>
#include <Platform/Vulkan/VulkanDevice.h>
#include <Platform/Vulkan/VulkanTexture.h>
#include <UHE/Audio/AudioEngine.h>
#include <UHE/Core/Application.h>
#include <UHE/Core/input.h>
#include <UHE/Core/keyCodes.h>
#include <UHE/Core/mouseButtonCodes.h>
#include <UHE/RHI/RHICommandBuffer.h>
#include <UHE/RHI/RHIDevice.h>
#include <UHE/Renderer3D/LightSystem.h>
#include <UHE/Renderer3D/Renderer3D.h>
#include <UHE/Scene/Components.h>
#include <glm/gtx/quaternion.hpp>
#include <imgui.h>
#include <random>

// ─── RenderGraph feature-pass migration (docs/architecture/rendergraph.md §14
// step 5 / ROADMAP M5 step 4): this layer's scene pass declares against the
// frame graph instead of calling cmd.BeginRenderPass/EndRenderPass (whose
// legacy path hardcodes Undefined→attachment and attachment→ShaderRead
// transitions every frame). Sync is the compiler's contract now.
namespace
{
using UHE::RHI::LoadOp;
using UHE::RHI::StoreOp;
using UHE::RHI::VULKAN::ImageState;
using UHE::RHI::VULKAN::RGTextureHandle;

struct AimLabRGHandles
{
    RGTextureHandle color{};
    RGTextureHandle entityTarget{};
    RGTextureHandle depth{};
    bool declared = false;
};
AimLabRGHandles s_AimLabRG;
} // namespace

// File-scoped statics for live tweaking the gun model
static f32 s_GunScale = 0.13f;
static glm::vec3 s_GunOffsetRot = glm::vec3(-1.0f, -178.0f, -1.0f);
static glm::vec3 s_GunOffsetPos = glm::vec3(0.65f, 0.05f, 0.46f);

static std::mt19937 s_RNG(std::random_device{}());

static glm::vec3 RandomTargetPosition()
{
    std::uniform_real_distribution<f32> distX(-8.0f, 8.0f);
    std::uniform_real_distribution<f32> distY(-1.0f, 5.0f);
    return glm::vec3(distX(s_RNG), distY(s_RNG), 10.0f);
}

AimLabLayer::AimLabLayer() : Layer("AimLabLayer"), m_Camera(45.0f, 16.0f / 9.0f, 0.1f, 1000.0f) {}

void AimLabLayer::OnAttach()
{
    UHE_INFO("AimLabLayer Attached!");
    m_ActiveScene = UHE::CreateRef<UHE::Scene>();

    // Directional Light
    auto lightEntity = m_ActiveScene->CreateEntity("DirectionalLight");
    lightEntity.AddComponent<UHE::DirectionalLightComponent>();
    lightEntity.GetComponent<UHE::TransformComponent>().Rotation =
        glm::vec3(glm::radians(45.0f), glm::radians(45.0f), 0.0f);

    auto rootPath = UHE::FileSystem::Get().GetRootPath();
    if (fs::exists(rootPath.parent_path() / "UHEGAME" / "assets"))
    {
        // Dev environment (rootPath is UHE_EDITOR, parent is repo root)
        m_GameAssetsPath = (rootPath.parent_path() / "UHEGAME" / "assets").string();
    }
    else
    {
        // Standalone release environment (assets folder pasted next to executable)
        m_GameAssetsPath = (rootPath / "assets").string();
    }

    // Gun model (animated pistol with fire/reload animations)
    m_GunEntity = m_ActiveScene->CreateEntity("Gun");
    auto& gunModel = m_GunEntity.AddComponent<UHE::Model3DComponent>();
    gunModel.ModelPath = (fs::path(m_GameAssetsPath) / "models/pistol_animations_blender.glb").string();
    gunModel.IsLoaded = gunModel.ModelData->loadModel(gunModel.ModelPath);
    if (!gunModel.IsLoaded)
    {
        UHE_ERROR("Failed to load animated Gun model from path: {0}", gunModel.ModelPath);
    }
    else
    {
        // Set up the Animator for skeletal animation
        m_GunAnimator = UHE::CreateRef<UHE::RD3d::Animator>(gunModel.ModelData);

        // Log available animations and read their durations
        const auto& animations = gunModel.ModelData->GetAnimations();
        UHE_INFO("Gun model has {} animations:", animations.size());
        for (size_t i = 0; i < animations.size(); i++)
        {
            UHE_INFO("  [{}] {} (duration: {:.2f}s)", i, animations[i].Name, animations[i].Duration);
            // Read actual durations for fire and reload
            if (animations[i].Name == "Fire")
                m_FireAnimDuration = animations[i].Duration;
            else if (animations[i].Name == "Reload_Complete")
                m_ReloadDuration = animations[i].Duration;
        }

        m_GunAnimator->PlayAnimation("Idle");
    }

    m_GunEntity.GetComponent<UHE::TransformComponent>().Scale = glm::vec3(0.05f);

    // Spawn 5 targets at random positions on a "wall" at z=10
    m_Targets.resize(5);
    for (i32 i = 0; i < 5; i++)
    {
        m_Targets[i] = m_ActiveScene->CreateEntity("Target_" + std::to_string(i));
        auto& model = m_Targets[i].AddComponent<UHE::Model3DComponent>();
        model.ModelPath = (fs::path(m_GameAssetsPath) / "models/Sphere.glb").string();
        model.IsLoaded = model.ModelData->loadModel(model.ModelPath);
        if (!model.IsLoaded)
            UHE_ERROR("Failed to load target Box model");
        m_Targets[i].GetComponent<UHE::TransformComponent>().Translation = RandomTargetPosition();
    }

    // Camera setup: orbit point at the wall, close distance
    m_Camera.SetDistance(5.0f);

    // Framebuffer: RGBA8 (color) + RED_INTEGER (entity ID picking) + Depth
    UHE::FramebufferSpecification fbSpec;
    fbSpec.Attachments = {UHE::FramebufferTextureFormat::RGBA8, UHE::FramebufferTextureFormat::RED_INTEGER,
                          UHE::FramebufferTextureFormat::Depth};
    fbSpec.Width = 1920;
    fbSpec.Height = 1080;
    m_Framebuffer = UHE::Framebuffer::Create(fbSpec);

    // Capture mouse for FPS-style aiming
    UHE::Application::Get().GetWindow().SetCursorLocked(true);
    m_CursorLocked = true;
    m_LastMousePos = {UHE::Input::GetMouseX(), UHE::Input::GetMouseY()};
}

void AimLabLayer::OnDetach()
{
    if (m_CursorLocked)
    {
        UHE::Application::Get().GetWindow().SetCursorLocked(false);
        m_CursorLocked = false;
    }
}

void AimLabLayer::OnUpdate(UHE::Timestep ts)
{
    // === Resize ===
    u32 width = UHE::Application::Get().GetWindow().GetWidth();
    u32 height = UHE::Application::Get().GetWindow().GetHeight();
    if (m_ViewportWidth != width || m_ViewportHeight != height)
    {
        m_ViewportWidth = width;
        m_ViewportHeight = height;
        m_Framebuffer->Resize(width, height);
        m_Camera.SetViewportSize(static_cast<f32>(width), static_cast<f32>(height));
        m_ActiveScene->OnViewportResize(width, height);
    }

    // === Shooting (reads entity ID from PREVIOUS frame's framebuffer) ===
    bool mouseDown = UHE::Input::IsMouseButtonPressed(UHE_MOUSE_BUTTON_LEFT);
    bool altHeld = UHE::Input::IsKeyPressed(UHE::Key::LeftAlt);

    // ESC toggles cursor capture
    bool escPressed = UHE::Input::IsKeyPressed(UHE::Key::Escape);
    if (escPressed && !m_EscapeWasPressed)
    {
        m_CursorLocked = !m_CursorLocked;
        UHE::Application::Get().GetWindow().SetCursorLocked(m_CursorLocked);
        m_SkipMouseDelta = true;
    }
    m_EscapeWasPressed = escPressed;

    // === Reload timer ===
    if (m_IsReloading)
    {
        m_ReloadTimer -= static_cast<f32>(ts);
        if (m_ReloadTimer <= 0.0f)
        {
            m_IsReloading = false;
            m_Ammo = MAX_AMMO;
            // Go back to Idle after reload completes
            if (m_GunAnimator)
                m_GunAnimator->PlayAnimation("Idle");
        }
    }

    // === Fire animation one-shot timer ===
    if (m_IsFireAnimPlaying)
    {
        m_FireAnimTimer -= static_cast<f32>(ts);
        if (m_FireAnimTimer <= 0.0f)
        {
            m_IsFireAnimPlaying = false;
            // Return to Idle after fire animation finishes (unless reloading)
            if (!m_IsReloading && m_GunAnimator)
                m_GunAnimator->PlayAnimation("Idle");
        }
    }

    // === Manual reload with R key ===
    if (UHE::Input::IsKeyPressed(UHE::Key::R) && m_Ammo < MAX_AMMO && !m_IsReloading && m_GunAnimator)
    {
        m_IsReloading = true;
        m_IsFireAnimPlaying = false;
        m_ReloadTimer = m_ReloadDuration;
        m_GunAnimator->PlayAnimation("Reload_Complete");

        UHE::Audio::AudioEngine::PlaySound3D((fs::path(m_GameAssetsPath) / "audio/reload.wav").string(),
                                             m_GunEntity.GetComponent<UHE::TransformComponent>().Translation);
    }

    // ─── RenderGraph migration: consume LAST frame's pending pickup ───
    // Reads the entity-ID target for a shot whose scene pass completed last
    // frame (graph-owned target; see the shot handler below).
    if (m_HasPendingPickup)
    {
        m_HasPendingPickup = false;
        const i32 pixelData = m_Framebuffer->ReadPixel(1, m_PendingPickupX, m_PendingPickupY);
        if (pixelData >= 0)
        {
            for (auto& target : m_Targets)
            {
                if (static_cast<i32>(static_cast<entt::entity>(target)) == pixelData)
                {
                    m_Hits++;
                    RespawnTarget(target);
                    break;
                }
            }
        }
    }

    // Only shoot on click (not hold); crosshair is fixed at screen center
    if (mouseDown && !m_MouseWasPressed && m_CursorLocked && !m_IsReloading && m_Ammo > 0)
    {
        m_Shots++;
        m_Ammo--;

        // Play fire animation as a one-shot
        if (m_GunAnimator)
        {
            m_GunAnimator->PlayAnimation("Fire");
        }
        m_IsFireAnimPlaying = true;
        m_FireAnimTimer = m_FireAnimDuration;

        // Play 3D Audio
        UHE::Audio::AudioEngine::PlaySound3D((fs::path(m_GameAssetsPath) / "audio/gunshot.wav").string(),
                                             m_GunEntity.GetComponent<UHE::TransformComponent>().Translation);

        // Add a kick to the gun's pitch for recoil
        m_RecoilOffset = 6.0f;

        // ─── RenderGraph migration: entity pickup is now FRAME-DELAYED ───
        // The entity-ID target is graph-owned; the pixel for THIS shot exists
        // only after this frame's scene pass runs on the GPU. The pending
        // query is consumed at the top of the NEXT OnUpdate (rendergraph.md
        // §12.2: readback must be pipelined, not synchronous).
        m_PendingPickupX = static_cast<i32>(static_cast<f32>(m_ViewportWidth) * 0.5f);
        m_PendingPickupY = static_cast<i32>(static_cast<f32>(m_ViewportHeight) * 0.5f);
        m_HasPendingPickup = true;

        // Auto-reload when magazine is empty
        if (m_Ammo <= 0)
        {
            m_IsReloading = true;
            m_IsFireAnimPlaying = false;
            m_ReloadTimer = m_ReloadDuration;
            if (m_GunAnimator)
            {
                m_GunAnimator->PlayAnimation("Reload_Complete");
            }

            UHE::Audio::AudioEngine::PlaySound3D((fs::path(m_GameAssetsPath) / "audio/reload.wav").string(),
                                                 m_GunEntity.GetComponent<UHE::TransformComponent>().Translation);
        }
    }
    m_MouseWasPressed = mouseDown;

    // === Update gun animation ===
    if (m_GunAnimator)
        m_GunAnimator->UpdateAnimation(static_cast<f32>(ts));

    // === Mouse look (crosshair stays centered, view follows the mouse) ===
    if (m_CursorLocked && !altHeld)
    {
        glm::vec2 currentPos{UHE::Input::GetMouseX(), UHE::Input::GetMouseY()};
        glm::vec2 delta = currentPos - m_LastMousePos;
        m_LastMousePos = currentPos;

        // Ignore the first frame and any huge jump (e.g. cursor re-capture/warp)
        if (!m_SkipMouseDelta && glm::length(delta) < 200.0f)
        {
            constexpr f32 sensitivity = 0.0024f; // radians per pixel
            f32 yawSign = m_Camera.GetUpDirection().y < 0.0f ? -1.0f : 1.0f;
            m_Camera.SetYaw(m_Camera.GetYaw() + yawSign * delta.x * sensitivity);

            f32 pitch = glm::clamp(m_Camera.GetPitch() + delta.y * sensitivity, -glm::half_pi<f32>() * 0.995f,
                                   glm::half_pi<f32>() * 0.995f);
            m_Camera.SetPitch(pitch);
        }
        m_SkipMouseDelta = false;
    }

    // === Camera Update ===
    m_Camera.OnUpdate(ts);

    // Update 3D Audio Listener
    UHE::Audio::AudioEngine::SetListenerPosition(m_Camera.GetPosition(), m_Camera.GetForwardDirection(),
                                                 m_Camera.GetUpDirection());

    auto& gunTransform = m_GunEntity.GetComponent<UHE::TransformComponent>();
    gunTransform.Scale = glm::vec3(s_GunScale);

    gunTransform.Translation = m_Camera.GetPosition() + m_Camera.GetForwardDirection() * s_GunOffsetPos.x +
                               m_Camera.GetUpDirection() * s_GunOffsetPos.y +
                               m_Camera.GetRightDirection() * s_GunOffsetPos.z;

    // Decay recoil smoothly back to zero
    m_RecoilOffset = glm::mix(m_RecoilOffset, 0.0f, glm::clamp(10.0f * static_cast<f32>(ts), 0.0f, 1.0f));

    glm::mat4 viewInverse = glm::inverse(m_Camera.GetViewMatrix());
    glm::quat camOrientation = glm::quat_cast(viewInverse);
    // Add recoil as a pitch rotation in camera space, so it's always "up" on screen
    // regardless of the gun model's internal axis orientations
    glm::quat recoilRot = glm::angleAxis(glm::radians(m_RecoilOffset), glm::vec3(1.0f, 0.0f, 0.0f));
    glm::quat modelFix = glm::quat(
        glm::vec3(glm::radians(s_GunOffsetRot.x), glm::radians(s_GunOffsetRot.y), glm::radians(s_GunOffsetRot.z)));

    gunTransform.Rotation = glm::eulerAngles(
        camOrientation * recoilRot * modelFix); // === Scene pass, declared against the frame graph (rendergraph.md §14

    // Undefined→attachment transitions happen as pre-barriers from TopOfPipe,
    // and EndRenderPass's hardcoded →ShaderRead exits are gone (the ImGui
    // pass declares its reads instead). ===
    auto& device = UHE::Renderer::GetDevice();
    auto* vulkanDevice = dynamic_cast<UHE::RHI::VULKAN::VulkanDevice*>(&device);
    if (vulkanDevice == nullptr)
        return; // no Vulkan device — nothing to declare against

    auto& executor = vulkanDevice->GetRenderGraphExecutor();
    auto& graph = vulkanDevice->GetFrameGraph();

    const u32 fbWidth = m_Framebuffer->GetSpecification().Width;
    const u32 fbHeight = m_Framebuffer->GetSpecification().Height;

    // ── Resources: persistent RG handles for the framebuffer attachments.
    // Re-imported every frame (imported slots are identity-keyed by name, so
    // the handles stay stable across Reset() cycles).
    s_AimLabRG.declared = true;
    s_AimLabRG.color =
        graph.ImportTexture("AimLab.Color", 1, fbWidth, fbHeight, 1, ImageState::Undefined, ImageState::ShaderRead);
    s_AimLabRG.entityTarget =
        graph.ImportTexture("AimLab.Entity", 1, fbWidth, fbHeight, 1, ImageState::Undefined, ImageState::ShaderRead);
    s_AimLabRG.depth =
        graph.ImportTexture("AimLab.Depth", 1, fbWidth, fbHeight, 1, ImageState::Undefined, ImageState::Undefined);

    // ── Pass declaration: Write + attachments (§7), then defer recording.
    auto& scenePass = graph.AddPass("AimLab.Scene", UHE::RHI::VULKAN::RGPassType::Graphics);
    scenePass.Write(s_AimLabRG.color)
        .Write(s_AimLabRG.entityTarget)
        .Write(s_AimLabRG.depth)
        .Color({s_AimLabRG.color, LoadOp::Clear, StoreOp::Store, {0.1f, 0.1f, 0.15f, 1.0f}})
        .Color({s_AimLabRG.entityTarget, LoadOp::Clear, StoreOp::Store, {-1.0f, -1.0f, -1.0f, -1.0f}})
        .Depth({s_AimLabRG.depth, LoadOp::Clear, StoreOp::Store, 1.0f, 0})
        .Execute([this](UHE::RHI::VULKAN::RGPassContext& context) { RecordScenePass(context); });

    // ── Live Vulkan objects registered so the executor can resolve barriers
    // and attachments (image/view/format from the live texture; the entity
    // target's R32_SINT format drives its int-typed clear value).
    vulkanDevice->RegisterFrameTexture(s_AimLabRG.color, m_Framebuffer->GetColorAttachments()[0]);
    vulkanDevice->RegisterFrameTexture(s_AimLabRG.entityTarget, m_Framebuffer->GetColorAttachments()[1]);
    vulkanDevice->RegisterFrameTexture(s_AimLabRG.depth, m_Framebuffer->GetDepthAttachment());

    // Viewport/scissor are recorded inside the pass callback (they are pass
    // state, not graph state).
}

void AimLabLayer::RecordScenePass(UHE::RHI::VULKAN::RGPassContext& context)
{
    // Runs inside the executor-opened scope: transitions/scope are the
    // compiler's/executor's, this records draws only (§8.4 contract).
    const vk::CommandBuffer cmd = context.Cmd();

    const vk::Extent2D extent{m_Framebuffer->GetSpecification().Width, m_Framebuffer->GetSpecification().Height};
    const vk::Viewport viewport{0.0f, 0.0f, static_cast<f32>(extent.width), static_cast<f32>(extent.height),
                                0.0f, 1.0f};
    cmd.setViewport(0, viewport, context.Dispatcher());
    cmd.setScissor(0, vk::Rect2D{vk::Offset2D{0, 0}, extent}, context.Dispatcher());

    // Render scene (same draw stream as before; Renderer3D now records into
    // the pass's command buffer through the RHI facade).
    auto lights = UHE::RD3d::LightSystem::ExtractLights(m_ActiveScene->GetRegistry());
    UHE::Renderer3D::BeginScene(m_Camera, lights);

    auto view = m_ActiveScene->GetRegistry().view<UHE::TransformComponent, UHE::Model3DComponent>();
    for (auto entity : view)
    {
        auto [transform, model] = view.get<UHE::TransformComponent, UHE::Model3DComponent>(entity);
        if (model.IsLoaded)
        {
            // Pass animator for the gun entity so skeletal animation works
            const UHE::RD3d::Animator* animator = nullptr;
            if (entity == static_cast<entt::entity>(m_GunEntity) && m_GunAnimator)
                animator = m_GunAnimator.get();
            UHE::Renderer3D::SubmitModel(*model.ModelData, transform.GetTransform(), static_cast<i32>(entity),
                                         animator);
        }
    }
    UHE::Renderer3D::EndScene();
}

void AimLabLayer::RespawnTarget(UHE::Entity target)
{
    target.GetComponent<UHE::TransformComponent>().Translation = RandomTargetPosition();
}

void AimLabLayer::OnImGuiRender()
{
    // === Blit framebuffer to full screen ===
    void* texInfo = m_Framebuffer->GetColorAttachmentRendererID(0);
    ImGui::GetBackgroundDrawList()->AddImage(
        texInfo, ImVec2(0, 0), ImVec2(static_cast<f32>(m_ViewportWidth), static_cast<f32>(m_ViewportHeight)),
        ImVec2(0, 1), ImVec2(1, 0));

    // === Crosshair fixed at screen center ===
    ImVec2 crosshairPos(static_cast<f32>(m_ViewportWidth) * 0.5f, static_cast<f32>(m_ViewportHeight) * 0.5f);
    auto* fg = ImGui::GetForegroundDrawList();
    f32 size = 12.0f;
    f32 thick = 2.0f;
    f32 gap = 4.0f;
    ImU32 green = IM_COL32(0, 255, 70, 230);
    ImU32 outline = IM_COL32(0, 0, 0, 180);

    // Outer lines (with gap in center)
    // Horizontal
    fg->AddLine(ImVec2(crosshairPos.x - size, crosshairPos.y), ImVec2(crosshairPos.x - gap, crosshairPos.y), outline,
                thick + 2.0f);
    fg->AddLine(ImVec2(crosshairPos.x + gap, crosshairPos.y), ImVec2(crosshairPos.x + size, crosshairPos.y), outline,
                thick + 2.0f);
    fg->AddLine(ImVec2(crosshairPos.x - size, crosshairPos.y), ImVec2(crosshairPos.x - gap, crosshairPos.y), green,
                thick);
    fg->AddLine(ImVec2(crosshairPos.x + gap, crosshairPos.y), ImVec2(crosshairPos.x + size, crosshairPos.y), green,
                thick);
    // Vertical
    fg->AddLine(ImVec2(crosshairPos.x, crosshairPos.y - size), ImVec2(crosshairPos.x, crosshairPos.y - gap), outline,
                thick + 2.0f);
    fg->AddLine(ImVec2(crosshairPos.x, crosshairPos.y + gap), ImVec2(crosshairPos.x, crosshairPos.y + size), outline,
                thick + 2.0f);
    fg->AddLine(ImVec2(crosshairPos.x, crosshairPos.y - size), ImVec2(crosshairPos.x, crosshairPos.y - gap), green,
                thick);
    fg->AddLine(ImVec2(crosshairPos.x, crosshairPos.y + gap), ImVec2(crosshairPos.x, crosshairPos.y + size), green,
                thick);
    // Center dot
    fg->AddCircleFilled(crosshairPos, 2.0f, green);

    // === Dashboard ===
    ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.7f);
    ImGui::Begin("Aim Lab", nullptr, ImGuiWindowFlags_AlwaysAutoResize);

    f32 accuracy = m_Shots > 0 ? static_cast<f32>(m_Hits) / static_cast<f32>(m_Shots) * 100.0f : 0.0f;

    ImGui::Text("FPS: %.0f", ImGui::GetIO().Framerate);
    ImGui::Separator();

    // Score display
    ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), "HITS: %d", m_Hits);
    ImGui::SameLine();
    ImGui::Text("/ %d shots", m_Shots);

    // Accuracy bar
    ImGui::Text("Accuracy:");
    ImGui::SameLine();
    if (accuracy >= 70.0f)
        ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), "%.1f%%", accuracy);
    else if (accuracy >= 40.0f)
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.3f, 1.0f), "%.1f%%", accuracy);
    else
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%.1f%%", accuracy);

    ImGui::ProgressBar(accuracy / 100.0f, ImVec2(200, 0));

    // Ammo display
    ImGui::Separator();
    if (m_IsReloading)
    {
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.0f, 1.0f), "RELOADING...");
        ImGui::ProgressBar(1.0f - (m_ReloadTimer / m_ReloadDuration), ImVec2(200, 0));
    }
    else
    {
        ImGui::Text("Ammo:");
        ImGui::SameLine();
        if (m_Ammo > 3)
            ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), "%d / %d", m_Ammo, MAX_AMMO);
        else if (m_Ammo > 0)
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.0f, 1.0f), "%d / %d", m_Ammo, MAX_AMMO);
        else
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "EMPTY - Press R to reload");
    }

    ImGui::Separator();
    ImGui::TextDisabled("Mouse: Aim");
    ImGui::TextDisabled("Left Click: Shoot");
    ImGui::TextDisabled("R: Reload");
    ImGui::TextDisabled("ESC: Release / Lock Cursor");
    ImGui::TextDisabled("ALT + Mouse: Orbit Camera");
    ImGui::TextDisabled("Scroll: Zoom");

    // Live Tweaking for Gun Model
    ImGui::Separator();
    ImGui::Text("Gun Tweak Settings:");
    ImGui::DragFloat("Scale", &s_GunScale, 0.01f, 0.01f, 10.0f);
    ImGui::DragFloat3("Pos Offset (Fwd, Up, Right)", &s_GunOffsetPos.x, 0.01f);
    ImGui::DragFloat3("Rot Offset (Pitch, Yaw, Roll)", &s_GunOffsetRot.x, 1.0f);

    ImGui::End();
}

void AimLabLayer::OnEvent(UHE::Event& e)
{
    m_Camera.OnEvent(e);
}
