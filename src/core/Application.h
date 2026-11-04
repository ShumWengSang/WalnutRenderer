#pragma once

#include "core/Input.h"
#include "core/Timer.h"
#include "renderer/TrianglePass.h"

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
        std::unique_ptr<ImGuiLayer> m_imgui;
        Timer m_timer;

        // Demo state
        float m_clearColor[4] = { 0.08f, 0.09f, 0.12f, 1.0f };
        TriangleConstants m_triangle;
        bool m_autoRotate = true;
        float m_rotationSpeed = 1.0f; // radians per second
        float m_moveSpeed = 1.0f;     // NDC units per second
        bool m_showDemoWindow = false;
        float m_smoothedFrameMs = 0.0f;
        std::filesystem::path m_lastCapturePath;
    };
}
