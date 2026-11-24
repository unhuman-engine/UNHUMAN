#include "uhepch.h"
#include "SlangCompiler.h"
#include <filesystem>
#include <fstream>
#include <slang-com-helper.h>
#include <slang-com-ptr.h>
#include <slang.h>

namespace UHE
{

static Slang::ComPtr<slang::IGlobalSession> s_GlobalSession;
static Slang::ComPtr<slang::ISession> s_Session;

static void InitSlang()
{
    if (s_GlobalSession)
        return;

    slang::createGlobalSession(s_GlobalSession.writeRef());
    slang::SessionDesc sessionDesc = {};

    slang::TargetDesc targets[2] = {};

    // ---------------- SPIR-V ----------------
    targets[0].format = SLANG_SPIRV;
    targets[0].profile = s_GlobalSession->findProfile("spirv_1_5");

    // ---------------- GLSL ----------------
    targets[1].format = SLANG_GLSL;
    targets[1].profile = s_GlobalSession->findProfile("glsl_450");

    sessionDesc.targets = targets;
    sessionDesc.targetCount = 2;

    s_GlobalSession->createSession(sessionDesc, s_Session.writeRef());
}

static void StringReplaceAll(std::string& source, const std::string& from, const std::string& to)
{
    if (from.empty())
        return;
    size_t start_pos = 0;
    while ((start_pos = source.find(from, start_pos)) != std::string::npos)
    {
        source.replace(start_pos, from.length(), to);
        start_pos += to.length();
    }
}

std::unordered_map<RHI::ShaderStage, std::string> SlangCompiler::CompileToGLSL(const std::string& filepath)
{
    InitSlang();

    std::filesystem::path p(filepath);
    std::string baseName = (p.parent_path() / p.stem()).string();

    std::unordered_map<RHI::ShaderStage, std::string> result;

    std::string sourceStr;
    std::ifstream in(filepath, std::ios::in | std::ios::binary);
    if (in)
    {
        in.seekg(0, std::ios::end);
        sourceStr.resize(in.tellg());
        in.seekg(0, std::ios::beg);
        in.read(&sourceStr[0], sourceStr.size());
        in.close();
    }
    else
    {
        UHE_CORE_ERROR("Could not open file: {0}", filepath);
        return result;
    }

    Slang::ComPtr<slang::IBlob> diagnosticBlob;
    slang::IModule* module = s_Session->loadModuleFromSourceString("ShaderModule", filepath.c_str(), sourceStr.c_str(),
                                                                   diagnosticBlob.writeRef());

    if (diagnosticBlob)
    {
        UHE_CORE_WARN("Slang diagnostics for {0}:\n{1}", filepath, (const char*)diagnosticBlob->getBufferPointer());
    }

    if (!module)
    {
        UHE_CORE_ERROR("Failed to load Slang module from: {0}", filepath);
        return result;
    }

    struct EntryPointInfo
    {
        const char* name;
        RHI::ShaderStage type;
        const char* suffix;
    };

    std::vector<EntryPointInfo> entryPoints = {{"vertexMain", RHI::ShaderStage::Vertex, ".vert"},
                                               {"fragmentMain", RHI::ShaderStage::Fragment, ".frag"},
                                               {"computeMain", RHI::ShaderStage::Compute, ".comp"}};

    for (const auto& ep : entryPoints)
    {
        Slang::ComPtr<slang::IEntryPoint> entryPoint;
        module->findEntryPointByName(ep.name, entryPoint.writeRef());

        if (!entryPoint)
            continue;

        slang::IComponentType* components[] = {module, entryPoint};
        Slang::ComPtr<slang::IComponentType> program;

        s_Session->createCompositeComponentType(components, 2, program.writeRef(), diagnosticBlob.writeRef());

        Slang::ComPtr<slang::IComponentType> linkedProgram;
        program->link(linkedProgram.writeRef(), diagnosticBlob.writeRef());

        if (diagnosticBlob)
        {
            const char* msg = (const char*)diagnosticBlob->getBufferPointer();
            if (msg && msg[0])
                UHE_CORE_WARN("Slang link diagnostics:\n{0}", msg);
        }

        // 1. SPIR-V Target (Index 0)
        Slang::ComPtr<slang::IBlob> spirvBlob;
        linkedProgram->getTargetCode(0, spirvBlob.writeRef(), diagnosticBlob.writeRef());

        if (spirvBlob)
        {
            std::string outPath = baseName + ep.suffix + ".spv";
            std::ofstream out(outPath, std::ios::binary);
            out.write((char*)spirvBlob->getBufferPointer(), spirvBlob->getBufferSize());
        }

        // 2. GLSL Target (Index 1)
        Slang::ComPtr<slang::IBlob> glslBlob;
        linkedProgram->getTargetCode(1, glslBlob.writeRef(), diagnosticBlob.writeRef());

        if (glslBlob)
        {
            std::string outPath = baseName + ep.suffix + ".glsl";
            std::ofstream out(outPath);
            out.write((char*)glslBlob->getBufferPointer(), glslBlob->getBufferSize());

            std::string code((const char*)glslBlob->getBufferPointer(), glslBlob->getBufferSize());
            result[ep.type] = code;
        }
        else
        {
            UHE_CORE_ERROR("Failed to generate GLSL target code for entry point: {0}", ep.name);
            if (diagnosticBlob)
            {
                UHE_CORE_ERROR("Error: {0}", (const char*)diagnosticBlob->getBufferPointer());
            }
        }
    }

    return result;
}

std::unordered_map<RHI::ShaderStage, std::vector<uint8_t>>
SlangCompiler::CompileToSPIRV(const std::string& filepath,
                              const std::vector<std::pair<std::string, std::string>>& defines, std::string* diagnostics)
{
    std::unordered_map<RHI::ShaderStage, std::vector<uint8_t>> result;
    if (diagnostics)
        diagnostics->clear();
    // Collects what Slang said, for the caller; logging stays as it was.
    auto note = [diagnostics](const char* text)
    {
        if (diagnostics && text && text[0])
        {
            if (!diagnostics->empty())
                *diagnostics += '\n';
            *diagnostics += text;
        }
    };

    Slang::ComPtr<slang::IGlobalSession> slangGlobalSession;
    slang::createGlobalSession(slangGlobalSession.writeRef());

    slang::SessionDesc sessionDesc = {};
    slang::TargetDesc targetDesc = {};
    targetDesc.format = SLANG_SPIRV;
    targetDesc.profile = slangGlobalSession->findProfile("spirv_1_5");

    // The macro names and values must outlive the session: `SessionDesc` holds
    // pointers into them.
    std::vector<slang::PreprocessorMacroDesc> macros;
    macros.reserve(defines.size());
    for (const auto& [name, value] : defines)
    {
        macros.push_back(slang::PreprocessorMacroDesc{name.c_str(), value.c_str()});
    }

    sessionDesc.targets = &targetDesc;
    sessionDesc.targetCount = 1;
    sessionDesc.preprocessorMacros = macros.empty() ? nullptr : macros.data();
    sessionDesc.preprocessorMacroCount = static_cast<SlangInt>(macros.size());
    sessionDesc.defaultMatrixLayoutMode = SLANG_MATRIX_LAYOUT_COLUMN_MAJOR;

    Slang::ComPtr<slang::ISession> session;
    slangGlobalSession->createSession(sessionDesc, session.writeRef());

    Slang::ComPtr<slang::IBlob> diagnosticBlob;
    slang::IModule* module = session->loadModule(filepath.c_str(), diagnosticBlob.writeRef());

    if (diagnosticBlob)
    {
        const char* msg = (const char*)diagnosticBlob->getBufferPointer();
        if (msg && msg[0])
            UHE_CORE_WARN("Slang compiler diagnostics:\n{0}", msg);
        if (!module)
            note(msg);
    }

    if (!module)
    {
        UHE_CORE_ERROR("Failed to load slang module: {0}", filepath);
        if (diagnostics && diagnostics->empty())
            *diagnostics = "failed to load '" + filepath + "'";
        return result;
    }

    struct EntryPointInfo
    {
        const char* name;
        RHI::ShaderStage stage;
    };

    EntryPointInfo entryPoints[] = {{"vertexMain", RHI::ShaderStage::Vertex},
                                    {"fragmentMain", RHI::ShaderStage::Fragment},
                                    {"computeMain", RHI::ShaderStage::Compute}};

    for (const auto& ep : entryPoints)
    {
        Slang::ComPtr<slang::IEntryPoint> entryPoint;
        module->findEntryPointByName(ep.name, entryPoint.writeRef());

        if (!entryPoint)
            continue;

        slang::IComponentType* components[] = {module, entryPoint};
        Slang::ComPtr<slang::IComponentType> program;

        session->createCompositeComponentType(components, 2, program.writeRef(), diagnosticBlob.writeRef());

        Slang::ComPtr<slang::IComponentType> linkedProgram;
        program->link(linkedProgram.writeRef(), diagnosticBlob.writeRef());

        if (diagnosticBlob)
        {
            const char* msg = (const char*)diagnosticBlob->getBufferPointer();
            if (msg && msg[0])
                UHE_CORE_WARN("Slang link diagnostics:\n{0}", msg);
        }

        Slang::ComPtr<slang::IBlob> spirvBlob;
        linkedProgram->getTargetCode(0, spirvBlob.writeRef(), diagnosticBlob.writeRef());

        if (spirvBlob)
        {
            const uint8_t* ptr = (const uint8_t*)spirvBlob->getBufferPointer();
            size_t size = spirvBlob->getBufferSize();
            result[ep.stage] = std::vector<uint8_t>(ptr, ptr + size);
        }
        else
        {
            UHE_CORE_ERROR("Failed to generate SPIR-V code for entry point: {0}", ep.name);
            note((std::string(ep.name) + ": no SPIR-V generated").c_str());
            if (diagnosticBlob)
            {
                UHE_CORE_ERROR("Error: {0}", (const char*)diagnosticBlob->getBufferPointer());
                note((const char*)diagnosticBlob->getBufferPointer());
            }
        }
    }

    return result;
}

} // namespace UHE
