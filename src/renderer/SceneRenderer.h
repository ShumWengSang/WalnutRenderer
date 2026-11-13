#pragma once

#include "renderer/RenderSettings.h"
#include "scene/Bounds.h"
#include "scene/PointLights.h"

#include <DirectXMath.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <vector>

namespace walnut
{
    class AccelerationStructures;
    class Camera;
    class ClusterGrid;
    class DeferredPass;
    class LightBuffer;
    class LightMarkerPass;
    class PointShadowMaps;
    class Renderer;
    class SceneGpuData;
    class ScenePass;
    class ShaderCompiler;
    class ShaderReloadable;
    class ShadowMap;
    class TonemapPass;
    struct Scene;
    struct SceneLighting;
    struct TonemapSettings;

    // What one frame shows: the camera, the lights, and the settings with this frame's decisions.
    struct FrameView
    {
        const Camera& camera;
        const SceneLighting& lighting;
        const RenderSettings& settings;
        const DebugSettings& debug;
        const FrameSettings& frame;
        const TonemapSettings& tonemap;
    };

    // Which lamps get a cube shadow map: the 'budget' ones closest to the camera, by how far the camera is from the
    // edge of their light (so a lamp lighting the space around the camera always qualifies). Returns their indices,
    // closest first: cube slot i belongs to lights[result[i]].
    std::vector<uint32_t> SelectShadowedLights(std::span<const PointLightGpu> lights, const DirectX::XMFLOAT3& camera,
                                               uint32_t budget);

    // Renders the 3D scene: owns the scene's GPU data and every pass, and records a frame's passes in order. The
    // Renderer owns the frame (command list, targets, swap chain); the Application owns the settings and the UI.
    class SceneRenderer
    {
    public:
        // Uploads the scene (if it has instances), creates the passes and builds the ray tracing structures where
        // supported. Waits for the uploads.
        SceneRenderer(Renderer& renderer, const ShaderCompiler& compiler, const std::filesystem::path& shaderDir,
                      const Scene& scene);
        ~SceneRenderer();

        SceneRenderer(const SceneRenderer&) = delete;
        SceneRenderer& operator=(const SceneRenderer&) = delete;

        // Records the frame between Renderer::BeginFrame and the UI: uploads the point lights (writing the shadowed
        // ones' cube slots into 'lights'), sets the frame constants, then
        //   light clusters -> sun shadow map -> point shadow cubes -> depth pre-pass ->
        //   forward, or G-buffer + deferred lighting -> light markers -> tonemap into the back buffer.
        void Record(const FrameView& view, std::span<PointLightGpu> lights);

        // Every pass whose shaders F5 recompiles.
        std::vector<ShaderReloadable*> GetShaderReloadables() const;

        const RenderCaps& GetCaps() const { return m_caps; }
        bool HasScene() const { return m_sceneData != nullptr; }
        const SceneGpuData* GetSceneData() const { return m_sceneData.get(); } // null without a scene
        const AccelerationStructures* GetAccelerationStructures() const { return m_accelerationStructures.get(); } // null without ray tracing
        const ShadowMap& GetShadowMap() const { return *m_shadowMap; }
        const PointShadowMaps& GetPointShadowMaps() const { return *m_pointShadowMaps; }
        const ClusterGrid& GetClusterGrid() const { return *m_clusterGrid; }

        // From the last recorded frame: the lights with a cube shadow map (by slot), and the draws into the cubes.
        std::span<const uint32_t> GetShadowedLights() const { return m_shadowedLights; }
        uint32_t GetPointShadowDraws() const { return m_pointShadowDraws; }

        static constexpr uint32_t kShadowMapSize = 2048;

    private:
        Renderer& m_renderer;
        Aabb m_sceneBounds;
        RenderCaps m_caps;

        // Members are destroyed in reverse order, all before the Renderer they were created from (which the owner
        // must keep alive longer). The acceleration structures read the scene's buffers, so they come after them.
        std::unique_ptr<SceneGpuData> m_sceneData; // null when the scene has nothing to draw
        std::unique_ptr<AccelerationStructures> m_accelerationStructures; // null without inline ray tracing support
        std::unique_ptr<ScenePass> m_scenePass;
        std::unique_ptr<ShadowMap> m_shadowMap;
        std::unique_ptr<PointShadowMaps> m_pointShadowMaps;
        std::unique_ptr<LightBuffer> m_lightBuffer;
        std::unique_ptr<ClusterGrid> m_clusterGrid;
        std::unique_ptr<DeferredPass> m_deferredPass;
        std::unique_ptr<LightMarkerPass> m_lightMarkerPass;
        std::unique_ptr<TonemapPass> m_tonemapPass;

        std::vector<uint32_t> m_shadowedLights;
        uint32_t m_pointShadowDraws = 0;
    };
}
