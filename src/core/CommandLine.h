#pragma once

#include "scene/Assets.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace walnut
{
    // Where the camera starts (--camera): position in meters, yaw and pitch in degrees.
    struct CameraPose
    {
        float x = 0.0f, y = 0.0f, z = 0.0f;
        float yawDegrees = 0.0f, pitchDegrees = 0.0f;
    };

    struct AppOptions
    {
        bool debugLayer = false;
        bool gpuValidation = false;
        bool pixGpuCapturer = false;   // load WinPixGpuCapturer.dll at startup
        uint64_t pixCaptureFrame = 0;  // take a GPU capture at this frame (implies pixGpuCapturer)
        uint64_t exitAfterFrames = 0;  // 0 = run until closed (useful for automated smoke tests)
        double exitAfterSeconds = 0.0; // 0 = run until closed
        std::filesystem::path scene = kDefaultScene; // relative to the asset directory, or absolute

        // Reproducible runs (for screenshots and before/after comparisons):
        std::optional<double> fixedTime;   // --time=S: animations use this time instead of the clock
        std::optional<CameraPose> camera;  // --camera=x,y,z,yaw,pitch
        std::vector<std::pair<std::string, std::string>> settings; // --set=name=value, applied in order
        std::filesystem::path screenshot;  // --screenshot=file.png: save frame screenshotFrame (without UI), then exit
        uint64_t screenshotFrame = 5;      // --screenshot-frame=N
        bool listSettings = false;         // --list-settings: log every --set name and its current value, then exit
        bool help = false;                 // --help: log the options, then exit
    };

    // Parses the process's command line. Unknown arguments are logged and ignored.
    AppOptions ParseCommandLine();
    // The usage text (--help).
    std::string GetCommandLineHelp();
}
