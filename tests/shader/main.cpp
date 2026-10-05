// Shader compilation harness (issue #29 follow-up).
//
// Basic3D.slang grew a per-material buffer whose struct must stay
// field-for-field identical to MaterialGPU.h; a drifted field or a bad
// std430 offset does not fail the C++ build, it fails at ENGINE STARTUP when
// the runtime Slang compiler errors - or worse, silently misbinds. Compiling
// the real .slang files to SPIR-V here moves that failure into the test run
// where it belongs.
//
// Links the real SlangCompiler (which uses the vendored Slang library the
// engine itself compiles shaders with at startup) - no GPU, no device.

#include <cstdio>
#include <string>
#include <vector>

#include "UHE/Renderer/SlangCompiler.h"
#include "UHE/RHI/RHITypes.h"

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

void section(const std::string& name)
{
    std::printf("[ %s ]\n", name.c_str());
}

} // namespace

int main(int argc, char** argv)
{
    // The CMake harness passes the shader source directory.
    std::string shaderDir = argc > 1 ? argv[1] : ".";

    struct Case
    {
        const char* file;
        bool requireAllStages;
    };
    const std::vector<Case> cases = {
        {"Basic3D.slang", true}, // the model shader this issue is about
        {"Grid.slang", true},
        {"Texture.slang", true},
        {"Text.slang", true},
    };

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

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
