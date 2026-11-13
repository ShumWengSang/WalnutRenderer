#pragma once

#include "core/CommandLine.h"
#include "core/Input.h"
#include "core/SettingsRegistry.h"
#include "core/Timer.h"
#include "renderer/RenderSettings.h"
#include "renderer/SceneRenderer.h"
#include "renderer/TonemapPass.h"
#include "scene/Assets.h"
#include "scene/Camera.h"
#include "scene/FlyCameraController.h"
#include "scene/GltfLoader.h"
#include "scene/Lighting.h"
#include "scene/PointLights.h"
#include "ui/SettingsPanel.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace walnut
{
    class Window;
    class Renderer;
    class ShaderCompiler;
    class ImGuiLayer;

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
        void ReloadShaders();
        void RequestGpuCapture();
        // The names --set and --list-settings know (core/SettingsRegistry.h).
        void RegisterSettings();
        // The time animations run on: the clock, or --time.
        double GetAnimationTime() const;

        // Declared first, so it's destroyed last: with the debug layer on, lists any D3D12 object still alive (a
        // leak) after everything else is gone.
        struct LiveObjectReport
        {
            bool enabled = false;
            ~LiveObjectReport();
        } m_liveObjectReport;

        AppOptions m_options;
        std::filesystem::path m_exeDir;
        std::filesystem::path m_shaderDir; // where .hlsl files are compiled from (source tree, or next to the exe)
        SceneFileInfo m_sceneFile;
        Scene m_scene; // CPU copy of the scene; SceneRenderer uploads it

        std::unique_ptr<Window> m_window;
        Input m_input;
        std::unique_ptr<Renderer> m_renderer;
        std::unique_ptr<ShaderCompiler> m_shaderCompiler;
        std::unique_ptr<SceneRenderer> m_sceneRenderer; // the scene's GPU data and every pass
        std::unique_ptr<ImGuiLayer> m_imgui;
        Timer m_timer;

        // Settings (renderer/RenderSettings.h) and what the scene contains
        RenderSettings m_renderSettings;
        DebugSettings m_debugSettings;
        TonemapSettings m_tonemapSettings;
        SceneLighting m_lighting;
        PointLights m_pointLights;
        std::vector<PointLightGpu> m_pointLightData; // this frame's lights, rebuilt every frame

        Camera m_camera;
        Camera m_homeCamera; // where "Reset camera" goes: framing the loaded scene
        FlyCameraController m_cameraController;
        bool m_showUI = true; // F1
        std::filesystem::path m_lastCapturePath;
        SettingsRegistry m_settings;
        // Last: it refers to the members above.
        std::unique_ptr<SettingsPanel> m_settingsPanel;
    };
}
