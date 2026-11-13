#pragma once

#include "renderer/RenderSettings.h"
#include "scene/GltfLoader.h"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace walnut
{
    class Camera;
    class FlyCameraController;
    class Input;
    class Renderer;
    class SceneRenderer;
    struct Scene;
    struct SceneLighting;
    struct TonemapSettings;


    // The "Settings" window (one function per section) and the stats overlay. It edits the settings structs in
    // place; the frame reads them afterwards.
    class SettingsPanel
    {
    public:
        // What the panel shows and changes. Everything must outlive the panel.
        struct Context
        {
            Renderer& renderer;
            SceneRenderer& sceneRenderer;
            const Input& input;
            RenderSettings& renderSettings;
            DebugSettings& debugSettings;
            TonemapSettings& tonemapSettings;
            SceneLighting& lighting;
            Camera& camera;
            const Camera& homeCamera; // where "Reset camera" goes
            FlyCameraController& cameraController;
            const Scene& scene;
            const SceneFileInfo& sceneFile;
            const std::filesystem::path& shaderDir;
            const std::filesystem::path& lastCapturePath;
            std::function<void()> reloadShaders;
            std::function<void()> requestGpuCapture;
        };

        explicit SettingsPanel(const Context& context);

        // Call once per frame between ImGui's BeginFrame and Render. With visible = false (F1) only the frame time
        // is tracked.
        void Draw(float deltaSeconds, bool visible);

    private:
        void DrawStatsOverlay() const;
        void DrawShaderErrors();
        // What you tweak most (open by default).
        void DrawView();
        void DrawLighting();
        void DrawPointLights();
        void DrawSunShadows();
        void DrawTonemapping();
        // Details (closed by default).
        void DrawCamera();
        void DrawSceneInfo();
        void DrawRendererInfo();
        void DrawShaders();
        void DrawInput();
        void DrawPix();

        Renderer& m_renderer;
        SceneRenderer& m_sceneRenderer;
        const Input& m_input;
        RenderSettings& m_renderSettings;
        DebugSettings& m_debugSettings;
        TonemapSettings& m_tonemapSettings;
        SceneLighting& m_lighting;
        Camera& m_camera;
        const Camera& m_homeCamera;
        FlyCameraController& m_cameraController;
        const Scene& m_scene;
        const SceneFileInfo& m_sceneFile;
        const std::filesystem::path& m_shaderDir;
        const std::filesystem::path& m_lastCapturePath;
        std::function<void()> m_reloadShaders;
        std::function<void()> m_requestGpuCapture;

        // UI-only state.
        float m_smoothedFrameMs = 0.0f;
        float m_roughnessOverrideValue = 0.3f; // slider values kept while an override is switched off
        float m_metallicOverrideValue = 1.0f;
        int m_pointShadowViewSlot = 0; // which cube the cube shadow map view shows
        bool m_showDemoWindow = false;
    };
}
