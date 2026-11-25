#pragma once

#include "UHE/Core/Core.h"

namespace UHE
{
class Scene;

/**
 * @brief The "Environment" window: edits how the open scene is rendered, like Godot's WorldEnvironment.
 *
 * It edits the scene's RenderSettings in place, so a change shows in the
 * viewport on the next frame and is saved with the scene. Each group of
 * settings is a collapsible section with a Reset button, and a feature that
 * can be switched off carries its Enabled checkbox in the section header.
 */
class EnvironmentPanel
{
public:
    /// @brief Sets the scene whose settings the panel edits; null shows an empty panel.
    void SetContext(const Ref<Scene>& scene) { m_Scene = scene; }

    /// @brief Draws the panel. @p open is the window's visibility flag, cleared by its close button.
    void OnImGuiRender(bool& open);

private:
    Ref<Scene> m_Scene;
};
} // namespace UHE
