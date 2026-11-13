#include "ui/SettingsPanel.h"

#include "core/Input.h"
#include "core/StringUtils.h"
#include "gfx/DescriptorHeap.h"
#include "gfx/Device.h"
#include "gfx/FrameAllocator.h"
#include "gfx/Pix.h"
#include "gfx/PipelineBuilder.h"
#include "gfx/ShaderCompiler.h"
#include "gfx/SwapChain.h"
#include "renderer/AccelerationStructures.h"
#include "renderer/ClusterGrid.h"
#include "renderer/GBuffer.h"
#include "renderer/PointShadowMaps.h"
#include "renderer/Renderer.h"
#include "renderer/SceneGpuData.h"
#include "renderer/SceneRenderer.h"
#include "renderer/ShadowMap.h"
#include "renderer/TonemapPass.h"
#include "scene/Camera.h"
#include "scene/FlyCameraController.h"
#include "scene/Lighting.h"
#include "scene/PointLights.h"
#include "scene/Scene.h"
#include "ui/Widgets.h"

#include <imgui.h>

#include <shellapi.h>

#include <algorithm>
#include <format>
#include <string>

namespace walnut
{
    using ui::DebugViewCombo;
    using ui::EnumCombo;
    using ui::EnumRadio;
    using ui::OverrideSlider;

    namespace
    {
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

    SettingsPanel::SettingsPanel(const Context& context)
        : m_renderer(context.renderer), m_sceneRenderer(context.sceneRenderer), m_input(context.input),
          m_renderSettings(context.renderSettings), m_debugSettings(context.debugSettings),
          m_tonemapSettings(context.tonemapSettings), m_lighting(context.lighting), m_camera(context.camera),
          m_homeCamera(context.homeCamera), m_cameraController(context.cameraController), m_scene(context.scene),
          m_sceneFile(context.sceneFile), m_shaderDir(context.shaderDir), m_lastCapturePath(context.lastCapturePath),
          m_reloadShaders(context.reloadShaders), m_requestGpuCapture(context.requestGpuCapture)
    {
    }

    void SettingsPanel::Draw(float deltaSeconds, bool visible)
    {
        const float frameMs = deltaSeconds * 1000.0f;
        m_smoothedFrameMs = (m_smoothedFrameMs == 0.0f) ? frameMs : m_smoothedFrameMs * 0.95f + frameMs * 0.05f;
        if (!visible)
            return;

        DrawStatsOverlay();

        // As tall as the window, so the sections scroll instead of running off the bottom of the screen.
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + 10.0f, viewport->WorkPos.y + 10.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(430.0f, viewport->WorkSize.y - 20.0f), ImGuiCond_FirstUseEver);
        ImGui::Begin("Settings");

        DrawShaderErrors();

        // --- What you tweak most: open by default ----------------------------------------------------------------
        DrawView();
        DrawLighting();
        DrawTonemapping();

        // --- Details: closed by default ---------------------------------------------------------------------------
        DrawCamera();
        DrawSceneInfo();
        DrawRendererInfo();
        DrawShaders();
        DrawInput();
        DrawPix();

        ImGui::Checkbox("Show ImGui demo window", &m_showDemoWindow);
        ImGui::End();

        if (m_showDemoWindow)
            ImGui::ShowDemoWindow(&m_showDemoWindow);
    }

    void SettingsPanel::DrawShaderErrors()
    {
        // Shader errors first: they would otherwise be easy to miss inside a closed section.
        for (const ShaderReloadable* pass : m_sceneRenderer.GetShaderReloadables())
        {
            const std::string& error = pass->GetLastError();
            if (error.empty())
                continue;
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
            ImGui::TextWrapped("%s", error.c_str());
            ImGui::PopStyleColor();
        }
    }

    void SettingsPanel::DrawView()
    {
        if (m_sceneRenderer.HasScene() && ImGui::CollapsingHeader("View", ImGuiTreeNodeFlags_DefaultOpen))
        {
            const bool canTrace = m_sceneRenderer.GetCaps().raytracing;

            // Forward or deferred. Deferred can show everything computed from the G-buffer, but not views that need a
            // draw's own inputs (kDebugViewForwardOnly in shaders/shared/DebugViewList.h).
            EnumRadio("Rendering", m_renderSettings.path,
                      { { "Forward##path", RenderPath::Forward }, { "Deferred##path", RenderPath::Deferred } },
                      "Forward: each draw samples its material and lights it.\n"
                      "Deferred: the draws store the material in the G-buffer; a fullscreen pass reads it.");
            const bool deferred = m_renderSettings.path == RenderPath::Deferred;
            if (deferred && (GetDebugViewInfo(m_debugSettings.view).flags & kDebugViewForwardOnly))
                m_debugSettings.view = SceneDebugView::Lit;

            DebugViewCombo("Show", m_debugSettings.view, deferred, canTrace);
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
            EnumCombo("Normal mapping", m_renderSettings.normalMapping, { "Off", "Mesh tangents", "Derived tangents" });
            ImGui::SetItemTooltip("Mesh: tangent frame from the glTF TANGENT attribute (derived where missing).\n"
                                  "Derived: always from screen-space derivatives of position and uv.");
            OverrideSlider("Roughness##override", m_debugSettings.roughnessOverride, m_roughnessOverrideValue);
            OverrideSlider("Metallic##override", m_debugSettings.metallicOverride, m_metallicOverrideValue);
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
    }

    void SettingsPanel::DrawLighting()
    {
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
            DrawPointLights();
            EnumRadio("Shading", m_renderSettings.shading,
                      { { "Lambert##shading", ShadingModel::Lambert }, { "PBR (GGX)##shading", ShadingModel::Pbr } },
                      "Lambert: matte only (baseColor / pi).\n"
                      "PBR: Lambert diffuse + Cook-Torrance GGX specular, driven by the metallic and roughness maps.");
            DrawSunShadows();
        }
    }

    void SettingsPanel::DrawPointLights()
    {
        if (m_sceneRenderer.HasScene() && ImGui::TreeNodeEx("Point lights", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::SliderInt("Count", &m_lighting.pointLightCount, 0, static_cast<int>(PointLights::kMaxLights), "%d",
                             ImGuiSliderFlags_Logarithmic);
            ImGui::SliderFloat("Range", &m_lighting.pointLightRange, 0.01f, 50.0f, "%.2f m", ImGuiSliderFlags_Logarithmic);
            ImGui::SetItemTooltip("Each lamp's light fades smoothly to zero at this distance.");
            ImGui::SliderFloat("Intensity##point", &m_lighting.pointLightIntensity, 0.0f, 1000.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
            ImGui::SetItemTooltip("Light received 1 m from a lamp, facing it. Falls off as 1 / distance^2.");
            EnumRadio("Shadows", m_renderSettings.pointShadows.technique,
                      { { "Off##pointShadows", PointShadowTechnique::Off },
                        { "Cube maps##pointShadows", PointShadowTechnique::CubeMaps },
                        { "Ray traced##pointShadows", PointShadowTechnique::RayTraced, m_sceneRenderer.GetCaps().raytracing } },
                      "Cube maps: the nearest lamps (the budget below) render a cube shadow map each.\n"
                      "Ray traced: every lamp, one shadow ray per lamp that reaches the pixel.");
            ImGui::BeginDisabled(m_renderSettings.pointShadows.technique != PointShadowTechnique::CubeMaps);
            ImGui::SliderInt("Shadowed lamps", &m_renderSettings.pointShadows.budget, 0,
                             static_cast<int>(PointShadowMaps::kMaxShadowedLights));
            ImGui::EndDisabled();
            ImGui::SetItemTooltip("The lamps nearest the camera get a cube shadow map (6 x %u^2 depth faces each).",
                                  PointShadowMaps::kFaceSize);
            ImGui::TextDisabled("%zu cube maps, %zu faces, %u shadow draws (%.0f MB for %u cubes)", m_sceneRenderer.GetShadowedLights().size(),
                                m_sceneRenderer.GetShadowedLights().size() * PointShadowMaps::kFaceCount, m_sceneRenderer.GetPointShadowDraws(),
                                static_cast<double>(m_sceneRenderer.GetPointShadowMaps().GetBytes()) / (1024.0 * 1024.0),
                                PointShadowMaps::kMaxShadowedLights);
            // The UI runs before this frame's cubes are chosen, and every cube is discarded when a frame starts: only
            // show the slots that this frame will draw again (the last frame's count, cut to this frame's budget).
            const FrameSettings frame = ResolveFrameSettings(m_renderSettings, m_debugSettings, m_sceneRenderer.GetCaps());
            const int cubes = static_cast<int>(std::min({ m_sceneRenderer.GetShadowedLights().size(), size_t{ frame.cubeShadowBudget },
                                                           static_cast<size_t>(std::max(m_lighting.pointLightCount, 0)) }));
            if (cubes > 0 && ImGui::TreeNode("Cube shadow map"))
            {
                m_pointShadowViewSlot = std::clamp(m_pointShadowViewSlot, 0, cubes - 1);
                ImGui::SliderInt("Cube", &m_pointShadowViewSlot, 0, cubes - 1);
                // The six faces as an unfolded cube: -X +Z +X -Z in a row, +Y above +Z, -Y below it.
                // Reversed-Z perspective depth: bright = close to the lamp, dark = towards its range.
                const float size = std::min(ImGui::GetContentRegionAvail().x / 4.0f - 4.0f, 96.0f);
                const uint32_t slot = static_cast<uint32_t>(m_pointShadowViewSlot);
                static constexpr const char* kFaceNames[PointShadowMaps::kFaceCount] = { "+X", "-X", "+Y", "-Y", "+Z", "-Z" };
                const auto face = [&](uint32_t f) {
                    ImGui::Image(static_cast<ImTextureID>(m_sceneRenderer.GetPointShadowMaps().GetFaceDebugSrv(slot, f).ptr), ImVec2(size, size));
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
            EnumRadio<bool>(nullptr, m_renderSettings.clusteredLighting,
                            { { "Brute force##lights", false, true, "Every pixel visits every lamp." },
                              { "Clustered##lights", true, true,
                                "A compute pass lists the lamps reaching each of 16x9x24 view-space clusters;\n"
                                "every pixel visits only its cluster's list." } });
            // The count is read back from the GPU, so it's only current while the grid is being built.
            if (frame.buildClusters)
            {
                const uint32_t used = m_sceneRenderer.GetClusterGrid().GetUsedIndices();
                ImGui::Text("Clusters: %ux%ux%u, %u light entries (%.1f per cluster, %.0f%% of the list)",
                            kClusterCountX, kClusterCountY, kClusterCountZ, used,
                            static_cast<double>(used) / kClusterCount,
                            100.0 * used / ClusterGrid::kIndexCapacity);
            }
            else
                ImGui::TextDisabled("Clusters: %ux%ux%u, not built (brute force)", kClusterCountX, kClusterCountY, kClusterCountZ);
            ImGui::TreePop();
        }
    }

    void SettingsPanel::DrawSunShadows()
    {
        ImGui::Checkbox("Sun shadows", &m_renderSettings.sunShadows.enabled);
        {
            // Ray-traced shadows need the TLAS, which exists only with inline ray tracing support.
            const bool canTrace = m_sceneRenderer.GetCaps().raytracing;
            // "##technique" makes the ID unique: the "Shadow map" tree node below has the same visible label,
            // and ImGui derives IDs from labels (text after ## is part of the ID but not displayed).
            bool raytraced = m_renderSettings.sunShadows.raytraced && canTrace;
            if (EnumRadio<bool>(nullptr, raytraced,
                                { { "Shadow map##technique", false, canTrace },
                                  { "Ray traced##technique", true, canTrace,
                                    canTrace ? nullptr : "Needs DirectX Raytracing tier 1.1 (inline ray tracing)." } }))
                m_renderSettings.sunShadows.raytraced = raytraced;
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
            const uint32_t size = m_sceneRenderer.GetShadowMap().GetSize();
            ImGui::Text("%u x %u, D32. Covers %.1f m: %.1f cm per texel", size, size, extent,
                        100.0f * extent / static_cast<float>(size));
            const float width = std::min(ImGui::GetContentRegionAvail().x, 400.0f);
            ImGui::Image(static_cast<ImTextureID>(m_sceneRenderer.GetShadowMap().GetDebugSrvGpuHandle().ptr), ImVec2(width, width));
            ImGui::TreePop();
        }
    }

    void SettingsPanel::DrawTonemapping()
    {
        if (ImGui::CollapsingHeader("Tonemapping", ImGuiTreeNodeFlags_DefaultOpen))
        {
            EnumCombo("Curve", m_tonemapSettings.op, { "Clamp (none)", "Reinhard", "ACES (fitted)" });
            ImGui::SliderFloat("Exposure", &m_tonemapSettings.exposureStops, -6.0f, 6.0f, "%+.1f stops");
            ImGui::TextDisabled("Output: back buffer _SRGB view (hardware linear -> sRGB)");
        }
    }

    void SettingsPanel::DrawCamera()
    {
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
    }

    void SettingsPanel::DrawSceneInfo()
    {
        if (ImGui::CollapsingHeader("Scene"))
        {
            const std::string fileName = ToUtf8(m_sceneFile.path.filename().wstring());
            if (m_sceneFile.summary)
            {
                const GltfSummary& s = *m_sceneFile.summary;
                ImGui::Text("%s (parsed in %.1f ms, built in %.1f ms)", fileName.c_str(), m_sceneFile.loadMs, m_sceneFile.buildMs);
                ImGui::SetItemTooltip("%s", ToUtf8(m_sceneFile.path.wstring()).c_str());
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
                if (m_sceneRenderer.HasScene())
                {
                    ImGui::Text("GPU: %.1f MB of buffers, %u textures (%.1f MB with mips)",
                                static_cast<double>(m_sceneRenderer.GetSceneData()->GetBufferBytes()) / (1024.0 * 1024.0), m_sceneRenderer.GetSceneData()->GetTextureCount(),
                                static_cast<double>(m_sceneRenderer.GetSceneData()->GetTextureBytes()) / (1024.0 * 1024.0));
                }
                if (const AccelerationStructures* accelerationStructures = m_sceneRenderer.GetAccelerationStructures())
                {
                    const AccelerationStructures::Stats& rt = accelerationStructures->GetStats();
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
                ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "%s %s", fileName.c_str(), m_sceneFile.error.c_str());
                ImGui::SetItemTooltip("%s", ToUtf8(m_sceneFile.path.wstring()).c_str());
            }
        }
    }

    void SettingsPanel::DrawRendererInfo()
    {
        const SwapChain& swapChain = m_renderer.GetSwapChain();
        Device& device = m_renderer.GetDevice();
        if (ImGui::CollapsingHeader("Renderer"))
        {
            ImGui::Text("Adapter: %s", ToUtf8(device.GetAdapterName()).c_str());
            ImGui::Text("Shader Model %s (compiling for %ls), Resource Binding Tier %d",
                        ToString(device.GetShaderModel()).c_str(), kShaderModelSuffix,
                        static_cast<int>(device.GetResourceBindingTier()));
            ImGui::Text("Debug layer: %s, frame %llu", device.IsDebugLayerEnabled() ? "on" : "off",
                        static_cast<unsigned long long>(m_renderer.GetFrameCount()));
            bool vsync = m_renderer.GetVsync();
            if (ImGui::Checkbox("VSync", &vsync))
                m_renderer.SetVsync(vsync);
            ImGui::SameLine();
            ImGui::TextDisabled(swapChain.IsTearingSupported() ? "(tearing supported)" : "(tearing unsupported)");
            ImGui::Text("G-buffer: %u targets, %.1f MB at %ux%u (used by deferred)", static_cast<uint32_t>(GBuffer::kCount),
                        static_cast<double>(m_renderer.GetGBuffer().GetBytes()) / (1024.0 * 1024.0),
                        swapChain.GetWidth(), swapChain.GetHeight());
            // Float + HDR: scene colors are linear light and may go above 1.0.
            ImGui::ColorEdit3("Clear color (linear)", m_debugSettings.clearColor, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);

            // Read before this frame's allocations, so this shows what the previous frame used.
            const FrameAllocator& frameAllocator = m_renderer.GetFrameAllocator();
            ImGui::Text("Frame upload memory: %llu / %llu bytes",
                        static_cast<unsigned long long>(frameAllocator.GetUsedBytes()),
                        static_cast<unsigned long long>(frameAllocator.GetBytesPerFrame()));
            const DescriptorHeap& srvHeap = m_renderer.GetSrvHeap();
            ImGui::Text("Bindless descriptors: %u / %u used", srvHeap.GetAllocatedCount(), srvHeap.GetCapacity());
        }
    }

    void SettingsPanel::DrawShaders()
    {
        if (ImGui::CollapsingHeader("Shaders"))
        {
            if (ImGui::Button("Reload shaders (F5)"))
                m_reloadShaders();
            ImGui::SameLine();
            ImGui::TextDisabled("from %s", ToUtf8(m_shaderDir.wstring()).c_str());
        }
    }

    void SettingsPanel::DrawInput()
    {
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
    }

    void SettingsPanel::DrawPix()
    {
        if (!ImGui::CollapsingHeader("PIX"))
            return;

        const bool capturerLoaded = pix::IsGpuCapturerLoaded();
        ImGui::Text("GPU capturer: %s", capturerLoaded ? "loaded" : "not loaded (run with --pix)");
        ImGui::Text("Attached for GPU capture: %s", PIXIsAttachedForGpuCapture() ? "yes" : "no");

        ImGui::BeginDisabled(!capturerLoaded);
        if (ImGui::Button("Capture next frame"))
            m_requestGpuCapture();
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

    void SettingsPanel::DrawStatsOverlay() const
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
            const SwapChain& swapChain = m_renderer.GetSwapChain();
            ImGui::Text("%.2f ms  (%.0f FPS)", m_smoothedFrameMs, m_smoothedFrameMs > 0.0f ? 1000.0f / m_smoothedFrameMs : 0.0f);
            ImGui::Text("%u x %u%s", swapChain.GetWidth(), swapChain.GetHeight(), m_renderer.GetVsync() ? ", vsync" : "");
            if (m_sceneRenderer.HasScene())
                ImGui::Text("%u draws, %.2f M triangles", m_sceneRenderer.GetSceneData()->GetDrawCount(),
                            static_cast<double>(m_scene.indices.size() / 3) / 1e6);
            ImGui::Text("Camera (%.1f, %.1f, %.1f)", m_camera.position.x, m_camera.position.y, m_camera.position.z);
            ImGui::TextDisabled("F1: hide UI");
        }
        ImGui::End();
    }
}
