#pragma once
#include "UHE/Core/Core.h"
#include "UHE/RHI/RHITypes.h"
#include "UHE/Renderer/ShaderReflection.h"

#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace UHE
{
/**
 * \brief A named preprocessor define set one shader source is compiled with.
 *
 * "One source per variant" is how a shader serves two pipelines that differ in
 * an output: `no_entity_id` is `Basic3D.slang` with `UHE_NO_ENTITY_ID=1`, which
 * the swapchain pass needs because a fragment output with no attachment is
 * invalid. The build and the runtime both read the list from `variants.txt` next
 * to the sources, so a define is written down once.
 */
struct ShaderVariant
{
    std::string Name = "default";
    std::vector<std::pair<std::string, std::string>> Defines;
};

/// \returns the variants declared in `<shaderDir>/variants.txt`.
///
/// A missing file yields the single `default` variant (no defines) rather than an
/// error: a project that needs no variants should not need the file either.
[[nodiscard]] std::vector<ShaderVariant> LoadShaderVariants(const std::filesystem::path& shaderDir);

/// \returns the file name of one stage's compiled artifact.
///
/// `<stem>.<stage>.<variant>.spv`, next to the source: the engine resolves assets
/// relative to the same root the `.slang` came from, so the artifact a build wrote
/// is the artifact the runtime reads, with no path table in between.
[[nodiscard]] std::filesystem::path ShaderArtifactPath(const std::filesystem::path& sourcePath,
                                                       const std::string& variant, RHI::ShaderStage stage);

/// Writes one stage's SPIR-V to \p path, creating its directory when needed.
[[nodiscard]] bool WriteShaderArtifact(const std::filesystem::path& path, const std::vector<u8>& spirv);

/// Reads one stage's SPIR-V from \p path. False when it is missing or empty.
[[nodiscard]] bool ReadShaderArtifact(const std::filesystem::path& path, std::vector<u8>& out);

/// One stage of one variant, compiled.
struct ShaderStageBinary
{
    RHI::ShaderStage Stage = RHI::ShaderStage::Vertex;
    std::vector<u8> SPIRV;
    ShaderReflection Reflection;
};

/**
 * \brief Loads shaders, and reloads them when their source changes.
 *
 * The normal path is the fast one: the build compiles every source to `.spv` next
 * to it, and this reads those bytes back -- no compiler, no session, no parse of
 * the shader source at startup. Compiling is what happens when there is no
 * artifact yet (a checkout that has not been built) or when hot reload sees an
 * edit; either way the result is written back out, so the next launch is fast
 * again.
 *
 * The reflection that comes out of the artifact is what lets a pipeline be built
 * for a shader the runtime never compiled: the vertex layout and the push
 * constant range are read from the module instead of being kept in step by hand.
 *
 * Device-free by design: it owns bytes and metadata, and tells listeners when
 * they are stale. Recreating pipelines is the renderer's job.
 */
class UHE_API ShaderCache
{
public:
    using Handle = u32; ///< 0 is invalid.

    struct Shader
    {
        std::string SourcePath;
        std::string Variant;
        std::vector<std::pair<std::string, std::string>> Defines;
        std::vector<ShaderStageBinary> Stages;
        /// True when the bytes came from the compiled artifact, false when this
        /// run had to compile them.
        bool FromArtifact = false;
        u32 ReloadCount = 0;
        std::filesystem::file_time_type SourceTime{};
        /// @brief Why the last compile failed; empty after a good one. The
        /// stages stay those of the last good compile, so a typo keeps drawing.
        std::string Error;

        [[nodiscard]] const ShaderStageBinary* FindStage(RHI::ShaderStage stage) const;
    };

    static ShaderCache& Get();

    /// Loads (or returns the already loaded) \p sourcePath compiled with \p variant.
    ///
    /// \returns 0 when the shader cannot be produced at all, which the caller must
    ///          treat as fatal for the pipeline it was building.
    Handle Load(const std::string& sourcePath, const std::string& variant = "default");

    [[nodiscard]] const Shader* Get(Handle handle) const;

    /// \returns every loaded handle, for a bulk reload.
    [[nodiscard]] std::vector<Handle> Handles() const;

    /// Recompiles \p handle from its source and rewrites its artifact.
    /// \returns false when the source no longer compiles.
    bool Reload(Handle handle);

    /// Polls the source mtimes of everything loaded and reloads what changed.
    /// \returns the handles that were reloaded (empty when nothing changed).
    std::vector<Handle> Poll();

    /// Recompiles everything from source, changed or not, and notifies once.
    ///
    /// For an explicit "reload shaders" command: a file that was restored, or a
    /// define that changed, is not something an mtime check can see.
    /// @return how many shaders failed to compile (0 means all reloaded).
    u32 ReloadAll();

    /// Enables or disables source watching. Off by default: a shipped build has
    /// no shader sources to watch.
    void SetHotReloadEnabled(bool enabled);
    [[nodiscard]] bool IsHotReloadEnabled() const { return m_HotReload; }

    /// `Poll()` at most a few times a second, so a frame loop can call this every
    /// frame without a stat() per shader per frame.
    void Tick();

    /// Notified after a shader's bytes changed, so the caller can rebuild the
    /// pipelines that were built from them.
    void AddReloadListener(std::function<void(Handle)> listener);

    /// @brief Notified when a reload fails to compile, with the shader and
    /// Slang's message, so the editor can show it rather than only log it.
    void AddFailureListener(std::function<void(Handle, const std::string&)> listener);

    /// Drops every loaded shader. For shutdown; the device must already be idle.
    void Clear();

private:
    /// How often `Tick()` looks at the sources.
    static constexpr double kPollIntervalSeconds = 0.25;

    struct Entry
    {
        Shader Data;
        std::filesystem::file_time_type ArtifactTime{};
    };

    bool LoadInto(Entry& entry, const std::filesystem::path& source, const ShaderVariant& variant);
    bool CompileInto(Entry& entry, const std::filesystem::path& source, const ShaderVariant& variant);
    static void RefreshTimestamps(Entry& entry);
    /// @brief Records a failed compile on \p handle and tells the failure listeners.
    void ReportFailure(Handle handle, Entry& entry);

    std::unordered_map<Handle, Entry> m_Shaders;
    std::vector<std::function<void(Handle)>> m_Listeners;
    std::vector<std::function<void(Handle, const std::string&)>> m_FailureListeners;
    Handle m_NextHandle = 1;
    bool m_HotReload = false;
    double m_LastPollSeconds = -1.0;
};
} // namespace UHE
