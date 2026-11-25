#pragma once

#include <UHE.h>
#include <cstdint>
#include <optional>
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
    /// @param project What FindProject found; nothing leaves the window empty (the error is logged).
    explicit RuntimeLayer(std::optional<UHE::ProjectInfo> project);

    /// @brief Finds and loads the project to play; nothing (logged) when there is none.
    [[nodiscard]] static std::optional<UHE::ProjectInfo> FindProject();
    /**
     * @brief The window \p project asks for, with the command line's overrides:
     *        -windowed, -borderless, -fullscreen, -width <px>, -height <px>.
     */
    [[nodiscard]] static UHE::ProjectWindowSettings ResolveWindow(const UHE::ProjectInfo* project);

    void OnAttach() override;
    void OnDetach() override;
    void OnUpdate(UHE::Timestep ts) override;
    void OnEvent(UHE::Event& event) override;

private:
    /// @brief Loads \p path and starts it in play mode; @return false (logged) on failure.
    bool LoadScene(const std::filesystem::path& path);
    /// @brief Declares this frame's scene passes, ending in the backbuffer.
    void DeclareFrame(UHE::Timestep ts);

    std::optional<UHE::ProjectInfo> m_Project;
    UHE::Ref<UHE::Scene> m_Scene;
    float m_Time = 0.0f;
    uint32_t m_Width = 0;
    uint32_t m_Height = 0;
};
