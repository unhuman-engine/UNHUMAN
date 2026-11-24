#pragma once

#include <glm/glm.hpp>
#include "Camera.h"
#include "UHE/Core/Timestep.h"
#include "UHE/Events/Event.h"
#include "UHE/Events/MouseEvent.h"

namespace UHE
{

class UHE_API EditorCamera : public Camera
{
public:
    EditorCamera() = default;
    EditorCamera(float fov, float aspectRatio, float nearClip, float farClip);

    void OnUpdate(Timestep ts);
    void OnEvent(Event& e);

    inline float GetDistance() const { return m_Distance; }
    inline void SetDistance(float distance) { m_Distance = distance; }

    // Orbit pivot. The editor uses it to focus the selected entity (F).
    inline const glm::vec3& GetFocalPoint() const { return m_FocalPoint; }
    inline void SetFocalPoint(const glm::vec3& focalPoint)
    {
        m_FocalPoint = focalPoint;
        UpdateView();
    }
    // Recentres the pivot on 'point' and pulls back to 'distance'.
    void FocusOn(const glm::vec3& point, float distance);

    // Multiplies pan/rotate/zoom sensitivity. 1.0 is the built-in speed.
    inline float GetSpeedMultiplier() const { return m_SpeedMultiplier; }
    inline void SetSpeedMultiplier(float multiplier)
    {
        m_SpeedMultiplier = multiplier < 0.05f ? 0.05f : (multiplier > 20.0f ? 20.0f : multiplier);
    }

    inline void SetViewportSize(float width, float height)
    {
        m_ViewportWidth = width;
        m_ViewportHeight = height;
        UpdateProjection();
    }

    const glm::mat4& GetViewMatrix() const { return m_ViewMatrix; }
    glm::mat4 GetViewProjection() const { return m_Projection * m_ViewMatrix; }

    glm::vec3 GetUpDirection() const;
    glm::vec3 GetRightDirection() const;
    glm::vec3 GetForwardDirection() const;
    const glm::vec3& GetPosition() const { return m_Position; }
    glm::quat GetOrientation() const;

    float GetPitch() const { return m_Pitch; }
    float GetYaw() const { return m_Yaw; }
    inline void SetPitch(float pitch) { m_Pitch = pitch; }
    inline void SetYaw(float yaw) { m_Yaw = yaw; }

private:
    void UpdateProjection();
    void UpdateView();

    bool OnMouseScroll(MouseScrolledEvent& e);

    void MousePan(const glm::vec2& delta);
    void MouseRotate(const glm::vec2& delta);
    void MouseZoom(float delta);

    glm::vec3 CalculatePosition() const;

    std::pair<float, float> PanSpeed() const;
    float RotationSpeed() const;
    float ZoomSpeed() const;

private:
    float m_FOV = 45.0f, m_AspectRatio = 1.778f, m_NearClip = 0.1f, m_FarClip = 1000.0f;

    glm::mat4 m_ViewMatrix;
    glm::vec3 m_Position = {0.0f, 0.0f, 0.0f};
    glm::vec3 m_FocalPoint = {0.0f, 0.0f, 0.0f};

    glm::vec2 m_InitialMousePosition = {0.0f, 0.0f};

    float m_Distance = 10.0f;
    float m_SpeedMultiplier = 1.0f;
    float m_Pitch = 0.0f, m_Yaw = 0.0f;

    float m_ViewportWidth = 1280, m_ViewportHeight = 720;
};
} // namespace UHE
