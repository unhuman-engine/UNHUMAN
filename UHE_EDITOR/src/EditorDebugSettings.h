#pragma once

namespace UHE::EditorDebugSettings
{

/**
 * @brief The editor's GPU debugging preferences, in editor_debug.ini.
 *
 * They apply to the next launch: synchronization validation and RenderDoc
 * are fixed when the Vulkan instance is created. Load() runs in
 * CreateApplication, before the Application exists, and copies the saved
 * choices into GetGpuDebugOptions().
 */
void Load();

/// @brief Writes the current requests of GetGpuDebugOptions(); a read-only directory is ignored.
void Save();

/// @brief Draws the GPU Debugging rows of the preferences.
void Draw();

} // namespace UHE::EditorDebugSettings
