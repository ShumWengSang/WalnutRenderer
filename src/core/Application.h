#pragma once

#include "core/Input.h"
#include "core/Timer.h"
#include "renderer/AccelerationStructures.h"
#include "renderer/ClusterGrid.h"
#include "renderer/ScenePass.h"
#include "renderer/ShadowMap.h"
#include "renderer/DeferredPass.h"
#include "renderer/LightBuffer.h"
#include "renderer/LightMarkerPass.h"
#include "renderer/PointShadowMaps.h"
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
        void DrawStatsOverlay() const;
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

        // Demo state
        float m_clearColor[4] = {}; // starts as Renderer::kDefaultClearColor (the scene target's fast-clear color)
        TonemapSettings m_tonemapSettings;
        SceneViewSettings m_sceneView;
        SceneLighting m_lighting;
        PointLights m_pointLights;
        std::vector<PointLightGpu> m_pointLightData; // this frame's lights, rebuilt every frame
        std::vector<uint32_t> m_shadowedLights;      // light index of each cube shadow map slot, this frame
        uint32_t m_pointShadowDraws = 0;             // draws recorded into the cube shadow maps last frame
        int m_pointShadowViewSlot = 0;               // which cube the UI shows

        bool m_shadowsEnabled = true;
        float m_roughnessOverrideValue = 0.3f; // slider values kept while an override is switched off
        float m_metallicOverrideValue = 1.0f;
        Camera m_camera;
        Camera m_homeCamera; // where "Reset camera" goes: framing the loaded scene
        FlyCameraController m_cameraController;
        bool m_showDemoWindow = false;
        bool m_showUI = true; // F1
        float m_smoothedFrameMs = 0.0f;
        std::filesystem::path m_lastCapturePath;
    };
}
