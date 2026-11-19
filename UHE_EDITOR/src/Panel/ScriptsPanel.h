#pragma once
struct ImFont;

namespace UHE
{
class Scene;

/**
 * \brief The editor's view of every script attached to the open scene.
 *
 * An inspector shows the script on the entity you have selected; this shows the
 * ones you have not: which entity is running what, which instance a failed load
 * or a runtime error disabled, and how many times hot reload has restarted it.
 * That is the question a scripting workflow actually asks -- "why is nothing
 * moving" -- and it answers it without hunting through the hierarchy.
 */
class ScriptsPanel
{
public:
    void SetContext(Scene* scene) { m_Scene = scene; }

    /// Draws the panel. \p open is the window's visibility flag.
    void OnImGuiRender(bool& open);

private:
    Scene* m_Scene = nullptr;
    bool m_ShowHealthy = true;
};
} // namespace UHE
