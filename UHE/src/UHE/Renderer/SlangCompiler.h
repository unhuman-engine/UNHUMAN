#pragma once
#include "UHE/Core/Core.h"
#include "UHE/RHI/RHITypes.h"
#include <string>
#include <unordered_map>
#include <vector>


namespace UHE {
	class UHE_API SlangCompiler {
	public:
		static std::unordered_map<RHI::ShaderStage, std::string> CompileToGLSL(const std::string& filepath);
        /// Compiles every entry point, with optional preprocessor defines.
        ///
        /// A define is how one source serves two pipelines that differ in a
        /// shader output, rather than keeping two near-identical files in step.
        static std::unordered_map<RHI::ShaderStage, std::vector<uint8_t>> CompileToSPIRV(
            const std::string& filepath,
            const std::vector<std::pair<std::string, std::string>>& defines = {});
	};
}
