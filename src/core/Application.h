#pragma once

#include "core/Input.h"
#include "core/Timer.h"
#include "renderer/ScenePass.h"
#include "renderer/TonemapPass.h"
#include "scene/Assets.h"
#include "scene/Camera.h"
#include "scene/FlyCameraController.h"
#include "scene/GltfLoader.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace walnut
{
    class Window;
    class Renderer;
    class ShaderCompiler;
    class ImGuiLayer;

    struct AppOptions
    {
        bool debugLayer = false;
        bool gpuValidation = false;
        bool pixGpuCapturer = false;   // load WinPixGpuCapturer.dll at startup
        uint64_t pixCaptureFrame = 0;  // take a GPU capture at this frame (implies pixGpuCapturer)
        uint64_t exitAfterFrames = 0; // 0 = run until closed (useful for automated smoke tests)
        double exitAfterSeconds = 0.0; // 0 = run until closed
        std::filesystem::path scene = kDefaultScene; // relative to the asset directory, or absolute
    };

    // Parses --debug-layer, --no-debug-layer, --gpu-validation, --pix, --pix-capture-frame=N,
    // --exit-after-frames=N, --exit-after-seconds=S and --scene=PATH.
    AppOptions ParseCommandLine();

    class Application
    {
    public:
        explicit Application(const AppOptions& options);
        ~Application();

        Application(const Application&) = delete;
        Application& operator=(const Application&) = delete;

        int Run();

    private:
        void Update(float deltaSeconds);
        void DrawUI(float deltaSeconds);
        void DrawPixUI();
        void ReloadShaders();
        void RequestGpuCapture();

        AppOptions m_options;
        std::filesystem::path m_exeDir;
        std::filesystem::path m_shaderDir; // where .hlsl files are compiled from (source tree, or next to the exe)
        std::filesystem::path m_scenePath; // m_options.scene resolved against the asset directory
        std::optional<GltfSummary> m_sceneSummary; // set when the scene parsed successfully
        std::string m_sceneError;                  // why it didn't, otherwise
        double m_sceneLoadMs = 0.0;
        Scene m_scene; // CPU copy of the scene; ScenePass uploads it
        double m_sceneBuildMs = 0.0;

        std::unique_ptr<Window> m_window;
        Input m_input;
        std::unique_ptr<Renderer> m_renderer;
        std::unique_ptr<ShaderCompiler> m_shaderCompiler;
        std::unique_ptr<ScenePass> m_scenePass; // null when no scene loaded
        std::unique_ptr<TonemapPass> m_tonemapPass;
        std::unique_ptr<ImGuiLayer> m_imgui;
        Timer m_timer;

        // Demo state
        float m_clearColor[4] = {}; // starts as Renderer::kDefaultClearColor (the scene target's fast-clear color)
        TonemapSettings m_tonemapSettings;
        SceneViewSettings m_sceneView;
        Camera m_camera;
        Camera m_homeCamera; // where "Reset camera" goes: framing the loaded scene
        FlyCameraController m_cameraController;
        bool m_showDemoWindow = false;
        float m_smoothedFrameMs = 0.0f;
        std::filesystem::path m_lastCapturePath;
    };
}
