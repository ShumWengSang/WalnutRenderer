#include "core/Application.h"
#include "core/Log.h"

#include <Windows.h>

#include <cstdlib>
#include <exception>
#include <filesystem>
#include <string>

// Opt in to the D3D12 Agility SDK: the D3D12 loader reads these exports and loads
// D3D12Core.dll from <exe dir>/D3D12/ instead of the OS copy.
extern "C"
{
    __declspec(dllexport) extern const UINT D3D12SDKVersion = WALNUT_AGILITY_SDK_VERSION;
    __declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12\\";
}

namespace
{
    std::filesystem::path GetLogPath()
    {
        wchar_t buffer[MAX_PATH]{};
        GetModuleFileNameW(nullptr, buffer, MAX_PATH);
        return std::filesystem::path(buffer).parent_path() / "WalnutRenderer.log";
    }
}

int WINAPI wWinMain(_In_ HINSTANCE, _In_opt_ HINSTANCE, _In_ PWSTR, _In_ int)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    walnut::log::Init(GetLogPath());
    walnut::log::Info("WalnutRenderer starting (Agility SDK {})", WALNUT_AGILITY_SDK_VERSION);

    const walnut::AppOptions options = walnut::ParseCommandLine();
    if (options.help)
    {
        walnut::log::Info("{}", walnut::GetCommandLineHelp());
        walnut::log::Shutdown();
        return EXIT_SUCCESS;
    }
    int exitCode = EXIT_SUCCESS;
    try
    {
        walnut::Application app(options);
        exitCode = app.Run();
    }
    catch (const std::exception& e)
    {
        walnut::log::Error("Fatal: {}", e.what());
        if (options.exitAfterFrames == 0 && options.exitAfterSeconds <= 0.0 && options.screenshot.empty())
            MessageBoxA(nullptr, e.what(), "WalnutRenderer - fatal error", MB_OK | MB_ICONERROR);
        exitCode = EXIT_FAILURE;
    }

    walnut::log::Info("Exiting with code {}", exitCode);
    walnut::log::Shutdown();
    return exitCode;
}
