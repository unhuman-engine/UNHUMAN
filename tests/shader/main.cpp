// Shader harness (issue #29 follow-up, extended for the offline compiler).
//
// Basic3D.slang grew a per-material buffer whose struct must stay
// field-for-field identical to MaterialGPU.h; a drifted field or a bad
// std430 offset does not fail the C++ build, it fails at ENGINE STARTUP when the
// runtime Slang compiler errors - or worse, silently misbinds. Compiling the
// real .slang files to SPIR-V here moves that failure into the test run where it
// belongs.
//
// It now covers the rest of the shader path too, because all of it is testable
// without a device:
//   - the reflection the renderers build pipelines from (vertex layouts, push
//     constant sizes, bindings) reads the same values the renderers used to
//     hard-code, so a shader edit that changes its interface fails here;
//   - `variants.txt` parsing, including the `no_entity_id` variant;
//   - a compiled artifact equals what a runtime compile produces, which is the
//     contract that lets the engine skip the compiler at startup;
//   - hot reload notices a source edit, recompiles it and rewrites the artifact;
//   - malformed input to the reflection reader is refused, not read out of
//     bounds.
//
// Links the real SlangCompiler (which uses the vendored Slang library the engine
// itself compiles shaders with at startup) - no GPU, no device.

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#include "UHE/RHI/RHITypes.h"
#include "UHE/Renderer/ShaderCache.h"
#include "UHE/Renderer/ShaderReflection.h"
#include "UHE/Renderer/SlangCompiler.h"

namespace
{

int g_failures = 0;
int g_checks = 0;

void check(bool condition, const std::string& what)
{
    ++g_checks;
    if (!condition)
    {
        ++g_failures;
        std::printf("  FAIL  %s\n", what.c_str());
    }
}

void note(const std::string& what)
{
    std::printf("  note  %s\n", what.c_str());
}

void section(const std::string& name)
{
    std::printf("[ %s ]\n", name.c_str());
}

/// The vertex layout Renderer3D hand-wrote before reflection replaced it, with
/// the location each element must come from. Location 5 (a_Tangent) is the one
/// the old comment warned about.
struct ExpectedAttribute
{
    UHE::RHI::ShaderDataType Type;
    const char* Name;
};

const ExpectedAttribute kBasic3DVertexLayout[] = {
    {UHE::RHI::ShaderDataType::Float3, "a_Position"}, {UHE::RHI::ShaderDataType::Float3, "a_Normal"},
    {UHE::RHI::ShaderDataType::Float2, "a_TexCoord"}, {UHE::RHI::ShaderDataType::Float4, "a_Tangent"},
    {UHE::RHI::ShaderDataType::Int4, "a_Joints"},     {UHE::RHI::ShaderDataType::Float4, "a_Weights"},
    {UHE::RHI::ShaderDataType::Float4, "a_Color"}};

std::string ReadFile(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

} // namespace

int main(int argc, char** argv)
{
    // The CMake harness passes the shader source directory.
    const std::string shaderDir = argc > 1 ? argv[1] : ".";
    const std::filesystem::path shaderPath(shaderDir);

    struct Case
    {
        const char* file;
        bool requireAllStages;
    };
    const std::vector<Case> cases = {
        {"Basic3D.slang", true}, // the model shader issue #29 is about
        {"Grid.slang", true},
        {"Texture.slang", true},
        {"Text.slang", true},
    };

    // ─── Compilation ────────────────────────────────────────────────────────
    for (const auto& testCase : cases)
    {
        section(testCase.file);
        const std::string path = shaderDir + "/" + testCase.file;
        auto compiled = UHE::SlangCompiler::CompileToSPIRV(path);

        check(!compiled.empty(), path + " compiles to SPIR-V");
        if (testCase.requireAllStages)
        {
            check(compiled.count(UHE::RHI::ShaderStage::Vertex) > 0, path + " has a vertex stage");
            check(compiled.count(UHE::RHI::ShaderStage::Fragment) > 0, path + " has a fragment stage");
        }
    }

    // ─── Reflection of the model shader ─────────────────────────────────────
    section("Basic3D reflection");
    {
        const auto compiled = UHE::SlangCompiler::CompileToSPIRV(shaderDir + "/Basic3D.slang");
        const auto vertexIt = compiled.find(UHE::RHI::ShaderStage::Vertex);
        const auto fragmentIt = compiled.find(UHE::RHI::ShaderStage::Fragment);
        check(vertexIt != compiled.end(), "Basic3D has a vertex stage to reflect");
        check(fragmentIt != compiled.end(), "Basic3D has a fragment stage to reflect");

        if (vertexIt != compiled.end())
        {
            const UHE::ShaderReflection reflection = UHE::ReflectSPIRV(vertexIt->second);
            check(reflection.Valid, "the vertex module reflects");
            check(reflection.Stage == UHE::RHI::ShaderStage::Vertex, "the vertex module reports the vertex stage");
            check(reflection.EntryPoint == "main", "Slang emits the entry point as 'main'");

            const std::size_t expected = std::size(kBasic3DVertexLayout);
            check(reflection.VertexInputs.size() == expected,
                  "the vertex layout has " + std::to_string(expected) + " attributes");
            for (std::size_t i = 0; i < expected && i < reflection.VertexInputs.size(); ++i)
            {
                const UHE::ShaderVertexInput& input = reflection.VertexInputs[i];
                const ExpectedAttribute& wanted = kBasic3DVertexLayout[i];
                check(input.Location == i,
                      std::string("attribute ") + std::to_string(i) + " is at location " + std::to_string(i));
                check(input.Type == wanted.Type, std::string("attribute ") + std::to_string(i) + " (" + wanted.Name +
                                                     ") has the format the layout declares");
            }
            check(reflection.PushConstantSize == 192, "the push constant block is 192 bytes, matching the host struct");
        }

        if (fragmentIt != compiled.end())
        {
            const UHE::ShaderReflection reflection = UHE::ReflectSPIRV(fragmentIt->second);
            check(reflection.Valid, "the fragment module reflects");
            check(reflection.Stage == UHE::RHI::ShaderStage::Fragment,
                  "the fragment module reports the fragment stage");
            check(reflection.PushConstantSize == 192, "the fragment stage agrees on the push constant block");
        }
    }

    // ─── Reflection of the grid shader: generated vertices, no inputs ───────
    section("Grid reflection");
    {
        const auto compiled = UHE::SlangCompiler::CompileToSPIRV(shaderDir + "/Grid.slang");
        const auto vertexIt = compiled.find(UHE::RHI::ShaderStage::Vertex);
        check(vertexIt != compiled.end(), "Grid has a vertex stage to reflect");
        if (vertexIt != compiled.end())
        {
            const UHE::ShaderReflection reflection = UHE::ReflectSPIRV(vertexIt->second);
            check(reflection.Valid, "the grid vertex module reflects");
            check(reflection.VertexInputs.empty(), "the grid takes no vertex buffer (gl_VertexIndex)");
            check(reflection.PushConstantSize == sizeof(float) * 16 * 2,
                  "the grid's push constant block is two matrices");
        }
    }

    // ─── Variants ───────────────────────────────────────────────────────────
    section("variants.txt");
    {
        const std::vector<UHE::ShaderVariant> variants = UHE::LoadShaderVariants(shaderPath);
        check(variants.size() == 2, "the shader directory declares two variants");
        if (variants.size() == 2)
        {
            check(variants[0].Name == "default", "the first variant is 'default'");
            check(variants[0].Defines.empty(), "'default' defines nothing");
            check(variants[1].Name == "no_entity_id", "the second variant is 'no_entity_id'");
            check(variants[1].Defines.size() == 1 && variants[1].Defines[0].first == "UHE_NO_ENTITY_ID" &&
                      variants[1].Defines[0].second == "1",
                  "'no_entity_id' defines UHE_NO_ENTITY_ID=1");
        }

        // The variant without the entity-id output must be a different module:
        // that difference is the whole reason the variant exists.
        const auto withId = UHE::SlangCompiler::CompileToSPIRV(shaderDir + "/Basic3D.slang");
        const auto withoutId =
            UHE::SlangCompiler::CompileToSPIRV(shaderDir + "/Basic3D.slang", {{"UHE_NO_ENTITY_ID", "1"}});
        const auto withIt = withId.find(UHE::RHI::ShaderStage::Fragment);
        const auto withoutIt = withoutId.find(UHE::RHI::ShaderStage::Fragment);
        check(withIt != withId.end() && withoutIt != withoutId.end(), "both fragment variants compile");
        if (withIt != withId.end() && withoutIt != withoutId.end())
        {
            check(withIt->second != withoutIt->second, "UHE_NO_ENTITY_ID produces a different fragment module");
            check(UHE::ReflectSPIRV(withoutIt->second).Valid, "the no_entity_id module reflects");
        }
    }

    // ─── The artifact is what the runtime would have compiled ───────────────
    section("compiled artifacts");
    {
        struct ArtifactCase
        {
            const char* source;
            const char* variant;
            UHE::RHI::ShaderStage stage;
        };
        const std::vector<ArtifactCase> artifactCases = {
            {"Basic3D.slang", "default", UHE::RHI::ShaderStage::Vertex},
            {"Basic3D.slang", "default", UHE::RHI::ShaderStage::Fragment},
            {"Basic3D.slang", "no_entity_id", UHE::RHI::ShaderStage::Fragment},
            {"Grid.slang", "default", UHE::RHI::ShaderStage::Vertex},
            {"Texture.slang", "default", UHE::RHI::ShaderStage::Fragment},
            {"Text.slang", "default", UHE::RHI::ShaderStage::Vertex},
        };

        int missing = 0;
        for (const ArtifactCase& artifactCase : artifactCases)
        {
            const std::filesystem::path source = shaderPath / artifactCase.source;
            const std::filesystem::path artifact =
                UHE::ShaderArtifactPath(source, artifactCase.variant, artifactCase.stage);

            std::vector<u8> bytes;
            if (!UHE::ReadShaderArtifact(artifact, bytes))
            {
                ++missing;
                continue;
            }

            const std::vector<std::pair<std::string, std::string>> defines =
                artifactCase.variant == std::string("no_entity_id")
                    ? std::vector<std::pair<std::string, std::string>>{{"UHE_NO_ENTITY_ID", "1"}}
                    : std::vector<std::pair<std::string, std::string>>{};
            const auto fresh = UHE::SlangCompiler::CompileToSPIRV(source.string(), defines);
            const auto freshIt = fresh.find(artifactCase.stage);
            check(freshIt != fresh.end(), std::string(artifactCase.source) + " compiles for comparison");
            if (freshIt != fresh.end())
            {
                check(bytes == freshIt->second, artifact.filename().string() + " is byte-identical to a fresh compile");
            }
        }

        if (missing > 0)
        {
            // Only reachable when the harness runs before `uhe_shaders` has: the
            // comparison above is skipped rather than failed, but the engine's
            // fast path is what depends on these files.
            note(std::to_string(missing) + " artifact(s) missing - run the `uhe_shaders` target to cover this");
        }
    }

    // ─── Hot reload ─────────────────────────────────────────────────────────
    section("hot reload");
    {
        // A throwaway copy of a real shader: the test must not edit the assets.
        const std::filesystem::path tempDir = std::filesystem::temp_directory_path() / "uhe-shader-test";
        std::error_code ec;
        std::filesystem::remove_all(tempDir, ec);
        std::filesystem::create_directories(tempDir, ec);
        std::filesystem::copy_file(shaderPath / "Basic3D.slang", tempDir / "Basic3D.slang", ec);
        std::filesystem::copy_file(shaderPath / "variants.txt", tempDir / "variants.txt", ec);

        UHE::ShaderCache& library = UHE::ShaderCache::Get();
        library.Clear();

        int reloadedHandles = 0;
        library.AddReloadListener([&reloadedHandles](UHE::ShaderCache::Handle) { ++reloadedHandles; });

        const UHE::ShaderCache::Handle handle = library.Load((tempDir / "Basic3D.slang").string());
        check(handle != 0, "a source with no artifact yet loads");
        const UHE::ShaderCache::Shader* shader = library.Get(handle);
        check(shader != nullptr, "the loaded shader is readable");
        if (shader != nullptr)
        {
            check(!shader->FromArtifact, "the first load had to compile");
            check(shader->Stages.size() == 2, "both stages were produced");
            check(shader->ReloadCount == 0, "nothing has reloaded yet");
        }

        std::vector<u8> written;
        check(
            UHE::ReadShaderArtifact(
                UHE::ShaderArtifactPath(tempDir / "Basic3D.slang", "default", UHE::RHI::ShaderStage::Vertex), written),
            "compiling wrote the vertex artifact");
        check(library.Load((tempDir / "Basic3D.slang").string()) == handle, "a second load is the same handle");

        // A second load with the artifact in place takes the fast path.
        library.Clear();
        library.AddReloadListener([&reloadedHandles](UHE::ShaderCache::Handle) { ++reloadedHandles; });
        const UHE::ShaderCache::Handle artifactHandle = library.Load((tempDir / "Basic3D.slang").string());
        const UHE::ShaderCache::Shader* fromArtifact = library.Get(artifactHandle);
        check(fromArtifact != nullptr && fromArtifact->FromArtifact,
              "a load with the artifact present reads it instead of compiling");

        check(library.Poll().empty(), "polling an unchanged source reloads nothing");

        const std::filesystem::path artifact =
            UHE::ShaderArtifactPath(tempDir / "Basic3D.slang", "default", UHE::RHI::ShaderStage::Vertex);
        const auto artifactTimeBefore = std::filesystem::last_write_time(artifact, ec);

        // Touch the source: the edit is what hot reload exists for.
        std::filesystem::last_write_time(tempDir / "Basic3D.slang",
                                         std::filesystem::file_time_type::clock::now() + std::chrono::seconds(10), ec);

        const std::vector<UHE::ShaderCache::Handle> polled = library.Poll();
        check(polled.size() == 1, "polling a touched source reports one reload");
        check(reloadedHandles == 1, "the reload listener was notified once");
        const UHE::ShaderCache::Shader* reloaded = library.Get(artifactHandle);
        check(reloaded != nullptr && reloaded->ReloadCount == 1, "the shader records the reload");
        check(reloaded != nullptr && reloaded->Stages.size() == 2, "the reload kept both stages");

        // And it wrote the artifact back, so the next launch is on the fast path.
        check(std::filesystem::last_write_time(artifact, ec) > artifactTimeBefore, "the reload rewrote the artifact");

        // A broken edit: reported once, with Slang's message, and the last good
        // stages stay in place.
        int failures = 0;
        std::string failureText;
        library.AddFailureListener(
            [&](UHE::ShaderCache::Handle, const std::string& error)
            {
                ++failures;
                failureText = error;
            });
        {
            std::ofstream broken(tempDir / "Basic3D.slang", std::ios::app);
            broken << "\nthis is not slang @@@\n";
        }
        std::filesystem::last_write_time(tempDir / "Basic3D.slang",
                                         std::filesystem::file_time_type::clock::now() + std::chrono::seconds(20), ec);
        check(library.Poll().empty(), "a broken edit reloads nothing");
        check(failures == 1, "the failure listener is told once");
        check(!failureText.empty(), "with the compiler's message");
        const UHE::ShaderCache::Shader* kept = library.Get(artifactHandle);
        check(kept != nullptr && !kept->Error.empty(), "the shader records the error");
        check(kept != nullptr && kept->Stages.size() == 2, "and keeps its last good stages");
        check(library.Poll().empty() && failures == 1, "an unchanged broken source is not recompiled every poll");
        check(library.ReloadAll() == 1, "ReloadAll counts the failure");

        library.Clear();
        std::filesystem::remove_all(tempDir, ec);
    }

    // ─── Malformed input ────────────────────────────────────────────────────
    section("malformed SPIR-V");
    {
        const std::vector<u8> empty;
        check(!UHE::ReflectSPIRV(empty).Valid, "an empty blob is refused");

        std::vector<u8> garbage(256, 0xAB);
        const UHE::ShaderReflection garbageReflection = UHE::ReflectSPIRV(garbage);
        check(!garbageReflection.Valid, "a blob with no SPIR-V magic is refused");
        check(!garbageReflection.Error.empty(), "the refusal says why");

        const auto compiled = UHE::SlangCompiler::CompileToSPIRV(shaderDir + "/Basic3D.slang");
        const auto vertexIt = compiled.find(UHE::RHI::ShaderStage::Vertex);
        if (vertexIt != compiled.end())
        {
            std::vector<u8> truncated = vertexIt->second;
            truncated.resize(truncated.size() / 3);
            check(!UHE::ReflectSPIRV(truncated).Valid, "a truncated module is refused");

            // A module whose last instruction claims more words than it has.
            std::vector<u8> lying = vertexIt->second;
            const std::size_t lastWord = lying.size() / sizeof(u32) - 1;
            lying[lastWord * 4 + 3] = 0x7F; // instruction word: huge word count
            check(!UHE::ReflectSPIRV(lying).Valid, "an instruction that overruns the blob is refused");
        }
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
