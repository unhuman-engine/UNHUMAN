#pragma once
#include <UHE.h>
#include "UHE/Renderer3D/LoadModel.h"
#include "UHE/Renderer/EditorCamera.h"

class ModelTestLayer : public UHE::Layer {
public:
    ModelTestLayer();
    virtual ~ModelTestLayer() = default;

    virtual void OnAttach() override;
    virtual void OnDetach() override;
    virtual void OnUpdate(UHE::Timestep ts) override;
    virtual void OnImGuiRender() override;
    virtual void OnEvent(UHE::Event& event) override;

private:
    UHE::RD3d::Model m_Model;
    UHE::EditorCamera m_Camera;
    float m_Rotation = 0.0f;
    UHE::RHI::TextureHandle m_DepthTexture = nullptr;
    /// The extent `m_DepthTexture` was created at: the attachment has to cover
    /// the frame's render area, which is the swapchain's extent.
    uint32_t m_DepthWidth = 0;
    uint32_t m_DepthHeight = 0;
};
