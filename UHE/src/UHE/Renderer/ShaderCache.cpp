#include "uhepch.h"
#include "ShaderCache.h"

#include "UHE/Renderer/SlangCompiler.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <system_error>

namespace UHE
{
namespace
{
/// The stages a source can produce, in the order the engine binds them. The entry
/// point names are fixed by the engine (see AGENTS.md): one source declares any
/// subset of them.
constexpr RHI::ShaderStage kStages[] = {RHI::ShaderStage::Vertex, RHI::ShaderStage::Fragment,
                                        RHI::ShaderStage::Compute};

const char* StageName(RHI::ShaderStage stage)
{
    switch (stage)
    {
        case RHI::ShaderStage::Vertex:
            return "vertex";
        case RHI::ShaderStage::Fragment:
            return "fragment";
        case RHI::ShaderStage::Compute:
            return "compute";
        default:
            return "all";
    }
}

std::string Trim(std::string text)
{
    const auto notSpace = [](unsigned char c) { return std::isspace(c) == 0; };
    text.erase(text.begin(), std::find_if(text.begin(), text.end(), notSpace));
    text.erase(std::find_if(text.rbegin(), text.rend(), notSpace).base(), text.end());
    return text;
}

std::filesystem::file_time_type ModificationTime(const std::filesystem::path& path)
{
    std::error_code ec;
    const auto time = std::filesystem::last_write_time(path, ec);
    return ec ? std::filesystem::file_time_type{} : time;
}

std::string Describe(const std::filesystem::path& path)
{
    return path.filename().string();
}
} // namespace

std::vector<ShaderVariant> LoadShaderVariants(const std::filesystem::path& shaderDir)
{
    std::vector<ShaderVariant> variants;

    const std::filesystem::path manifest = shaderDir / "variants.txt";
    std::ifstream file(manifest);
    if (file)
    {
        std::string line;
        while (std::getline(file, line))
        {
            const std::string text = Trim(line);
            if (text.empty() || text[0] == '#')
            {
                continue;
            }

            ShaderVariant variant;
            const std::size_t colon = text.find(':');
            variant.Name = Trim(colon == std::string::npos ? text : text.substr(0, colon));
            if (variant.Name.empty())
            {
                continue;
            }

            if (colon != std::string::npos)
            {
                std::string rest = text.substr(colon + 1);
                std::size_t start = 0;
                while (start <= rest.size())
                {
                    const std::size_t semicolon = rest.find(';', start);
                    const std::string define =
                        Trim(rest.substr(start, semicolon == std::string::npos ? std::string::npos
                                                                              : semicolon - start));
                    if (!define.empty())
                    {
                        const std::size_t equals = define.find('=');
                        if (equals == std::string::npos)
                        {
                            variant.Defines.emplace_back(define, "1");
                        }
                        else
                        {
                            variant.Defines.emplace_back(Trim(define.substr(0, equals)), Trim(define.substr(equals + 1)));
                        }
                    }
                    if (semicolon == std::string::npos)
                    {
                        break;
                    }
                    start = semicolon + 1;
                }
            }

            variants.push_back(std::move(variant));
        }
    }

    if (variants.empty())
    {
        variants.push_back(ShaderVariant{});
    }

    // A missing `default` would make Load(path) -- the common case -- fail, so it
    // always exists, even in a project that only wrote down the other variants.
    const bool hasDefault = std::any_of(variants.begin(), variants.end(),
                                        [](const ShaderVariant& variant) { return variant.Name == "default"; });
    if (!hasDefault)
    {
        variants.insert(variants.begin(), ShaderVariant{});
    }
    return variants;
}

std::filesystem::path ShaderArtifactPath(const std::filesystem::path& sourcePath, const std::string& variant,
                                         RHI::ShaderStage stage)
{
    const std::string name =
        sourcePath.stem().string() + "." + StageName(stage) + "." + variant + ".spv";
    return sourcePath.parent_path() / name;
}

bool WriteShaderArtifact(const std::filesystem::path& path, const std::vector<u8>& spirv)
{
    if (spirv.empty())
    {
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);

    // Write beside the target and rename, so a reader never sees half a module:
    // hot reload rewrites these files while the engine is running.
    const std::filesystem::path temporary = path.string() + ".tmp";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file)
        {
            return false;
        }
        file.write(reinterpret_cast<const char*>(spirv.data()), static_cast<std::streamsize>(spirv.size()));
        if (!file.good())
        {
            return false;
        }
    }

    std::filesystem::rename(temporary, path, ec);
    if (ec)
    {
        std::filesystem::remove(temporary, ec);
        return false;
    }
    return true;
}

bool ReadShaderArtifact(const std::filesystem::path& path, std::vector<u8>& out)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        return false;
    }

    out.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    return !out.empty();
}

const ShaderStageBinary* ShaderCache::Shader::FindStage(RHI::ShaderStage stage) const
{
    for (const ShaderStageBinary& binary : Stages)
    {
        if (binary.Stage == stage)
        {
            return &binary;
        }
    }
    return nullptr;
}

ShaderCache& ShaderCache::Get()
{
    static ShaderCache library;
    return library;
}

ShaderCache::Handle ShaderCache::Load(const std::string& sourcePath, const std::string& variant)
{
    const std::filesystem::path source(sourcePath);

    for (const auto& [handle, entry] : m_Shaders)
    {
        if (entry.Data.SourcePath == source.string() && entry.Data.Variant == variant)
        {
            return handle;
        }
    }

    const std::vector<ShaderVariant> variants = LoadShaderVariants(source.parent_path());
    const auto variantIt = std::find_if(variants.begin(), variants.end(), [&](const ShaderVariant& candidate)
                                        { return candidate.Name == variant; });
    if (variantIt == variants.end())
    {
        UHE_CORE_ERROR("Shader '{0}': variant '{1}' is not declared in variants.txt", Describe(source), variant);
        return 0;
    }

    const Handle handle = m_NextHandle++;
    Entry& entry = m_Shaders[handle];
    if (!LoadInto(entry, source, *variantIt))
    {
        m_Shaders.erase(handle);
        return 0;
    }
    return handle;
}

const ShaderCache::Shader* ShaderCache::Get(Handle handle) const
{
    const auto it = m_Shaders.find(handle);
    return it == m_Shaders.end() ? nullptr : &it->second.Data;
}

std::vector<ShaderCache::Handle> ShaderCache::Handles() const
{
    std::vector<Handle> handles;
    handles.reserve(m_Shaders.size());
    for (const auto& [handle, entry] : m_Shaders)
    {
        (void)entry;
        handles.push_back(handle);
    }
    return handles;
}

bool ShaderCache::LoadInto(Entry& entry, const std::filesystem::path& source, const ShaderVariant& variant)
{
    entry.Data.SourcePath = source.string();
    entry.Data.Variant = variant.Name;
    entry.Data.Defines = variant.Defines;

    const std::filesystem::file_time_type sourceTime = ModificationTime(source);

    // Fast path: the build already compiled this variant and the source has not
    // been touched since. Reading a file is the whole cost.
    std::vector<ShaderStageBinary> fromArtifact;
    bool anyArtifact = false;
    bool staleArtifact = false;
    for (const RHI::ShaderStage stage : kStages)
    {
        const std::filesystem::path path = ShaderArtifactPath(source, variant.Name, stage);
        std::vector<u8> bytes;
        if (!ReadShaderArtifact(path, bytes))
        {
            continue;
        }
        anyArtifact = true;
        if (sourceTime > ModificationTime(path))
        {
            staleArtifact = true;
        }

        ShaderStageBinary binary;
        binary.Stage = stage;
        binary.SPIRV = std::move(bytes);
        binary.Reflection = ReflectSPIRV(binary.SPIRV);
        fromArtifact.push_back(std::move(binary));
    }

    if (anyArtifact && !staleArtifact)
    {
        entry.Data.Stages = std::move(fromArtifact);
        entry.Data.FromArtifact = true;
        RefreshTimestamps(entry);
        UHE_CORE_INFO("Shader '{0}' [{1}]: {2} stage(s) loaded from .spv", Describe(source), variant.Name,
                      entry.Data.Stages.size());
        return !entry.Data.Stages.empty();
    }

    return CompileInto(entry, source, variant);
}

bool ShaderCache::CompileInto(Entry& entry, const std::filesystem::path& source, const ShaderVariant& variant)
{
    UHE_CORE_INFO("Shader '{0}' [{1}]: compiling", Describe(source), variant.Name);

    std::string diagnostics;
    const auto compiled = SlangCompiler::CompileToSPIRV(source.string(), variant.Defines, &diagnostics);
    if (compiled.empty())
    {
        UHE_CORE_ERROR("Shader '{0}' [{1}] did not compile to any stage", Describe(source), variant.Name);
        entry.Data.Error = diagnostics.empty() ? "did not compile to any stage" : diagnostics;
        return false;
    }
    entry.Data.Error.clear();

    entry.Data.Stages.clear();
    entry.Data.FromArtifact = false;
    for (const RHI::ShaderStage stage : kStages)
    {
        const auto it = compiled.find(stage);
        if (it == compiled.end())
        {
            continue;
        }

        ShaderStageBinary binary;
        binary.Stage = stage;
        binary.SPIRV = it->second;
        binary.Reflection = ReflectSPIRV(binary.SPIRV);
        if (!binary.Reflection.Valid)
        {
            UHE_CORE_WARN("Shader '{0}' [{1}] {2}: reflection failed: {3}", Describe(source), variant.Name,
                          StageName(stage), binary.Reflection.Error);
        }

        // Write it back so the next launch takes the fast path; a read-only asset
        // folder only costs the compile, it is not an error.
        const std::filesystem::path path = ShaderArtifactPath(source, variant.Name, stage);
        if (!WriteShaderArtifact(path, binary.SPIRV))
        {
            UHE_CORE_WARN("Shader '{0}': could not write '{1}'", Describe(source), path.string());
        }

        entry.Data.Stages.push_back(std::move(binary));
    }

    RefreshTimestamps(entry);
    return !entry.Data.Stages.empty();
}

void ShaderCache::RefreshTimestamps(Entry& entry)
{
    entry.Data.SourceTime = ModificationTime(entry.Data.SourcePath);
    entry.ArtifactTime = std::filesystem::file_time_type{};
    for (const ShaderStageBinary& binary : entry.Data.Stages)
    {
        const std::filesystem::path path =
            ShaderArtifactPath(entry.Data.SourcePath, entry.Data.Variant, binary.Stage);
        entry.ArtifactTime = std::max(entry.ArtifactTime, ModificationTime(path));
    }
}

bool ShaderCache::Reload(Handle handle)
{
    const auto it = m_Shaders.find(handle);
    if (it == m_Shaders.end())
    {
        return false;
    }

    Entry& entry = it->second;
    const std::filesystem::path source(entry.Data.SourcePath);
    const ShaderVariant variant{.Name = entry.Data.Variant, .Defines = entry.Data.Defines};

    if (!CompileInto(entry, source, variant))
    {
        ReportFailure(handle, entry);
        return false;
    }

    ++entry.Data.ReloadCount;
    UHE_CORE_INFO("Shader '{0}' [{1}] reloaded ({2} stage(s), {3} reload(s))", Describe(source), variant.Name,
                  entry.Data.Stages.size(), entry.Data.ReloadCount);

    for (const auto& listener : m_Listeners)
    {
        listener(handle);
    }
    return true;
}

std::vector<ShaderCache::Handle> ShaderCache::Poll()
{
    std::vector<Handle> reloaded;

    for (auto& [handle, entry] : m_Shaders)
    {
        const std::filesystem::path source(entry.Data.SourcePath);

        // An edit is compiled here and now; that is the whole point of hot reload,
        // and the artifact it writes keeps the next launch on the fast path.
        if (ModificationTime(source) > entry.Data.SourceTime)
        {
            if (Reload(handle))
            {
                reloaded.push_back(handle);
            }
            continue;
        }

        // A build regenerated the artifact under a running engine: read it back
        // rather than compiling a second time.
        std::filesystem::file_time_type newest{};
        for (const RHI::ShaderStage stage : kStages)
        {
            newest = std::max(newest, ModificationTime(ShaderArtifactPath(source, entry.Data.Variant, stage)));
        }
        if (newest > entry.ArtifactTime)
        {
            Entry replacement;
            if (LoadInto(replacement, source, ShaderVariant{.Name = entry.Data.Variant,
                                                            .Defines = entry.Data.Defines}))
            {
                replacement.Data.ReloadCount = entry.Data.ReloadCount + 1;
                entry = std::move(replacement);
                UHE_CORE_INFO("Shader '{0}' [{1}] reloaded from the rebuilt artifact",
                              Describe(source), entry.Data.Variant);
                for (const auto& listener : m_Listeners)
                {
                    listener(handle);
                }
                reloaded.push_back(handle);
            }
        }
    }

    return reloaded;
}

u32 ShaderCache::ReloadAll()
{
    u32 failed = 0;
    for (Handle handle : Handles())
    {
        const auto it = m_Shaders.find(handle);
        if (it == m_Shaders.end())
        {
            continue;
        }

        Entry& entry = it->second;
        if (CompileInto(entry, entry.Data.SourcePath, ShaderVariant{.Name = entry.Data.Variant,
                                                                   .Defines = entry.Data.Defines}))
        {
            ++entry.Data.ReloadCount;
        }
        else
        {
            ReportFailure(handle, entry);
            ++failed;
        }
    }

    // One notification, not one per shader: a listener rebuilds the pipelines it
    // owns, and doing that per shader would rebuild them N times.
    for (const auto& listener : m_Listeners)
    {
        listener(0);
    }
    return failed;
}

void ShaderCache::ReportFailure(Handle handle, Entry& entry)
{
    // The failed source counts as seen: polling must wait for the next edit
    // instead of recompiling (and reporting) the same error four times a second.
    entry.Data.SourceTime = ModificationTime(entry.Data.SourcePath);
    for (const auto& listener : m_FailureListeners)
    {
        listener(handle, entry.Data.Error);
    }
}

void ShaderCache::AddFailureListener(std::function<void(Handle, const std::string&)> listener)
{
    m_FailureListeners.push_back(std::move(listener));
}

void ShaderCache::SetHotReloadEnabled(bool enabled)
{
    m_HotReload = enabled;
    m_LastPollSeconds = -1.0;
    UHE_CORE_INFO("Shader hot reload {0}", enabled ? "enabled" : "disabled");
}

void ShaderCache::Tick()
{
    if (!m_HotReload || m_Shaders.empty())
    {
        return;
    }

    const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (m_LastPollSeconds >= 0.0 && now - m_LastPollSeconds < kPollIntervalSeconds)
    {
        return;
    }
    m_LastPollSeconds = now;
    Poll();
}

void ShaderCache::AddReloadListener(std::function<void(Handle)> listener)
{
    m_Listeners.push_back(std::move(listener));
}

void ShaderCache::Clear()
{
    m_Shaders.clear();
    m_Listeners.clear();
    m_FailureListeners.clear();
}
} // namespace UHE
