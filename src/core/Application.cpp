#include "core/Application.h"

#include "core/Log.h"
#include "core/StringUtils.h"
#include "core/Window.h"
#include "gfx/Pix.h"
#include "gfx/ShaderCompiler.h"
#include "renderer/Renderer.h"
#include "ui/ImGuiLayer.h"

#include <imgui.h>

#include <shellapi.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>

namespace walnut
{
    namespace
    {
        std::filesystem::path GetExecutableDirectory()
        {
            std::wstring buffer(MAX_PATH, L'\0');
            DWORD length = 0;
            while ((length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()))) == buffer.size())
                buffer.resize(buffer.size() * 2);
            buffer.resize(length);
            return std::filesystem::path(buffer).parent_path();
        }

        std::string GetKeyName(int vk)
        {
            const UINT scanCode = MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_VSC_EX);
            LONG lParam = static_cast<LONG>((scanCode & 0xFF) << 16);
            if ((scanCode & 0xFF00) == 0xE000)
                lParam |= 1 << 24; // extended key (arrows, Insert, ...)
            wchar_t name[64]{};
            if (scanCode != 0 && GetKeyNameTextW(lParam, name, static_cast<int>(std::size(name))) > 0)
                return ToUtf8(name);
            return std::format("VK 0x{:02X}", vk);
        }
    }

    Application::Application(const AppOptions& options)
        : m_options(options), m_exeDir(GetExecutableDirectory())
    {
        // The GPU capturer hooks D3D12 creation, so it has to be in the process before the device exists.
        if (options.pixGpuCapturer)
            pix::LoadGpuCapturer();

        // Converted into the engine's Scene and kept on the CPU; the GPU doesn't see it yet. The glTF document is
        // freed right after, since everything needed was copied out of it.
        m_scenePath = ResolveAssetPath(options.scene);
        if (!std::filesystem::is_regular_file(m_scenePath))
        {
            m_sceneError = "not found: run tools\\fetch-assets.ps1";
            log::Warn("Scene not found: {} (run tools\\fetch-assets.ps1 to download Sponza)", ToUtf8(m_scenePath.wstring()));
        }
        else
        {
            const auto loadStart = std::chrono::steady_clock::now();
            const std::unique_ptr<GltfDocument> document = GltfDocument::Load(m_scenePath, m_sceneError);
            m_sceneLoadMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - loadStart).count();
            if (document)
            {
                m_sceneSummary = SummarizeGltf(document->GetData());
                log::Info("Scene: {} (parsed in {:.1f} ms)", ToUtf8(m_scenePath.wstring()), m_sceneLoadMs);
                LogGltfSummary(*m_sceneSummary);

                const auto buildStart = std::chrono::steady_clock::now();
                m_scene = BuildSceneFromGltf(document->GetData(), m_scenePath.parent_path());
                m_sceneBuildMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - buildStart).count();
                log::Info("Scene built in {:.1f} ms:", m_sceneBuildMs);
                LogSceneStats(m_scene);
            }
            else
                log::Error("Scene {} failed to load: {}", ToUtf8(m_scenePath.wstring()), m_sceneError);
        }

        m_window = std::make_unique<Window>(WindowDesc{ L"WalnutRenderer", 1280, 720 });

        // Register input before ImGui so input always sees every message, even ones ImGui consumes.
        m_input.RegisterRawMouse(m_window->GetHandle());
        m_window->AddMessageHandler([this](HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
            m_input.HandleMessage(hwnd, msg, wParam, lParam);
            return false;
        });

        RendererDesc rendererDesc;
        rendererDesc.device.enableDebugLayer = options.debugLayer;
        rendererDesc.device.enableGpuValidation = options.gpuValidation;
        rendererDesc.vsync = true;
        m_renderer = std::make_unique<Renderer>(*m_window, rendererDesc);
        std::copy(std::begin(Renderer::kDefaultClearColor), std::end(Renderer::kDefaultClearColor), m_debugSettings.clearColor);

#ifdef _DEBUG
        constexpr bool kShaderDebugInfo = true;
#else
        constexpr bool kShaderDebugInfo = false;
#endif
        // Prefer the source tree's shaders, so edits there are picked up by Reload (F5) without rebuilding.
        // Fall back to the copy CMake puts next to the exe.
        m_shaderDir = std::filesystem::path(u8"" WALNUT_SHADER_SOURCE_DIR).lexically_normal();
        if (!std::filesystem::is_directory(m_shaderDir))
            m_shaderDir = m_exeDir / "shaders";
        log::Info("Compiling shaders from {}", ToUtf8(m_shaderDir.wstring()));
        m_shaderCompiler = std::make_unique<ShaderCompiler>(kShaderDebugInfo, m_shaderDir);
        m_scenePass = std::make_unique<ScenePass>(m_renderer->GetDevice(), *m_shaderCompiler, m_shaderDir / "scene.hlsl",
                                                  m_renderer->GetRootSignature(), Renderer::kSceneColorFormat,
                                                  Renderer::kDepthFormat);
        if (!m_scene.instances.empty())
        {
            m_sceneData = std::make_unique<SceneGpuData>(m_renderer->GetDevice(), m_renderer->GetUploadContext(),
                                                         m_renderer->GetSrvHeap(), m_scene);
            // The pixels were copied into the upload ring, so the CPU copy (~130 MB for Sponza) can go.
            for (SceneTexture& texture : m_scene.textures)
                texture.mips = {};
            PlaceStartCamera(m_homeCamera, m_scene.bounds);
            m_camera = m_homeCamera;
            // Cross the scene's bounding sphere in about 8 seconds.
            const float diameter = m_scene.bounds.Diagonal();
            m_cameraController.moveSpeed = std::clamp(diameter / 8.0f, 0.1f, 100.0f);

            // Lamps scaled to the scene: Sponza (37 m across) gets ~2.6 m of range, small pools of light that only
            // overlap where lamps are close. Intensity grows with range^2, so the pools look the same at any scale.
            m_pointLights.Generate(m_scene.bounds);
            m_lighting.pointLightRange = 0.07f * diameter;
            m_lighting.pointLightIntensity = 0.3f * m_lighting.pointLightRange * m_lighting.pointLightRange;
        }
        m_tonemapPass = std::make_unique<TonemapPass>(m_renderer->GetDevice(), *m_shaderCompiler,
                                                      m_shaderDir / "tonemap.hlsl", m_renderer->GetRootSignature(),
                                                      SwapChain::kSrgbFormat);
        m_deferredPass = std::make_unique<DeferredPass>(m_renderer->GetDevice(), *m_shaderCompiler,
                                                        m_shaderDir / "deferred.hlsl", m_renderer->GetRootSignature(),
                                                        Renderer::kSceneColorFormat, m_renderer->GetDevice().SupportsInlineRaytracing());
        m_lightBuffer = std::make_unique<LightBuffer>(m_renderer->GetDevice(), m_renderer->GetSrvHeap(), Renderer::kFramesInFlight);
        m_pointShadowMaps = std::make_unique<PointShadowMaps>(m_renderer->GetDevice(), m_renderer->GetSrvHeap());
        m_clusterGrid = std::make_unique<ClusterGrid>(m_renderer->GetDevice(), m_renderer->GetSrvHeap(), *m_shaderCompiler,
                                                      m_shaderDir / "clusters.hlsl", m_renderer->GetRootSignature(),
                                                      Renderer::kFramesInFlight);
        m_lightMarkerPass = std::make_unique<LightMarkerPass>(m_renderer->GetDevice(), *m_shaderCompiler,
                                                              m_shaderDir / "lightmarkers.hlsl", m_renderer->GetRootSignature(),
                                                              Renderer::kSceneColorFormat, Renderer::kDepthFormat);
        m_shadowMap = std::make_unique<ShadowMap>(m_renderer->GetDevice(), m_renderer->GetSrvHeap(), kShadowMapSize);

        // Execute every upload recorded while loading, before the first frame reads that data.
        m_renderer->GetUploadContext().Flush();

        // Ray tracing acceleration structures are built from the uploaded geometry, so after the flush.
        if (m_sceneData && m_renderer->GetDevice().SupportsInlineRaytracing())
        {
            m_accelerationStructures = std::make_unique<AccelerationStructures>(
                m_renderer->GetDevice(), m_renderer->GetDirectQueue(), m_renderer->GetSrvHeap(), m_scene, *m_sceneData);
        }
        else if (m_sceneData)
            log::Warn("Inline ray tracing (tier 1.1) not supported: ray-traced features disabled");
        m_caps.raytracing = m_accelerationStructures != nullptr;

        m_imgui = std::make_unique<ImGuiLayer>(*m_window, *m_renderer);

        // Reproducible starts: camera and settings from the command line, after the scene-based defaults above.
        if (options.camera)
        {
            m_camera.position = { options.camera->x, options.camera->y, options.camera->z };
            m_camera.yaw = DirectX::XMConvertToRadians(options.camera->yawDegrees);
            m_camera.pitch = DirectX::XMConvertToRadians(options.camera->pitchDegrees);
        }
        RegisterSettings();
        for (const auto& [name, value] : options.settings)
            m_settings.Set(name, value);
    }

    double Application::GetAnimationTime() const
    {
        return m_options.fixedTime ? *m_options.fixedTime : m_timer.GetTotalSeconds();
    }

    void Application::RegisterSettings()
    {
        using E = SettingsRegistry::EnumName;
        SettingsRegistry& s = m_settings;
        std::vector<E> views;
        for (const DebugViewInfo& view : kDebugViews)
            views.push_back({ view.setting, static_cast<int>(view.view) });
        s.AddEnum("view.show", &m_debugSettings.view, std::move(views));
        s.AddEnum("view.path", &m_renderSettings.path, { E{ "forward", 0 }, E{ "deferred", 1 } });
        s.AddBool("view.prepass", &m_renderSettings.depthPrepass);
        s.AddEnum("view.normalMapping", &m_renderSettings.normalMapping, { E{ "off", 0 }, E{ "mesh", 1 }, E{ "derived", 2 } });
        s.AddBool("view.alphaTest", &m_renderSettings.alphaTest);
        s.AddBool("view.backfaceCulling", &m_renderSettings.backfaceCulling);
        s.AddFloat("view.roughnessOverride", &m_debugSettings.roughnessOverride);
        s.AddFloat("view.metallicOverride", &m_debugSettings.metallicOverride);

        s.AddEnum("lighting.shading", &m_renderSettings.shading, { E{ "lambert", 0 }, E{ "pbr", 1 } });
        s.AddAngle("sun.azimuth", &m_lighting.sunAzimuth);
        s.AddAngle("sun.elevation", &m_lighting.sunElevation);
        s.AddFloat("sun.intensity", &m_lighting.sunIntensity);
        s.AddBool("sun.shadows", &m_renderSettings.sunShadows.enabled);
        s.AddCustom("sun.technique",
                    [this](std::string_view v) {
                        if (v == "map") { m_renderSettings.sunShadows.raytraced = false; return true; }
                        if (v == "ray") { m_renderSettings.sunShadows.raytraced = true; return true; }
                        return false;
                    },
                    [this] { return std::string(m_renderSettings.sunShadows.raytraced ? "ray" : "map"); });
        s.AddFloat("sun.constantBias", &m_renderSettings.sunShadows.constantBias);
        s.AddFloat("sun.slopeBias", &m_renderSettings.sunShadows.slopeBias);
        s.AddFloat("sun.normalOffset", &m_renderSettings.sunShadows.normalOffset);
        s.AddInt("sun.softness", &m_renderSettings.sunShadows.filterRadius, -1, 4);
        s.AddFloat("ambient.intensity", &m_lighting.ambientIntensity);

        s.AddInt("points.count", &m_lighting.pointLightCount, 0, static_cast<int>(PointLights::kMaxLights));
        s.AddFloat("points.range", &m_lighting.pointLightRange);
        s.AddFloat("points.intensity", &m_lighting.pointLightIntensity);
        s.AddBool("points.animate", &m_lighting.animatePointLights);
        s.AddBool("points.markers", &m_debugSettings.lightMarkers);
        s.AddBool("points.clustered", &m_renderSettings.clusteredLighting);
        s.AddEnum("points.shadows", &m_renderSettings.pointShadows.technique, { E{ "off", 0 }, E{ "cube", 1 }, E{ "ray", 2 } });
        s.AddInt("points.budget", &m_renderSettings.pointShadows.budget, 0, static_cast<int>(PointShadowMaps::kMaxShadowedLights));

        s.AddFloat("tonemap.exposure", &m_tonemapSettings.exposureStops);
        s.AddEnum("tonemap.curve", &m_tonemapSettings.op, { E{ "clamp", 0 }, E{ "reinhard", 1 }, E{ "aces", 2 } });
        s.AddCustom("renderer.vsync",
                    [this](std::string_view v) {
                        if (v != "0" && v != "1")
                            return false;
                        m_renderer->SetVsync(v == "1");
                        return true;
                    },
                    [this] { return std::string(m_renderer->GetVsync() ? "1" : "0"); });
    }

    Application::~Application()
    {
        if (m_renderer)
            m_renderer->WaitForGpu();

        // Tear down in dependency order: everything that holds GPU objects goes before the renderer.
        m_imgui.reset();
        m_accelerationStructures.reset();
        m_scenePass.reset();
        m_sceneData.reset();
        m_shadowMap.reset();
        m_tonemapPass.reset();
        m_deferredPass.reset();
        m_lightBuffer.reset();
        m_lightMarkerPass.reset();
        m_clusterGrid.reset();
        m_pointShadowMaps.reset();
        m_shaderCompiler.reset();
        m_renderer.reset();
        m_window.reset();

        if (m_options.debugLayer || m_options.gpuValidation)
            ReportLiveObjects();
    }

    int Application::Run()
    {
        if (m_options.listSettings)
        {
            log::Info("Settings (--set=NAME=VALUE):");
            for (const std::string& line : m_settings.List())
                log::Info("  {}", line);
            return 0;
        }

        log::Info("Entering main loop");
        m_timer.Reset();

        while (true)
        {
            PIXScopedEvent(pix::kColorFrame, L"Frame");

            m_input.BeginFrame();
            {
                PIXScopedEvent(pix::kColorCpu, L"PumpMessages");
                if (!m_window->PumpMessages())
                    break;
            }

            const float deltaSeconds = m_timer.Tick();
            PIXReportCounter(L"Frame time (ms)", deltaSeconds * 1000.0f);

            uint32_t width = 0;
            uint32_t height = 0;
            if (m_window->ConsumeResize(width, height))
                m_renderer->Resize(width, height);

            if (m_window->IsMinimized())
            {
                Sleep(16); // nothing to present; don't spin
                continue;
            }

            if (m_options.pixCaptureFrame != 0 && m_renderer->GetFrameCount() == m_options.pixCaptureFrame)
                RequestGpuCapture();

            {
                PIXScopedEvent(pix::kColorCpu, L"Update + UI");
                m_imgui->BeginFrame();
                m_input.SetUICapture(m_imgui->WantsKeyboard(), m_imgui->WantsMouse());

                Update(deltaSeconds);
                DrawUI(deltaSeconds);
            }

            // This frame's decisions, from the settings the UI just changed.
            const FrameSettings frame = ResolveFrameSettings(m_renderSettings, m_debugSettings, m_caps);

            // The aspect ratio comes from the back buffer, so the image doesn't stretch when the window resizes.
            const SwapChain& swapChain = m_renderer->GetSwapChain();
            const float aspect = static_cast<float>(swapChain.GetWidth()) / static_cast<float>(swapChain.GetHeight());

            // Everything the frame's shaders share: camera and lights. Colors are multiplied by their intensity here,
            // so shaders only see the resulting light amounts.
            FrameConstants frameConstants{};
            const DirectX::XMMATRIX viewProjection = m_camera.GetViewProjectionMatrix(aspect);
            DirectX::XMStoreFloat4x4(&frameConstants.viewProjection, viewProjection);
            DirectX::XMStoreFloat4x4(&frameConstants.inverseViewProjection, DirectX::XMMatrixInverse(nullptr, viewProjection));
            // Clustered lighting: view space, the field of view, and the depths the slices span (the near plane to
            // the farthest corner of the scene's bounds: nothing visible is farther).
            DirectX::XMStoreFloat4x4(&frameConstants.view, m_camera.GetViewMatrix());
            frameConstants.tanHalfFovY = std::tan(m_camera.verticalFov * 0.5f);
            frameConstants.tanHalfFovX = frameConstants.tanHalfFovY * aspect;
            frameConstants.clusterNear = m_camera.nearZ;
            frameConstants.clusterFar = std::max(m_camera.nearZ * 2.0f, m_scene.bounds.FarthestCornerDistance(m_camera.position));
            frameConstants.clusterGridIndex = m_clusterGrid->GetGridSrvIndex();
            frameConstants.clusterLightIndicesIndex = m_clusterGrid->GetIndicesSrvIndex();
            frameConstants.clusteredLighting = m_renderSettings.clusteredLighting ? 1u : 0u;
            frameConstants.pointShadowMapIndex = m_pointShadowMaps->GetCubeArraySrvIndex();
            frameConstants.pointShadowNearScale = PointShadowMaps::kNearScale;
            frameConstants.sunShadowRaytraced = frame.sunRaytraced ? 1u : 0u;
            frameConstants.pointShadowTechnique = static_cast<uint32_t>(frame.pointShadows);
            frameConstants.cameraPosition = m_camera.position;
            frameConstants.sunDirection = m_lighting.GetSunDirection();
            const auto scaled = [](const DirectX::XMFLOAT3& color, float intensity) {
                return DirectX::XMFLOAT3{ color.x * intensity, color.y * intensity, color.z * intensity };
            };
            frameConstants.sunIlluminance = scaled(m_lighting.sunColor, m_lighting.sunIntensity);
            frameConstants.skyRadiance = scaled(m_lighting.skyColor, m_lighting.ambientIntensity);
            frameConstants.groundRadiance = scaled(m_lighting.groundColor, m_lighting.ambientIntensity);
            frameConstants.roughnessOverride = m_debugSettings.roughnessOverride;
            frameConstants.metallicOverride = m_debugSettings.metallicOverride;
            frameConstants.shadingModel = static_cast<uint32_t>(frame.shading);
            DirectX::XMStoreFloat4x4(&frameConstants.sunViewProjection,
                                     m_lighting.GetSunViewProjection(m_scene.bounds));
            frameConstants.shadowMapIndex = m_shadowMap->GetSrvIndex();
            frameConstants.shadowsEnabled = frame.sunShadows ? 1u : 0u;
            const float shadowExtent = GetSunShadowExtent(m_scene.bounds);
            frameConstants.shadowTexelWorldSize = shadowExtent / static_cast<float>(m_shadowMap->GetSize());
            frameConstants.shadowDepthPerMeter = 1.0f / shadowExtent;
            frameConstants.shadowConstantBias = m_renderSettings.sunShadows.constantBias;
            frameConstants.shadowSlopeBias = m_renderSettings.sunShadows.slopeBias;
            frameConstants.shadowNormalOffset = m_renderSettings.sunShadows.normalOffset;
            frameConstants.shadowFilterRadius = m_renderSettings.sunShadows.filterRadius < 0 ? UINT32_MAX
                                                                                  : static_cast<uint32_t>(m_renderSettings.sunShadows.filterRadius);
            frameConstants.tlasIndex = m_accelerationStructures ? m_accelerationStructures->GetTlasSrvIndex() : UINT32_MAX;
            frameConstants.rayAlphaTest = frame.alphaTest ? 1u : 0u;
            if (m_sceneData)
            {
                const SceneGpuData::Bindings bindings = m_sceneData->GetBindings();
                frameConstants.vertexBufferIndex = bindings.vertices;
                frameConstants.instanceBufferIndex = bindings.instances;
                frameConstants.materialBufferIndex = bindings.materials;
                frameConstants.indexBufferIndex = bindings.indices;
            }

            {
                PIXScopedEvent(pix::kColorCpu, L"Render");
                m_renderer->BeginFrame(m_debugSettings.clearColor);
                // The point lights go into this frame's upload memory, which BeginFrame just made reusable.
                if (m_sceneData)
                    m_pointLights.Update(GetAnimationTime(), static_cast<uint32_t>(m_lighting.pointLightCount),
                                         m_lighting.pointLightRange, m_lighting.pointLightIntensity,
                                         m_lighting.animatePointLights, m_pointLightData);
                // Which lamps get a cube shadow map: the ones closest to the camera (by how far the camera is from
                // the edge of their light, so a lamp lighting the space around the camera always qualifies).
                m_shadowedLights.clear();
                {
                    // Only the cube-map technique uses slots (rays shadow every lamp; off shadows none).
                    const uint32_t budget = frame.cubeShadowBudget;
                    std::vector<std::pair<float, uint32_t>> byDistance;
                    byDistance.reserve(m_pointLightData.size());
                    for (uint32_t i = 0; i < m_pointLightData.size(); ++i)
                    {
                        const PointLightGpu& light = m_pointLightData[i];
                        const float dx = light.position.x - m_camera.position.x, dy = light.position.y - m_camera.position.y,
                                    dz = light.position.z - m_camera.position.z;
                        byDistance.emplace_back(std::max(std::sqrt(dx * dx + dy * dy + dz * dz) - light.range, 0.0f), i);
                    }
                    const size_t count = std::min<size_t>(budget, byDistance.size());
                    std::partial_sort(byDistance.begin(), byDistance.begin() + count, byDistance.end());
                    for (size_t slot = 0; slot < count; ++slot)
                    {
                        m_pointLightData[byDistance[slot].second].shadowSlot = static_cast<uint32_t>(slot);
                        m_shadowedLights.push_back(byDistance[slot].second);
                    }
                }
                frameConstants.pointLightBufferIndex = m_lightBuffer->Upload(m_renderer->GetFrameAllocator(),
                                                                             m_renderer->GetFrameIndex(), m_pointLightData);
                frameConstants.pointLightCount = static_cast<uint32_t>(m_pointLightData.size());
                m_renderer->SetFrameConstants(frameConstants);
                ID3D12GraphicsCommandList7* commandList = m_renderer->GetCommandList();

                // 0. Sort the point lights into the cluster grid (compute), before anything reads it (skipped when
                //    nothing reads it: see FrameSettings::buildClusters).
                if (m_sceneData && frame.buildClusters)
                    m_clusterGrid->Build(commandList, m_renderer->GetFrameIndex());

                // 1. Depth from the sun's point of view, into the shadow map. With ray-traced shadows the casters are
                //    skipped (nothing reads the map), except for the difference view, which compares both. The pass
                //    still clears + transitions it, which keeps it valid for the UI's shadow map view.
                {
                    PIXScopedEvent(commandList, pix::kColorGpu, L"ShadowMap");
                    m_shadowMap->Begin(commandList);
                    if (m_sceneData && frame.drawSunCasters)
                        m_scenePass->RenderShadowDepth(commandList, *m_sceneData, frame);
                    m_shadowMap->End(commandList);
                }

                // 1b. Cube shadow maps for the shadowed point lights: six 90-degree depth renders per lamp, each with
                //     only the meshes whose bounds reach the lamp's sphere.
                {
                    PIXScopedEvent(commandList, pix::kColorGpu, L"Point shadows");
                    m_pointShadowMaps->Begin(commandList);
                    m_pointShadowDraws = 0;
                    for (uint32_t slot = 0; slot < m_shadowedLights.size(); ++slot)
                    {
                        const PointLightGpu& light = m_pointLightData[m_shadowedLights[slot]];
                        for (uint32_t face = 0; face < PointShadowMaps::kFaceCount; ++face)
                        {
                            DirectX::XMFLOAT4X4 faceViewProjection;
                            DirectX::XMStoreFloat4x4(&faceViewProjection,
                                                     PointShadowMaps::GetFaceViewProjection(light.position, light.range, face));
                            m_pointShadowMaps->BeginFace(commandList, slot, face);
                            m_pointShadowDraws += m_scenePass->RenderPointShadowDepth(
                                commandList, *m_sceneData, frame, m_renderer->GetFrameAllocator().AllocateConstants(faceViewProjection),
                                light.position, light.range);
                        }
                    }
                    m_pointShadowMaps->End(commandList);
                }

                // 2. The scene from the camera, into the HDR scene color target. With the depth pre-pass, depth
                //    first: the scene pass then shades only the closest surface of each pixel.
                const bool deferred = m_sceneData && frame.deferred;
                if (m_sceneData && frame.depthPrepass)
                {
                    m_renderer->BeginDepthPrepass();
                    m_scenePass->RenderDepthPrepass(commandList, *m_sceneData, frame);
                }
                if (deferred)
                {
                    // Deferred: the draws write each visible pixel's material into the G-buffer, then one
                    // fullscreen pass reads it back.
                    m_renderer->BeginGBufferPass();
                    m_scenePass->RenderGBuffer(commandList, *m_sceneData, frame);
                    m_renderer->BeginDeferredPass();
                    const GBuffer& gbuffer = m_renderer->GetGBuffer();
                    const DeferredInputs inputs{
                        .baseColor = gbuffer.GetSrvIndex(GBuffer::BaseColor),
                        .normals = gbuffer.GetSrvIndex(GBuffer::Normals),
                        .material = gbuffer.GetSrvIndex(GBuffer::Material),
                        .emissive = gbuffer.GetSrvIndex(GBuffer::Emissive),
                        .depth = m_renderer->GetDepthSrvIndex(),
                    };
                    m_deferredPass->Render(commandList, inputs, frame.view, frame.raytracedVariant);
                }
                else
                {
                    m_renderer->BeginScenePass();
                    if (m_sceneData)
                        m_scenePass->Render(commandList, *m_sceneData, frame);
                }

                // Markers at the point lights: on top of the shaded scene, hidden behind closer geometry.
                if (m_sceneData && m_debugSettings.lightMarkers && !m_pointLightData.empty())
                {
                    m_renderer->BeginOverlayPass();
                    m_lightMarkerPass->Render(commandList, static_cast<uint32_t>(m_pointLightData.size()),
                                              0.04f * m_lighting.pointLightRange);
                }

                // The 3D scene is done; from here on we draw into the back buffer.
                m_renderer->BeginBackBufferPass();
                m_tonemapPass->Render(commandList, m_renderer->GetSceneColorSrvIndex(), m_tonemapSettings);
                // --screenshot: the frame as tonemapped, before the UI is drawn over it.
                const bool screenshotFrame = !m_options.screenshot.empty() &&
                                             m_renderer->GetFrameCount() + 1 == m_options.screenshotFrame;
                if (screenshotFrame)
                    m_renderer->CaptureBackBuffer();
                m_renderer->BeginUiPass();
                m_imgui->Render(commandList);
                m_renderer->EndFrame();
                if (screenshotFrame)
                {
                    m_renderer->SaveScreenshot(m_options.screenshot);
                    break;
                }
            }

            if (m_options.exitAfterFrames != 0 && m_renderer->GetFrameCount() >= m_options.exitAfterFrames)
            {
                log::Info("Rendered {} frames, exiting (--exit-after-frames)", m_renderer->GetFrameCount());
                break;
            }
            if (m_options.exitAfterSeconds > 0.0 && m_timer.GetTotalSeconds() >= m_options.exitAfterSeconds)
            {
                log::Info("Ran for {:.1f} s ({} frames), exiting (--exit-after-seconds)",
                          m_timer.GetTotalSeconds(), m_renderer->GetFrameCount());
                break;
            }
        }

        m_renderer->WaitForGpu();
        log::Info("Main loop exited after {} frames", m_renderer->GetFrameCount());
        return 0;
    }

    void Application::Update(float deltaSeconds)
    {
        if (m_input.WasKeyPressed(VK_ESCAPE))
            PostMessageW(m_window->GetHandle(), WM_CLOSE, 0, 0);
        if (m_input.WasKeyPressed(VK_F5))
            ReloadShaders();
        if (m_input.WasKeyPressed(VK_F1))
            m_showUI = !m_showUI;

        m_cameraController.Update(m_camera, m_input, deltaSeconds);
    }

    void Application::ReloadShaders()
    {
        // The old PSOs may still be referenced by in-flight frames.
        m_renderer->WaitForGpu();
        bool allOk = true;
        for (ShaderReloadable* pass : GetShaderReloadables())
            allOk &= pass->ReloadShaders();
        if (allOk)
            log::Info("Shaders reloaded");
    }

    std::vector<ShaderReloadable*> Application::GetShaderReloadables() const
    {
        return { m_scenePass.get(), m_tonemapPass.get(), m_deferredPass.get(), m_lightMarkerPass.get(), m_clusterGrid.get() };
    }

    void Application::RequestGpuCapture()
    {
        SYSTEMTIME t{};
        GetLocalTime(&t);
        const std::string name = std::format("WalnutRenderer_{:04}{:02}{:02}_{:02}{:02}{:02}_frame{}.wpix",
                                             t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond,
                                             m_renderer->GetFrameCount());
        const std::filesystem::path path = m_exeDir / "captures" / name;
        if (pix::CaptureNextFrames(path, 1))
            m_lastCapturePath = path;
    }

    void Application::DrawPixUI()
    {
        if (!ImGui::CollapsingHeader("PIX"))
            return;

        const bool capturerLoaded = pix::IsGpuCapturerLoaded();
        ImGui::Text("GPU capturer: %s", capturerLoaded ? "loaded" : "not loaded (run with --pix)");
        ImGui::Text("Attached for GPU capture: %s", PIXIsAttachedForGpuCapture() ? "yes" : "no");

        ImGui::BeginDisabled(!capturerLoaded);
        if (ImGui::Button("Capture next frame"))
            RequestGpuCapture();
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled("(when attached from PIX, F11 also captures)");

        if (!m_lastCapturePath.empty())
        {
            const std::string capture = ToUtf8(m_lastCapturePath.filename().wstring());
            ImGui::TextWrapped("Last capture: %s", capture.c_str());
            if (ImGui::Button("Open in PIX") && std::filesystem::exists(m_lastCapturePath))
                ShellExecuteW(nullptr, L"open", m_lastCapturePath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
    }

    void Application::DrawStatsOverlay() const
    {
        // A small, borderless, see-through window pinned to the top-right corner: numbers you want while looking at
        // the scene. Not saved to imgui.ini and never takes focus, so it doesn't get in the way.
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - 10.0f, viewport->WorkPos.y + 10.0f),
                                ImGuiCond_Always, ImVec2(1.0f, 0.0f));
        ImGui::SetNextWindowBgAlpha(0.35f);
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                       ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoInputs;
        if (ImGui::Begin("Stats", nullptr, flags))
        {
            const SwapChain& swapChain = m_renderer->GetSwapChain();
            ImGui::Text("%.2f ms  (%.0f FPS)", m_smoothedFrameMs, m_smoothedFrameMs > 0.0f ? 1000.0f / m_smoothedFrameMs : 0.0f);
            ImGui::Text("%u x %u%s", swapChain.GetWidth(), swapChain.GetHeight(), m_renderer->GetVsync() ? ", vsync" : "");
            if (m_sceneData)
                ImGui::Text("%u draws, %.2f M triangles", m_sceneData->GetDrawCount(),
                            static_cast<double>(m_scene.indices.size() / 3) / 1e6);
            ImGui::Text("Camera (%.1f, %.1f, %.1f)", m_camera.position.x, m_camera.position.y, m_camera.position.z);
            ImGui::TextDisabled("F1: hide UI");
        }
        ImGui::End();
    }

    void Application::DrawUI(float deltaSeconds)
    {
        const float frameMs = deltaSeconds * 1000.0f;
        m_smoothedFrameMs = (m_smoothedFrameMs == 0.0f) ? frameMs : m_smoothedFrameMs * 0.95f + frameMs * 0.05f;
        if (!m_showUI)
            return;

        DrawStatsOverlay();

        const SwapChain& swapChain = m_renderer->GetSwapChain();
        Device& device = m_renderer->GetDevice();

        // As tall as the window, so the sections scroll instead of running off the bottom of the screen.
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + 10.0f, viewport->WorkPos.y + 10.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(430.0f, viewport->WorkSize.y - 20.0f), ImGuiCond_FirstUseEver);
        ImGui::Begin("Settings");

        // Shader errors first: they would otherwise be easy to miss inside a closed section.
        for (const ShaderReloadable* pass : GetShaderReloadables())
        {
            const std::string& error = pass->GetLastError();
            if (error.empty())
                continue;
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
            ImGui::TextWrapped("%s", error.c_str());
            ImGui::PopStyleColor();
        }

        // --- What you tweak most: open by default ----------------------------------------------------------------

        if (m_sceneData && ImGui::CollapsingHeader("View", ImGuiTreeNodeFlags_DefaultOpen))
        {
            const bool canTrace = m_caps.raytracing;

            // Forward or deferred. Deferred can show everything computed from the G-buffer, but not views that need a
            // draw's own inputs (kDebugViewForwardOnly in shaders/shared/DebugViewList.h).
            {
                int path = static_cast<int>(m_renderSettings.path);
                ImGui::RadioButton("Forward##path", &path, static_cast<int>(RenderPath::Forward));
                ImGui::SameLine();
                ImGui::RadioButton("Deferred##path", &path, static_cast<int>(RenderPath::Deferred));
                ImGui::SameLine();
                ImGui::TextDisabled("Rendering");
                ImGui::SetItemTooltip("Forward: each draw samples its material and lights it.\n"
                                      "Deferred: the draws store the material in the G-buffer; a fullscreen pass reads it.");
                m_renderSettings.path = static_cast<RenderPath>(path);
            }
            const bool deferred = m_renderSettings.path == RenderPath::Deferred;
            if (deferred && (GetDebugViewInfo(m_debugSettings.view).flags & kDebugViewForwardOnly))
                m_debugSettings.view = SceneDebugView::Lit;

            if (ImGui::BeginCombo("Show", GetDebugViewInfo(m_debugSettings.view).label))
            {
                for (const DebugViewInfo& view : kDebugViews)
                {
                    if (view.group[0] != '\0')
                        ImGui::SeparatorText(view.group);
                    const bool unavailable = ((view.flags & kDebugViewNeedsRayTracing) && !canTrace) ||
                                             (deferred && (view.flags & kDebugViewForwardOnly));
                    ImGui::BeginDisabled(unavailable);
                    if (ImGui::Selectable(view.label, view.view == m_debugSettings.view))
                        m_debugSettings.view = view.view;
                    ImGui::EndDisabled();
                }
                ImGui::EndCombo();
            }
            if (m_debugSettings.view == SceneDebugView::ShadowDifference)
            {
                // The comparison is computed in the ray-traced shader, which also samples the shadow map (just for
                // this view: see ResolveFrameSettings).
                ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.2f, 1.0f), "red");
                ImGui::SameLine(0.0f, 4.0f);
                ImGui::TextDisabled("= only the shadow map shadows   ");
                ImGui::SameLine(0.0f, 0.0f);
                ImGui::TextColored(ImVec4(0.3f, 0.5f, 1.0f, 1.0f), "blue");
                ImGui::SameLine(0.0f, 4.0f);
                ImGui::TextDisabled("= only the ray shadows");
            }
            {
                const char* modes[] = { "Off", "Mesh tangents", "Derived tangents" };
                int mode = static_cast<int>(m_renderSettings.normalMapping);
                if (ImGui::Combo("Normal mapping", &mode, modes, IM_ARRAYSIZE(modes)))
                    m_renderSettings.normalMapping = static_cast<NormalMapping>(mode);
                ImGui::SetItemTooltip("Mesh: tangent frame from the glTF TANGENT attribute (derived where missing).\n"
                                      "Derived: always from screen-space derivatives of position and uv.");
            }
            // A checkbox to switch the override on (value >= 0) or off (negative), and a slider for its value.
            const auto overrideSlider = [](const char* label, float& value, float& lastValue) {
                bool enabled = value >= 0.0f;
                if (ImGui::Checkbox(label, &enabled))
                    value = enabled ? lastValue : -1.0f;
                ImGui::SameLine();
                ImGui::BeginDisabled(!enabled);
                ImGui::PushID(label);
                float shown = enabled ? value : lastValue;
                if (ImGui::SliderFloat("##value", &shown, 0.0f, 1.0f, "%.2f") && enabled)
                    value = shown;
                lastValue = shown;
                ImGui::PopID();
                ImGui::EndDisabled();
            };
            overrideSlider("Roughness##override", m_debugSettings.roughnessOverride, m_roughnessOverrideValue);
            overrideSlider("Metallic##override", m_debugSettings.metallicOverride, m_metallicOverrideValue);
            ImGui::SetItemTooltip("Replace every material's metallic / roughness, to see what each one does.");
            ImGui::Checkbox("Back-face culling", &m_renderSettings.backfaceCulling);
            ImGui::SameLine();
            ImGui::Checkbox("Alpha test", &m_renderSettings.alphaTest);
            {
                // Deferred always draws the pre-pass first (its G-buffer pass tests depth EQUAL).
                bool prepass = m_renderSettings.depthPrepass || deferred;
                ImGui::BeginDisabled(deferred);
                if (ImGui::Checkbox("Depth pre-pass", &prepass))
                    m_renderSettings.depthPrepass = prepass;
                ImGui::EndDisabled();
                ImGui::SetItemTooltip("Draw depth first (no pixel shading), then shade only the closest surface of each pixel\n"
                                      "(depth test EQUAL). Off: every fragment that passes the depth test when drawn is shaded.\n"
                                      "Always on for deferred rendering.");
            }
        }

        if (ImGui::CollapsingHeader("Lighting", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::SliderAngle("Sun azimuth", &m_lighting.sunAzimuth, -180.0f, 180.0f);
            ImGui::SliderAngle("Sun elevation", &m_lighting.sunElevation, -10.0f, 90.0f);
            ImGui::ColorEdit3("Sun color", &m_lighting.sunColor.x, ImGuiColorEditFlags_Float);
            ImGui::SliderFloat("Sun intensity", &m_lighting.sunIntensity, 0.0f, 50.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
            ImGui::ColorEdit3("Sky color", &m_lighting.skyColor.x, ImGuiColorEditFlags_Float);
            ImGui::ColorEdit3("Ground color", &m_lighting.groundColor.x, ImGuiColorEditFlags_Float);
            ImGui::SetItemTooltip("Ambient light arriving from below the horizon: sunlight bounced off the ground.");
            ImGui::SliderFloat("Ambient intensity", &m_lighting.ambientIntensity, 0.0f, 5.0f, "%.3f", ImGuiSliderFlags_Logarithmic);
            const DirectX::XMFLOAT3 sun = m_lighting.GetSunDirection();
            ImGui::TextDisabled("Towards the sun: (%.2f, %.2f, %.2f)", sun.x, sun.y, sun.z);
            // Lamps are easiest to see at night: no sun, a faint sky. Day puts the defaults back.
            if (ImGui::Button("Night"))
            {
                m_lighting.sunIntensity = 0.0f;
                m_lighting.ambientIntensity = 0.02f;
            }
            ImGui::SameLine();
            if (ImGui::Button("Day"))
            {
                const SceneLighting defaults;
                m_lighting.sunIntensity = defaults.sunIntensity;
                m_lighting.ambientIntensity = defaults.ambientIntensity;
            }
            if (m_sceneData && ImGui::TreeNodeEx("Point lights", ImGuiTreeNodeFlags_DefaultOpen))
            {
                ImGui::SliderInt("Count", &m_lighting.pointLightCount, 0, static_cast<int>(PointLights::kMaxLights), "%d",
                                 ImGuiSliderFlags_Logarithmic);
                ImGui::SliderFloat("Range", &m_lighting.pointLightRange, 0.01f, 50.0f, "%.2f m", ImGuiSliderFlags_Logarithmic);
                ImGui::SetItemTooltip("Each lamp's light fades smoothly to zero at this distance.");
                ImGui::SliderFloat("Intensity##point", &m_lighting.pointLightIntensity, 0.0f, 1000.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
                ImGui::SetItemTooltip("Light received 1 m from a lamp, facing it. Falls off as 1 / distance^2.");
                {
                    int technique = static_cast<int>(m_renderSettings.pointShadows.technique);
                    ImGui::RadioButton("Off##pointShadows", &technique, static_cast<int>(PointShadowTechnique::Off));
                    ImGui::SameLine();
                    ImGui::RadioButton("Cube maps##pointShadows", &technique, static_cast<int>(PointShadowTechnique::CubeMaps));
                    ImGui::SameLine();
                    ImGui::BeginDisabled(!m_caps.raytracing);
                    ImGui::RadioButton("Ray traced##pointShadows", &technique, static_cast<int>(PointShadowTechnique::RayTraced));
                    ImGui::EndDisabled();
                    ImGui::SameLine();
                    ImGui::TextDisabled("Shadows");
                    ImGui::SetItemTooltip("Cube maps: the nearest lamps (the budget below) render a cube shadow map each.\n"
                                          "Ray traced: every lamp, one shadow ray per lamp that reaches the pixel.");
                    m_renderSettings.pointShadows.technique = static_cast<PointShadowTechnique>(technique);
                }
                ImGui::BeginDisabled(m_renderSettings.pointShadows.technique != PointShadowTechnique::CubeMaps);
                ImGui::SliderInt("Shadowed lamps", &m_renderSettings.pointShadows.budget, 0,
                                 static_cast<int>(PointShadowMaps::kMaxShadowedLights));
                ImGui::EndDisabled();
                ImGui::SetItemTooltip("The lamps nearest the camera get a cube shadow map (6 x %u^2 depth faces each).",
                                      PointShadowMaps::kFaceSize);
                ImGui::TextDisabled("%zu cube maps, %zu faces, %u shadow draws (%.0f MB for %u cubes)", m_shadowedLights.size(),
                                    m_shadowedLights.size() * PointShadowMaps::kFaceCount, m_pointShadowDraws,
                                    static_cast<double>(m_pointShadowMaps->GetBytes()) / (1024.0 * 1024.0),
                                    PointShadowMaps::kMaxShadowedLights);
                if (!m_shadowedLights.empty() && ImGui::TreeNode("Cube shadow map"))
                {
                    m_pointShadowViewSlot = std::clamp(m_pointShadowViewSlot, 0, static_cast<int>(m_shadowedLights.size()) - 1);
                    ImGui::SliderInt("Cube", &m_pointShadowViewSlot, 0, static_cast<int>(m_shadowedLights.size()) - 1);
                    // The six faces as an unfolded cube: -X +Z +X -Z in a row, +Y above +Z, -Y below it.
                    // Reversed-Z perspective depth: bright = close to the lamp, dark = towards its range.
                    const float size = std::min(ImGui::GetContentRegionAvail().x / 4.0f - 4.0f, 96.0f);
                    const uint32_t slot = static_cast<uint32_t>(m_pointShadowViewSlot);
                    static constexpr const char* kFaceNames[PointShadowMaps::kFaceCount] = { "+X", "-X", "+Y", "-Y", "+Z", "-Z" };
                    const auto face = [&](uint32_t f) {
                        ImGui::Image(static_cast<ImTextureID>(m_pointShadowMaps->GetFaceDebugSrv(slot, f).ptr), ImVec2(size, size));
                        ImGui::SetItemTooltip("%s", kFaceNames[f]);
                    };
                    ImGui::Dummy(ImVec2(size, size)); ImGui::SameLine(); face(2);
                    face(1); ImGui::SameLine(); face(4); ImGui::SameLine(); face(0); ImGui::SameLine(); face(5);
                    ImGui::Dummy(ImVec2(size, size)); ImGui::SameLine(); face(3);
                    ImGui::TreePop();
                }
                ImGui::Checkbox("Animate", &m_lighting.animatePointLights);
                ImGui::SameLine();
                ImGui::Checkbox("Markers", &m_debugSettings.lightMarkers);
                ImGui::SameLine();
                {
                    int clustered = m_renderSettings.clusteredLighting ? 1 : 0;
                    ImGui::RadioButton("Brute force##lights", &clustered, 0);
                    ImGui::SetItemTooltip("Every pixel visits every lamp.");
                    ImGui::SameLine();
                    ImGui::RadioButton("Clustered##lights", &clustered, 1);
                    ImGui::SetItemTooltip("A compute pass lists the lamps reaching each of 16x9x24 view-space clusters;\n"
                                          "every pixel visits only its cluster's list.");
                    m_renderSettings.clusteredLighting = clustered != 0;
                }
                const uint32_t used = m_clusterGrid->GetUsedIndices();
                ImGui::Text("Clusters: %ux%ux%u, %u light entries (%.1f per cluster, %.0f%% of the list)",
                            kClusterCountX, kClusterCountY, kClusterCountZ, used,
                            static_cast<double>(used) / kClusterCount,
                            100.0 * used / ClusterGrid::kIndexCapacity);
                ImGui::TreePop();
            }
            {
                int shading = static_cast<int>(m_renderSettings.shading);
                ImGui::RadioButton("Lambert##shading", &shading, static_cast<int>(ShadingModel::Lambert));
                ImGui::SameLine();
                ImGui::RadioButton("PBR (GGX)##shading", &shading, static_cast<int>(ShadingModel::Pbr));
                ImGui::SameLine();
                ImGui::TextDisabled("Shading");
                ImGui::SetItemTooltip("Lambert: matte only (baseColor / pi).\n"
                                      "PBR: Lambert diffuse + Cook-Torrance GGX specular, driven by the metallic and roughness maps.");
                m_renderSettings.shading = static_cast<ShadingModel>(shading);
            }
            ImGui::Checkbox("Sun shadows", &m_renderSettings.sunShadows.enabled);
            {
                // Ray-traced shadows need the TLAS, which exists only with inline ray tracing support.
                const bool canTrace = m_caps.raytracing;
                ImGui::BeginDisabled(!canTrace);
                int technique = m_renderSettings.sunShadows.raytraced && canTrace ? 1 : 0;
                // "##technique" makes the ID unique: the "Shadow map" tree node below has the same visible label,
                // and ImGui derives IDs from labels (text after ## is part of the ID but not displayed).
                bool clicked = ImGui::RadioButton("Shadow map##technique", &technique, 0);
                ImGui::SameLine();
                clicked |= ImGui::RadioButton("Ray traced##technique", &technique, 1);
                if (clicked)
                    m_renderSettings.sunShadows.raytraced = technique == 1;
                ImGui::EndDisabled();
                if (!canTrace)
                    ImGui::SetItemTooltip("Needs DirectX Raytracing tier 1.1 (inline ray tracing).");
            }
            {
                const int r = m_renderSettings.sunShadows.filterRadius;
                const std::string label = r < 0  ? std::string("hard (1 texel)")
                                        : r == 0 ? std::string("2x2 hardware filter")
                                                 : std::format("{}x{} taps (each 2x2)", 2 * r + 1, 2 * r + 1);
                ImGui::SliderInt("Shadow softness", &m_renderSettings.sunShadows.filterRadius, -1, 4, label.c_str());
                ImGui::SetItemTooltip("PCF kernel radius. Each step widens the soft edge by about two shadow-map texels\n"
                                      "and costs more samples: (2r+1)^2 per pixel.");
            }
            if (ImGui::TreeNode("Shadow bias"))
            {
                // All three in shadow-map texels (1.8 cm each for Sponza). Zero shows the acne; large values show
                // shadows detaching from their casters (peter-panning).
                ImGui::SliderFloat("Constant", &m_renderSettings.sunShadows.constantBias, 0.0f, 20.0f, "%.2f texels");
                ImGui::SetItemTooltip("Pushes every comparison towards the sun by this distance.");
                ImGui::SliderFloat("Slope-scaled", &m_renderSettings.sunShadows.slopeBias, 0.0f, 20.0f, "%.2f texels");
                ImGui::SetItemTooltip("Extra push that grows with how tilted the surface is relative to the sun\n"
                                      "(times tan of the angle): tilted surfaces need more.");
                ImGui::SliderFloat("Normal offset", &m_renderSettings.sunShadows.normalOffset, 0.0f, 20.0f, "%.2f texels");
                ImGui::SetItemTooltip("Looks the shadow map up this far out along the surface normal.");
                if (ImGui::Button("Defaults"))
                {
                    const SunShadowSettings defaults;
                    m_renderSettings.sunShadows.constantBias = defaults.constantBias;
                    m_renderSettings.sunShadows.slopeBias = defaults.slopeBias;
                    m_renderSettings.sunShadows.normalOffset = defaults.normalOffset;
                }
                ImGui::SameLine();
                if (ImGui::Button("None (show acne)"))
                    m_renderSettings.sunShadows.constantBias = m_renderSettings.sunShadows.slopeBias = m_renderSettings.sunShadows.normalOffset = 0.0f;
                ImGui::TreePop();
            }

            if (ImGui::TreeNode("Shadow map"))
            {
                // The depth the sun sees: brighter = closer to the sun (reversed-Z).
                const float extent = GetSunShadowExtent(m_scene.bounds);
                const uint32_t size = m_shadowMap->GetSize();
                ImGui::Text("%u x %u, D32. Covers %.1f m: %.1f cm per texel", size, size, extent,
                            100.0f * extent / static_cast<float>(size));
                const float width = std::min(ImGui::GetContentRegionAvail().x, 400.0f);
                ImGui::Image(static_cast<ImTextureID>(m_shadowMap->GetDebugSrvGpuHandle().ptr), ImVec2(width, width));
                ImGui::TreePop();
            }
        }

        if (ImGui::CollapsingHeader("Tonemapping", ImGuiTreeNodeFlags_DefaultOpen))
        {
            const char* operators[] = { "Clamp (none)", "Reinhard", "ACES (fitted)" };
            int op = static_cast<int>(m_tonemapSettings.op);
            if (ImGui::Combo("Curve", &op, operators, IM_ARRAYSIZE(operators)))
                m_tonemapSettings.op = static_cast<TonemapOperator>(op);
            ImGui::SliderFloat("Exposure", &m_tonemapSettings.exposureStops, -6.0f, 6.0f, "%+.1f stops");
            ImGui::TextDisabled("Output: back buffer _SRGB view (hardware linear -> sRGB)");
        }

        // --- Details: closed by default ---------------------------------------------------------------------------

        if (ImGui::CollapsingHeader("Camera"))
        {
            ImGui::DragFloat3("Position", &m_camera.position.x, 0.01f);
            ImGui::SliderAngle("Yaw", &m_camera.yaw, -180.0f, 180.0f);
            ImGui::SliderAngle("Pitch", &m_camera.pitch, -89.0f, 89.0f);
            ImGui::SliderAngle("Vertical FOV", &m_camera.verticalFov, 10.0f, 120.0f);
            ImGui::SliderFloat("Near plane", &m_camera.nearZ, 0.01f, 2.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
            ImGui::SliderFloat("Move speed", &m_cameraController.moveSpeed, 0.05f, 500.0f, "%.2f m/s", ImGuiSliderFlags_Logarithmic);
            if (ImGui::Button("Reset camera"))
                m_camera = m_homeCamera;
        }

        if (ImGui::CollapsingHeader("Scene"))
        {
            const std::string fileName = ToUtf8(m_scenePath.filename().wstring());
            if (m_sceneSummary)
            {
                const GltfSummary& s = *m_sceneSummary;
                ImGui::Text("%s (parsed in %.1f ms, built in %.1f ms)", fileName.c_str(), m_sceneLoadMs, m_sceneBuildMs);
                ImGui::SetItemTooltip("%s", ToUtf8(m_scenePath.wstring()).c_str());
                ImGui::Text("%zu nodes, %zu meshes, %zu images", s.nodes, s.meshes, s.images);
                ImGui::Text("%zu materials (%zu alpha-mask, %zu alpha-blend, %zu double-sided)", m_scene.materials.size(),
                            static_cast<size_t>(std::ranges::count(m_scene.materials, AlphaMode::Mask, &SceneMaterial::alphaMode)),
                            static_cast<size_t>(std::ranges::count(m_scene.materials, AlphaMode::Blend, &SceneMaterial::alphaMode)),
                            static_cast<size_t>(std::ranges::count(m_scene.materials, true, &SceneMaterial::doubleSided)));
                const auto withMap = [&](uint32_t SceneMaterial::*map) {
                    return std::ranges::count_if(m_scene.materials, [&](const SceneMaterial& m) { return m.*map != kNoTexture; });
                };
                ImGui::Text("  maps: base color %zu, metal-rough %zu, normal %zu, occlusion %zu, emissive %zu",
                            static_cast<size_t>(withMap(&SceneMaterial::baseColorTexture)),
                            static_cast<size_t>(withMap(&SceneMaterial::metallicRoughnessTexture)),
                            static_cast<size_t>(withMap(&SceneMaterial::normalTexture)),
                            static_cast<size_t>(withMap(&SceneMaterial::occlusionTexture)),
                            static_cast<size_t>(withMap(&SceneMaterial::emissiveTexture)));
                ImGui::Text("%zu instances of %zu primitives", m_scene.instances.size(), m_scene.primitives.size());
                ImGui::Text("%zu vertices, %zu triangles", m_scene.vertices.size(), m_scene.indices.size() / 3);
                const DirectX::XMFLOAT3& lo = m_scene.bounds.min;
                const DirectX::XMFLOAT3& hi = m_scene.bounds.max;
                ImGui::Text("Size %.1f x %.1f x %.1f", hi.x - lo.x, hi.y - lo.y, hi.z - lo.z);
                ImGui::SetItemTooltip("World bounds (%.2f, %.2f, %.2f) to (%.2f, %.2f, %.2f)", lo.x, lo.y, lo.z, hi.x, hi.y, hi.z);
                if (m_sceneData)
                {
                    ImGui::Text("GPU: %.1f MB of buffers, %u textures (%.1f MB with mips)",
                                static_cast<double>(m_sceneData->GetBufferBytes()) / (1024.0 * 1024.0), m_sceneData->GetTextureCount(),
                                static_cast<double>(m_sceneData->GetTextureBytes()) / (1024.0 * 1024.0));
                }
                if (m_accelerationStructures)
                {
                    const AccelerationStructures::Stats& rt = m_accelerationStructures->GetStats();
                    constexpr double kMB = 1024.0 * 1024.0;
                    ImGui::Text("Ray tracing: %u BLASes %.1f MB (compactable to %.1f MB)", rt.blasCount,
                                static_cast<double>(rt.blasBytes) / kMB, static_cast<double>(rt.blasCompactedBytes) / kMB);
                    ImGui::Text("     TLAS: %u instances, %.1f KB; built in %.1f ms", rt.instanceCount,
                                static_cast<double>(rt.tlasBytes) / 1024.0, rt.buildMs);
                }
                else
                    ImGui::TextDisabled("Ray tracing: not available on this GPU");
            }
            else
            {
                ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "%s %s", fileName.c_str(), m_sceneError.c_str());
                ImGui::SetItemTooltip("%s", ToUtf8(m_scenePath.wstring()).c_str());
            }
        }

        if (ImGui::CollapsingHeader("Renderer"))
        {
            ImGui::Text("Adapter: %s", ToUtf8(device.GetAdapterName()).c_str());
            ImGui::Text("Shader Model %s (compiling for %ls), Resource Binding Tier %d",
                        ToString(device.GetShaderModel()).c_str(), kShaderModelSuffix,
                        static_cast<int>(device.GetResourceBindingTier()));
            ImGui::Text("Debug layer: %s, frame %llu", device.IsDebugLayerEnabled() ? "on" : "off",
                        static_cast<unsigned long long>(m_renderer->GetFrameCount()));
            bool vsync = m_renderer->GetVsync();
            if (ImGui::Checkbox("VSync", &vsync))
                m_renderer->SetVsync(vsync);
            ImGui::SameLine();
            ImGui::TextDisabled(swapChain.IsTearingSupported() ? "(tearing supported)" : "(tearing unsupported)");
            ImGui::Text("G-buffer: %u targets, %.1f MB at %ux%u (used by deferred)", static_cast<uint32_t>(GBuffer::kCount),
                        static_cast<double>(m_renderer->GetGBuffer().GetBytes()) / (1024.0 * 1024.0),
                        swapChain.GetWidth(), swapChain.GetHeight());
            // Float + HDR: scene colors are linear light and may go above 1.0.
            ImGui::ColorEdit3("Clear color (linear)", m_debugSettings.clearColor, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);

            // Read before this frame's allocations, so this shows what the previous frame used.
            const FrameAllocator& frameAllocator = m_renderer->GetFrameAllocator();
            ImGui::Text("Frame upload memory: %llu / %llu bytes",
                        static_cast<unsigned long long>(frameAllocator.GetUsedBytes()),
                        static_cast<unsigned long long>(frameAllocator.GetBytesPerFrame()));
            const DescriptorHeap& srvHeap = m_renderer->GetSrvHeap();
            ImGui::Text("Bindless descriptors: %u / %u used", srvHeap.GetAllocatedCount(), srvHeap.GetCapacity());
        }

        if (ImGui::CollapsingHeader("Shaders"))
        {
            if (ImGui::Button("Reload shaders (F5)"))
                ReloadShaders();
            ImGui::SameLine();
            ImGui::TextDisabled("from %s", ToUtf8(m_shaderDir.wstring()).c_str());
        }

        if (ImGui::CollapsingHeader("Input"))
        {
            ImGui::Text("Mouse: (%d, %d)  raw delta: (%d, %d)", m_input.GetMouseX(), m_input.GetMouseY(),
                        m_input.GetMouseDeltaXRaw(), m_input.GetMouseDeltaYRaw());
            ImGui::Text("Buttons: %s %s %s %s %s",
                        m_input.IsMouseDownRaw(MouseButton::Left) ? "[L]" : " L ",
                        m_input.IsMouseDownRaw(MouseButton::Middle) ? "[M]" : " M ",
                        m_input.IsMouseDownRaw(MouseButton::Right) ? "[R]" : " R ",
                        m_input.IsMouseDownRaw(MouseButton::X1) ? "[X1]" : " X1 ",
                        m_input.IsMouseDownRaw(MouseButton::X2) ? "[X2]" : " X2 ");
            ImGui::Text("Captured by UI: keyboard %s, mouse %s",
                        m_input.IsKeyboardCapturedByUI() ? "yes" : "no",
                        m_input.IsMouseCapturedByUI() ? "yes" : "no");

            std::string held;
            for (int vk = 0; vk < Input::kKeyCount; ++vk)
            {
                if (m_input.IsKeyDownRaw(vk))
                {
                    if (!held.empty())
                        held += ", ";
                    held += GetKeyName(vk);
                }
            }
            ImGui::TextWrapped("Keys held: %s", held.empty() ? "-" : held.c_str());

            ImGui::Separator();
            ImGui::TextDisabled("Hold right mouse: look   WASD: move   Q/E: down/up");
            ImGui::TextDisabled("Shift: fast   wheel: speed   F1: hide UI   F5: reload shaders   Esc: quit");
        }

        DrawPixUI();

        ImGui::Checkbox("Show ImGui demo window", &m_showDemoWindow);
        ImGui::End();

        if (m_showDemoWindow)
            ImGui::ShowDemoWindow(&m_showDemoWindow);
    }
}
