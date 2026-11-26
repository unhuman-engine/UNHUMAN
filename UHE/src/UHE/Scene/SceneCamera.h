#pragma once

#include "UHE/Core/Core.h"
#include "UHE/Renderer/Camera.h"

namespace UHE
{
class UHE_API SceneCamera final : public Camera
{
public:
    enum class ProjectionType
    {
        Perpective = 0,
        Orhtographic = 1
    };

    SceneCamera() = default;
    ~SceneCamera() override;

    void SetOrthographic(f32 size, f32 nearClip, f32 farClip);

    void SetViewportSize(u32 width, u32 height);

    void SetPerspective(f32 fov, f32 nearClip, f32 farClip);

    f32 GetOrthographicSize() const { return m_OrhtographicSize; }
    void SetOrthographicSize(f32 size)
    {
        m_OrhtographicSize = size;
        RecalculateProjection();
    }
    f32 GetOrthographicNearClip() const { return m_OrthographicNear; }
    void SetOrthoGraphicNearClip(f32 nearClip)
    {
        m_OrthographicNear = nearClip;
        RecalculateProjection();
    }
    f32 GetOrthographicFarClip() const { return m_OthergraphicFar; }
    void SetOrthoGraphicFarClip(f32 farClip)
    {
        m_OthergraphicFar = farClip;
        RecalculateProjection();
    }
    ProjectionType GetProjectionType() const { return m_ProjectionType; }

    f32 GetProjectionFOV() const { return m_PerspectiveFOV; }
    f32 GetProjectionNear() const { return m_PerspectiveNear; }
    /// @brief The perspective far plane (it used to return the orthographic one,
    ///        so every saved scene stored a perspective far plane of 1 m).
    f32 GetProjectionFar() const { return m_PerspectiveFar; }

    void SetPerspectiveFOV(f32 fov)
    {
        m_PerspectiveFOV = fov;
        RecalculateProjection();
    }
    void SetPerpecstiveNear(f32 nearClip)
    {
        m_PerspectiveNear = nearClip;
        RecalculateProjection();
    }
    void SetPerspecstiveFar(f32 farClip)
    {
        m_PerspectiveFar = farClip;
        RecalculateProjection();
    }

    void SetProjectionType(ProjectionType Type)
    {
        m_ProjectionType = Type;
        RecalculateProjection();
    }

private:
    void RecalculateProjection();

private:
    ProjectionType m_ProjectionType = ProjectionType::Orhtographic;
    f32 m_PerspectiveFOV = glm::radians(45.0f);
    f32 m_PerspectiveNear = 0.01f, m_PerspectiveFar = 1000.0f;

    f32 m_OrhtographicSize = 10.0f;
    f32 m_OrthographicNear = -1.0f, m_OthergraphicFar = 1.0f;

    f32 m_AspactRatio = 0;
};
} // namespace UHE
