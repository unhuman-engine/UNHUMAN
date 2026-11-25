#pragma once
#include <UHE.h>

namespace UHE::RHI
{
class FGContext;
}
#include <UHE/Renderer/EditorCamera.h>
#include <UHE/Renderer3D/Animator.h>
#include <UHE/Scene/Entity.h>
#include <string>
#include <vector>

class AimLabLayer : public UHE::Layer
{
public:
    AimLabLayer();
    virtual ~AimLabLayer() = default;

    void OnAttach() override;
    void OnDetach() override;
    void OnUpdate(UHE::Timestep ts) override;
    void OnImGuiRender() override;
    void OnEvent(UHE::Event& e) override;

private:
    void RespawnTarget(UHE::Entity target);
    void RecordScenePass(UHE::RHI::FGContext& context);

    UHE::Ref<UHE::Scene> m_ActiveScene;
    UHE::EditorCamera m_Camera;
    // Scene targets: created by the frame graph, exported for the UI and picking.
    static constexpr const char* kSceneColor = "AimLab.Color";
    static constexpr const char* kSceneEntity = "AimLab.Entity";
    u32 m_ViewportWidth = 0, m_ViewportHeight = 0;

    // ─── RenderGraph migration (M5 step 4): pending entity pickup ───
    // The entity-ID target is graph-owned; shots consume LAST frame's pixel
    // at the top of OnUpdate (pipelined readback, rendergraph.md §12.2).
    i32 m_PendingPickupX = 0;
    i32 m_PendingPickupY = 0;
    bool m_HasPendingPickup = false;

    UHE::Entity m_GunEntity;
    UHE::Ref<UHE::RD3d::Animator> m_GunAnimator;
    std::vector<UHE::Entity> m_Targets;

    // Shooting
    i32 m_Hits = 0;
    i32 m_Shots = 0;
    bool m_MouseWasPressed = false;

    f32 m_RecoilOffset = 0.0f;

    // Ammo & Reload
    static constexpr i32 MAX_AMMO = 7;
    i32 m_Ammo = MAX_AMMO;
    bool m_IsReloading = false;
    f32 m_ReloadTimer = 0.0f;
    f32 m_ReloadDuration = 3.5f; // will be overridden from animation data

    // Fire animation one-shot
    bool m_IsFireAnimPlaying = false;
    f32 m_FireAnimTimer = 0.0f;
    f32 m_FireAnimDuration = 0.5f; // will be overridden from animation data

    // Mouse look
    glm::vec2 m_LastMousePos{0.0f};
    bool m_CursorLocked = false;
    bool m_EscapeWasPressed = false;
    bool m_SkipMouseDelta = false;
    std::string m_GameAssetsPath;
};
