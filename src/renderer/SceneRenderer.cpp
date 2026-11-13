#include "renderer/SceneRenderer.h"

#include "core/Log.h"
#include "gfx/Pix.h"
#include "gfx/ShaderCompiler.h"
#include "gfx/SwapChain.h"
#include "gfx/UploadContext.h"
#include "renderer/AccelerationStructures.h"
#include "renderer/ClusterGrid.h"
#include "renderer/DeferredPass.h"
#include "renderer/FrameConstants.h"
#include "renderer/GBuffer.h"
#include "renderer/LightBuffer.h"
#include "renderer/LightMarkerPass.h"
#include "renderer/PointShadowMaps.h"
#include "renderer/Renderer.h"
#include "renderer/SceneGpuData.h"
#include "renderer/ScenePass.h"
#include "renderer/ShadowMap.h"
#include "renderer/TonemapPass.h"
#include "scene/Camera.h"
#include "scene/Lighting.h"
#include "scene/Scene.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace walnut
{
    std::vector<uint32_t> SelectShadowedLights(std::span<const PointLightGpu> lights, const DirectX::XMFLOAT3& camera,
                                               uint32_t budget)
    {
        std::vector<std::pair<float, uint32_t>> byDistance;
        byDistance.reserve(lights.size());
        for (uint32_t i = 0; i < lights.size(); ++i)
        {
            const PointLightGpu& light = lights[i];
            const float dx = light.position.x - camera.x, dy = light.position.y - camera.y, dz = light.position.z - camera.z;
            byDistance.emplace_back(std::max(std::sqrt(dx * dx + dy * dy + dz * dz) - light.range, 0.0f), i);
        }
        const size_t count = std::min<size_t>(budget, byDistance.size());
        std::partial_sort(byDistance.begin(), byDistance.begin() + count, byDistance.end());
        std::vector<uint32_t> selected;
        selected.reserve(count);
        for (size_t slot = 0; slot < count; ++slot)
            selected.push_back(byDistance[slot].second);
        return selected;
    }

    SceneRenderer::SceneRenderer(Renderer& renderer, const ShaderCompiler& compiler, const std::filesystem::path& shaderDir,
                                 const Scene& scene)
        : m_renderer(renderer), m_sceneBounds(scene.bounds)
    {
        Device& device = renderer.GetDevice();
        DescriptorHeap& srvHeap = renderer.GetSrvHeap();
        ID3D12RootSignature* rootSignature = renderer.GetRootSignature();

        if (!scene.instances.empty())
            m_sceneData = std::make_unique<SceneGpuData>(device, renderer.GetUploadContext(), srvHeap, scene);
        m_scenePass = std::make_unique<ScenePass>(device, compiler, shaderDir / "scene.hlsl", rootSignature,
                                                  Renderer::kSceneColorFormat, Renderer::kDepthFormat);
        m_tonemapPass = std::make_unique<TonemapPass>(device, compiler, shaderDir / "tonemap.hlsl", rootSignature,
                                                      SwapChain::kSrgbFormat);
        m_deferredPass = std::make_unique<DeferredPass>(device, compiler, shaderDir / "deferred.hlsl", rootSignature,
                                                        Renderer::kSceneColorFormat, device.SupportsInlineRaytracing());
        m_lightBuffer = std::make_unique<LightBuffer>(device, srvHeap, Renderer::kFramesInFlight);
        m_pointShadowMaps = std::make_unique<PointShadowMaps>(device, srvHeap);
        m_clusterGrid = std::make_unique<ClusterGrid>(device, srvHeap, compiler, shaderDir / "clusters.hlsl", rootSignature,
                                                      Renderer::kFramesInFlight);
        m_lightMarkerPass = std::make_unique<LightMarkerPass>(device, compiler, shaderDir / "lightmarkers.hlsl", rootSignature,
                                                              Renderer::kSceneColorFormat, Renderer::kDepthFormat);
        m_shadowMap = std::make_unique<ShadowMap>(device, srvHeap, kShadowMapSize);

        // Execute every upload recorded while loading, before the first frame reads that data.
        renderer.GetUploadContext().Flush();

        // Ray tracing acceleration structures are built from the uploaded geometry, so after the flush.
        if (m_sceneData && device.SupportsInlineRaytracing())
        {
            m_accelerationStructures = std::make_unique<AccelerationStructures>(device, renderer.GetDirectQueue(), srvHeap,
                                                                                scene, *m_sceneData);
        }
        else if (m_sceneData)
            log::Warn("Inline ray tracing (tier 1.1) not supported: ray-traced features disabled");
        m_caps.raytracing = m_accelerationStructures != nullptr;
    }

    SceneRenderer::~SceneRenderer() = default;

    std::vector<ShaderReloadable*> SceneRenderer::GetShaderReloadables() const
    {
        return { m_scenePass.get(), m_tonemapPass.get(), m_deferredPass.get(), m_lightMarkerPass.get(), m_clusterGrid.get() };
    }

    void SceneRenderer::Record(const FrameView& view, std::span<PointLightGpu> lights)
    {
        const FrameSettings& frame = view.frame;
        Renderer& renderer = m_renderer;

        // The cube shadow map slots (only the cube-map technique uses them: rays shadow every lamp; off shadows none).
        m_shadowedLights = SelectShadowedLights(lights, view.camera.position, frame.cubeShadowBudget);
        for (uint32_t slot = 0; slot < m_shadowedLights.size(); ++slot)
            lights[m_shadowedLights[slot]].shadowSlot = slot;
        // The point lights go into this frame's upload memory, which Renderer::BeginFrame made reusable.
        const uint32_t pointLightBufferIndex = m_lightBuffer->Upload(renderer.GetFrameAllocator(), renderer.GetFrameIndex(), lights);

        // Everything the frame's shaders share: camera, lights, settings and where the frame's resources are. The
        // aspect ratio comes from the back buffer, so the image doesn't stretch when the window resizes.
        const SwapChain& swapChain = renderer.GetSwapChain();
        const float aspect = static_cast<float>(swapChain.GetWidth()) / static_cast<float>(swapChain.GetHeight());
        renderer.SetFrameConstants(BuildFrameConstants({
            .camera = view.camera,
            .aspect = aspect,
            .lighting = view.lighting,
            .settings = view.settings,
            .debug = view.debug,
            .frame = frame,
            .sceneBounds = m_sceneBounds,
            .shadowMapIndex = m_shadowMap->GetSrvIndex(),
            .shadowMapSize = m_shadowMap->GetSize(),
            .tlasIndex = m_accelerationStructures ? m_accelerationStructures->GetTlasSrvIndex() : UINT32_MAX,
            .sceneBindings = m_sceneData ? m_sceneData->GetBindings() : SceneGpuData::Bindings{ UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX },
            .pointLightBufferIndex = pointLightBufferIndex,
            .pointLightCount = static_cast<uint32_t>(lights.size()),
            .pointShadowMapIndex = m_pointShadowMaps->GetCubeArraySrvIndex(),
            .clusterGridIndex = m_clusterGrid->GetGridSrvIndex(),
            .clusterLightIndicesIndex = m_clusterGrid->GetIndicesSrvIndex(),
        }));
        ID3D12GraphicsCommandList7* commandList = renderer.GetCommandList();

        // 0. Sort the point lights into the cluster grid (compute), before anything reads it (skipped when nothing
        //    reads it: see FrameSettings::buildClusters).
        if (m_sceneData && frame.buildClusters)
            m_clusterGrid->Build(commandList, renderer.GetFrameIndex());

        // 1. Depth from the sun's point of view, into the shadow map. With ray-traced shadows the casters are skipped
        //    (nothing reads the map), except for the difference view, which compares both. The pass still clears +
        //    transitions it, which keeps it valid for the UI's shadow map view.
        {
            PIXScopedEvent(commandList, pix::kColorGpu, L"ShadowMap");
            m_shadowMap->Begin(commandList);
            if (m_sceneData && frame.drawSunCasters)
                m_scenePass->RenderShadowDepth(commandList, *m_sceneData, frame);
            m_shadowMap->End(commandList);
        }

        // 1b. Cube shadow maps for the shadowed point lights: six 90-degree depth renders per lamp, each with only the
        //     meshes whose bounds reach the lamp's sphere.
        {
            PIXScopedEvent(commandList, pix::kColorGpu, L"Point shadows");
            m_pointShadowMaps->Begin(commandList);
            m_pointShadowDraws = 0;
            for (uint32_t slot = 0; slot < m_shadowedLights.size(); ++slot)
            {
                const PointLightGpu& light = lights[m_shadowedLights[slot]];
                for (uint32_t face = 0; face < PointShadowMaps::kFaceCount; ++face)
                {
                    DirectX::XMFLOAT4X4 faceViewProjection;
                    DirectX::XMStoreFloat4x4(&faceViewProjection,
                                             PointShadowMaps::GetFaceViewProjection(light.position, light.range, face));
                    m_pointShadowMaps->BeginFace(commandList, slot, face);
                    m_pointShadowDraws += m_scenePass->RenderPointShadowDepth(
                        commandList, *m_sceneData, frame, renderer.GetFrameAllocator().AllocateConstants(faceViewProjection),
                        light.position, light.range);
                }
            }
            m_pointShadowMaps->End(commandList);
            // The faces set b1 to their camera; nothing after them should see that.
            renderer.ResetDrawData();
        }

        // 2. The scene from the camera, into the HDR scene color target. With the depth pre-pass, depth first: the
        //    scene pass then shades only the closest surface of each pixel.
        const bool deferred = m_sceneData && frame.deferred;
        if (m_sceneData && frame.depthPrepass)
        {
            renderer.BeginDepthPrepass();
            m_scenePass->RenderDepthPrepass(commandList, *m_sceneData, frame);
        }
        if (deferred)
        {
            // Deferred: the draws write each visible pixel's material into the G-buffer, then one fullscreen pass
            // reads it back.
            renderer.BeginGBufferPass();
            m_scenePass->RenderGBuffer(commandList, *m_sceneData, frame);
            renderer.BeginDeferredPass();
            m_deferredPass->Render(commandList, renderer.GetGBuffer().GetDeferredInputs(renderer.GetDepthSrvIndex()),
                                   frame.view, frame.raytracedVariant);
        }
        else
        {
            renderer.BeginScenePass();
            if (m_sceneData)
                m_scenePass->Render(commandList, *m_sceneData, frame);
        }

        // 3. Markers at the point lights: on top of the shaded scene, hidden behind closer geometry.
        if (m_sceneData && view.debug.lightMarkers && !lights.empty())
        {
            renderer.BeginOverlayPass();
            m_lightMarkerPass->Render(commandList, static_cast<uint32_t>(lights.size()), 0.04f * view.lighting.pointLightRange);
        }

        // 4. The 3D scene is done; from here on we draw into the back buffer.
        renderer.BeginBackBufferPass();
        m_tonemapPass->Render(commandList, renderer.GetSceneColorSrvIndex(), view.tonemap);
    }
}
