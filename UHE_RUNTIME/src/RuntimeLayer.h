#pragma once

#include <UHE.h>
#include <cstdint>
#include "UHE/AssetsManager/Project.h"

/**
 * @brief Plays a project's start scene with nothing of the editor around it.
 *
 * What a shipped game runs. The project comes from `-project <dir>`, or from
 * the folder next to the executable when it holds a Project.uheproject (an
 * exported build); `-scene <path>` overrides the start scene. The scene runs
 * as in the editor's Play mode and is drawn by SceneRenderer straight into the
 * window, through the scene's primary camera.
 */
class RuntimeLayer : public UHE::Layer
{
public:
    RuntimeLayer();

    void OnAttach() override;
    void OnDetach() override;
    void OnUpdate(UHE::Timestep ts) override;
    void OnEvent(UHE::Event& event) override;

private:
    /// @brief Finds, loads and mounts the project; @return false (logged) when there is none.
    bool LoadProject();
    /// @brief Loads \p path and starts it in play mode; @return false (logged) on failure.
    bool LoadScene(const std::filesystem::path& path);
    /// @brief Declares this frame's scene passes, ending in the backbuffer.
    void DeclareFrame(UHE::Timestep ts);

    UHE::ProjectInfo m_Project;
    UHE::Ref<UHE::Scene> m_Scene;
    float m_Time = 0.0f;
    uint32_t m_Width = 0;
    uint32_t m_Height = 0;
};
