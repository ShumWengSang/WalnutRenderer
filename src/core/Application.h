#pragma once

#include "core/Input.h"
#include "core/Timer.h"
#include "renderer/TonemapPass.h"
#include "renderer/TrianglePass.h"
#include "scene/Camera.h"
#include "scene/FlyCameraController.h"

#include <cstdint>
#include <filesystem>
#include <memory>

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
    };

    // Parses --debug-layer, --no-debug-layer, --gpu-validation, --pix, --pix-capture-frame=N,
    // --exit-after-frames=N and --exit-after-seconds=S.
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

        std::unique_ptr<Window> m_window;
        Input m_input;
        std::unique_ptr<Renderer> m_renderer;
        std::unique_ptr<ShaderCompiler> m_shaderCompiler;
        std::unique_ptr<TrianglePass> m_trianglePass;
        std::unique_ptr<TonemapPass> m_tonemapPass;
        std::unique_ptr<ImGuiLayer> m_imgui;
        Timer m_timer;

        // Demo state
        float m_clearColor[4] = {}; // starts as Renderer::kDefaultClearColor (the scene target's fast-clear color)
        TonemapSettings m_tonemapSettings;
        // Multiplies both triangles' colors, to push them into HDR (> 1.0) and see what the tonemap curve does.
        float m_triangleBrightness = 1.0f;
        Camera m_camera;
        FlyCameraController m_cameraController;
        TriangleParams m_triangle;
        // A bigger, darker (gray-tinted) triangle half a unit behind the main one.
        TriangleParams m_backTriangle{ .tint = { 0.55f, 0.55f, 0.6f, 1.0f }, .offset = { 0.25f, 0.1f, 0.5f }, .angle = 0.4f, .scale = 1.8f };
        bool m_depthTest = true;
        TextureViewSettings m_textureView;
        bool m_autoRotate = true;
        float m_rotationSpeed = 1.0f; // radians per second
        bool m_showDemoWindow = false;
        float m_smoothedFrameMs = 0.0f;
        std::filesystem::path m_lastCapturePath;
    };
}
