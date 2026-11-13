#pragma once

#include "core/CommandLine.h"
#include "core/Input.h"
#include "core/SettingsRegistry.h"
#include "core/Timer.h"
#include "renderer/AccelerationStructures.h"
#include "renderer/ClusterGrid.h"
#include "renderer/SceneGpuData.h"
#include "renderer/ScenePass.h"
#include "renderer/ShadowMap.h"
#include "renderer/DeferredPass.h"
#include "renderer/LightBuffer.h"
#include "renderer/LightMarkerPass.h"
#include "renderer/PointShadowMaps.h"
#include "renderer/RenderSettings.h"
#include "renderer/TonemapPass.h"
#include "scene/Assets.h"
#include "scene/Camera.h"
#include "scene/FlyCameraController.h"
#include "scene/GltfLoader.h"
#include "scene/Lighting.h"
#include "scene/PointLights.h"

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
        void DrawUI(float deltaSeconds);
        void DrawStatsOverlay() const;
        void DrawPixUI();
        void ReloadShaders();
        // Every pass whose shaders F5 recompiles.
        std::vector<ShaderReloadable*> GetShaderReloadables() const;
        void RequestGpuCapture();
        // The names --set and --list-settings know (core/SettingsRegistry.h).
        void RegisterSettings();
        // The time animations run on: the clock, or --time.
        double GetAnimationTime() const;

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
        std::unique_ptr<SceneGpuData> m_sceneData; // null when no scene loaded
        std::unique_ptr<ScenePass> m_scenePass;
        std::unique_ptr<ShadowMap> m_shadowMap;
        std::unique_ptr<AccelerationStructures> m_accelerationStructures; // null without inline ray tracing support
        static constexpr uint32_t kShadowMapSize = 2048;
        std::unique_ptr<TonemapPass> m_tonemapPass;
        std::unique_ptr<DeferredPass> m_deferredPass;
        std::unique_ptr<LightBuffer> m_lightBuffer;
        std::unique_ptr<LightMarkerPass> m_lightMarkerPass;
        std::unique_ptr<ClusterGrid> m_clusterGrid;
        std::unique_ptr<PointShadowMaps> m_pointShadowMaps;
        std::unique_ptr<ImGuiLayer> m_imgui;
        Timer m_timer;

        // Settings (renderer/RenderSettings.h) and what the scene contains
        RenderSettings m_renderSettings;
        DebugSettings m_debugSettings;
        RenderCaps m_caps;
        TonemapSettings m_tonemapSettings;
        SceneLighting m_lighting;
        PointLights m_pointLights;
        std::vector<PointLightGpu> m_pointLightData; // this frame's lights, rebuilt every frame
        std::vector<uint32_t> m_shadowedLights;      // light index of each cube shadow map slot, this frame
        uint32_t m_pointShadowDraws = 0;             // draws recorded into the cube shadow maps last frame
        int m_pointShadowViewSlot = 0;               // which cube the UI shows

        float m_roughnessOverrideValue = 0.3f; // slider values kept while an override is switched off
        float m_metallicOverrideValue = 1.0f;
        Camera m_camera;
        Camera m_homeCamera; // where "Reset camera" goes: framing the loaded scene
        FlyCameraController m_cameraController;
        bool m_showDemoWindow = false;
        bool m_showUI = true; // F1
        float m_smoothedFrameMs = 0.0f;
        std::filesystem::path m_lastCapturePath;
        SettingsRegistry m_settings;
    };
}
