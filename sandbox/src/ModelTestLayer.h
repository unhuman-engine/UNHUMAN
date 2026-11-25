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
};
