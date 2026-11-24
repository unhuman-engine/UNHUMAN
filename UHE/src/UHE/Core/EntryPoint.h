#pragma once
#include "UHE/AssetsManager/VfsSystem.h"
#include "UHE/Core/CommandLine.h"

extern UHE::Application* UHE::CreateApplication();

int main(int argc, char** argv)
{
    UHE::FileSystem::Get().Initialize(argv[0]);
    UHE::CommandLine::Init(argc, argv);
    UHE::Log::Init();
    auto app = UHE::CreateApplication();
    
    app->Run();

    const int exitCode = app->GetExitCode();
    delete app;
    return exitCode;
}
