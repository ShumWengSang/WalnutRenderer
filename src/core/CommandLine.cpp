#include "core/CommandLine.h"

#include "core/Log.h"
#include "core/StringUtils.h"

#include <Windows.h>
#include <shellapi.h>

#include <cstdlib>
#include <cwchar>
#include <format>
#include <functional>
#include <string_view>

namespace walnut
{
    namespace
    {
        struct Option
        {
            std::wstring_view name;  // "--name" (flag) or "--name=" (takes a value)
            std::string_view usage;  // what to write after the name in --help
            std::string_view help;
            std::function<bool(AppOptions&, std::wstring_view value)> apply; // false: bad value
        };

        bool ParseCamera(std::wstring_view value, CameraPose& pose)
        {
            float v[5]{};
            const std::wstring text(value);
            if (swscanf_s(text.c_str(), L"%f,%f,%f,%f,%f", &v[0], &v[1], &v[2], &v[3], &v[4]) != 5)
                return false;
            pose = CameraPose{ v[0], v[1], v[2], v[3], v[4] };
            return true;
        }

        const std::vector<Option>& GetOptions()
        {
            const auto toU64 = [](std::wstring_view v) { return std::wcstoull(std::wstring(v).c_str(), nullptr, 10); };
            const auto toDouble = [](std::wstring_view v) { return std::wcstod(std::wstring(v).c_str(), nullptr); };
            static const std::vector<Option> options = {
                { L"--debug-layer", "", "Force the D3D12 debug layer on (default in Debug builds).",
                  [](AppOptions& o, std::wstring_view) { o.debugLayer = true; return true; } },
                { L"--no-debug-layer", "", "Force the D3D12 debug layer off.",
                  [](AppOptions& o, std::wstring_view) { o.debugLayer = false; return true; } },
                { L"--gpu-validation", "", "GPU-based validation (also enables the debug layer). Slow.",
                  [](AppOptions& o, std::wstring_view) { o.gpuValidation = true; return true; } },
                { L"--pix", "", "Load PIX's GPU capturer at startup.",
                  [](AppOptions& o, std::wstring_view) { o.pixGpuCapturer = true; return true; } },
                { L"--pix-capture-frame=", "N", "Take a GPU capture of frame N into bin/captures/ (implies --pix).",
                  [=](AppOptions& o, std::wstring_view v) { o.pixGpuCapturer = true; o.pixCaptureFrame = toU64(v); return true; } },
                { L"--exit-after-frames=", "N", "Quit after N frames.",
                  [=](AppOptions& o, std::wstring_view v) { o.exitAfterFrames = toU64(v); return true; } },
                { L"--exit-after-seconds=", "S", "Quit after S seconds (used by tools/pix-capture.ps1).",
                  [=](AppOptions& o, std::wstring_view v) { o.exitAfterSeconds = toDouble(v); return true; } },
                { L"--scene=", "PATH", "glTF scene; relative paths start at assets/ (default Sponza/Sponza.gltf).",
                  [](AppOptions& o, std::wstring_view v) { o.scene = v; return true; } },
                { L"--time=", "S", "Animations use time S instead of the clock (reproducible frames).",
                  [=](AppOptions& o, std::wstring_view v) { o.fixedTime = toDouble(v); return true; } },
                { L"--camera=", "X,Y,Z,YAW,PITCH", "Start camera: position in meters, yaw and pitch in degrees.",
                  [](AppOptions& o, std::wstring_view v) { CameraPose pose; if (!ParseCamera(v, pose)) return false; o.camera = pose; return true; } },
                { L"--set=", "NAME=VALUE", "Change a setting at startup (repeatable; see --list-settings).",
                  [](AppOptions& o, std::wstring_view v) {
                      const size_t equals = v.find(L'=');
                      if (equals == std::wstring_view::npos)
                          return false;
                      o.settings.emplace_back(ToUtf8(v.substr(0, equals)), ToUtf8(v.substr(equals + 1)));
                      return true;
                  } },
                { L"--list-settings", "", "Log every --set name with its current value, then quit.",
                  [](AppOptions& o, std::wstring_view) { o.listSettings = true; return true; } },
                { L"--screenshot=", "FILE.png", "Save frame N (see --screenshot-frame) without the UI, then quit.",
                  [](AppOptions& o, std::wstring_view v) { o.screenshot = v; return true; } },
                { L"--screenshot-frame=", "N", "Which frame --screenshot saves (default 5).",
                  [=](AppOptions& o, std::wstring_view v) { o.screenshotFrame = toU64(v); return true; } },
                { L"--help", "", "Log these options, then quit.",
                  [](AppOptions& o, std::wstring_view) { o.help = true; return true; } },
            };
            return options;
        }
    }

    std::string GetCommandLineHelp()
    {
        std::string text = "Options:\n";
        for (const Option& option : GetOptions())
            text += std::format("  {}{:<24} {}\n", ToUtf8(option.name), option.usage, option.help);
        return text;
    }

    AppOptions ParseCommandLine()
    {
        AppOptions options;
#ifdef _DEBUG
        options.debugLayer = true;
#endif
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (!argv)
            return options;

        for (int i = 1; i < argc; ++i)
        {
            const std::wstring_view arg = argv[i];
            bool known = false;
            for (const Option& option : GetOptions())
            {
                const bool takesValue = option.name.ends_with(L'=');
                if (takesValue ? !arg.starts_with(option.name) : arg != option.name)
                    continue;
                known = true;
                if (!option.apply(options, takesValue ? arg.substr(option.name.size()) : std::wstring_view{}))
                    log::Warn("Bad value in command-line argument: {}", ToUtf8(arg));
                break;
            }
            if (!known)
                log::Warn("Unknown command-line argument: {} (see --help)", ToUtf8(arg));
        }
        LocalFree(argv);
        return options;
    }
}
