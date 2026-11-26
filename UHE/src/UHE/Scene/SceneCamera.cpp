#include "uhepch.h"
#include "SceneCamera.h"
#include <glm/gtc/matrix_transform.hpp>

namespace UHE {


	SceneCamera::~SceneCamera()
	{
	}

	void SceneCamera::SetOrthographic(f32 size, f32 nearClip, f32 farClip)
	{
        m_ProjectionType = ProjectionType::Orhtographic;

        m_OrhtographicSize = size;
		m_OrthographicNear = nearClip;
		m_OthergraphicFar = farClip;
		RecalculateProjection();
	}

	void SceneCamera::SetViewportSize(u32 width, u32 height)
	{
		m_AspactRatio = (f32)width / (f32)height;
		RecalculateProjection();
	}

	void SceneCamera::SetPerspective(f32 fov, f32 nearClip, f32 farClip)
	{
        m_ProjectionType = ProjectionType::Perpective;
        m_PerspectiveFOV = fov;
        m_PerspectiveNear = nearClip;
        m_PerspectiveFar = farClip;
        RecalculateProjection();
    }
	// Fix: Ensure m_Projection is of type glm::mat4 (or compatible with glm::ortho return type)
    // If not, update its declaration in SceneCamera.h to:
    // glm::mat4 m_Projection;

    // Example fix in SceneCamera.h (not shown here):
    // private:
    //     glm::mat4 m_Projection;

    void SceneCamera::RecalculateProjection()
    {
		if (m_ProjectionType == ProjectionType::Perpective)
		{
			m_Projection = glm::perspective(m_PerspectiveFOV, m_AspactRatio, m_PerspectiveNear, m_PerspectiveFar);
		}
		else
		{
			f32 orthoLeft = -m_OrhtographicSize * m_AspactRatio * 0.5f;
			f32 orthoRight = m_OrhtographicSize * m_AspactRatio * 0.5f;
			f32 orhtoBottom = -m_OrhtographicSize * 0.5f;
			f32 orthoTop = m_OrhtographicSize * 0.5f;

			m_Projection = glm::ortho
			(orthoLeft, orthoRight, orhtoBottom, orthoTop, m_OrthographicNear, m_OthergraphicFar);
		}



    }

}