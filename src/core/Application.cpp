#include "core/Application.h"

#include "core/Log.h"
#include "core/StringUtils.h"
#include "core/Window.h"
#include "gfx/Pix.h"
#include "gfx/ShaderCompiler.h"
#include "gfx/PipelineBuilder.h"
#include "renderer/PointShadowMaps.h"
#include "renderer/Renderer.h"
#include "ui/ImGuiLayer.h"

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
    }

    Application::Application(const AppOptions& options)
        : m_options(options), m_exeDir(GetExecutableDirectory())
    {
        m_liveObjectReport.enabled = options.debugLayer || options.gpuValidation;
        // The GPU capturer hooks D3D12 creation, so it has to be in the process before the device exists.
        if (options.pixGpuCapturer)
            pix::LoadGpuCapturer();

        // Converted into the engine's Scene and kept on the CPU; the GPU doesn't see it yet. The glTF document is
        // freed right after, since everything needed was copied out of it.
        m_sceneFile.path = ResolveAssetPath(options.scene);
        if (!std::filesystem::is_regular_file(m_sceneFile.path))
        {
            m_sceneFile.error = "not found: run tools\\fetch-assets.ps1";
            log::Warn("Scene not found: {} (run tools\\fetch-assets.ps1 to download Sponza)", ToUtf8(m_sceneFile.path.wstring()));
        }
        else
        {
            const auto loadStart = std::chrono::steady_clock::now();
            const std::unique_ptr<GltfDocument> document = GltfDocument::Load(m_sceneFile.path, m_sceneFile.error);
            m_sceneFile.loadMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - loadStart).count();
            if (document)
            {
                m_sceneFile.summary = SummarizeGltf(document->GetData());
                log::Info("Scene: {} (parsed in {:.1f} ms)", ToUtf8(m_sceneFile.path.wstring()), m_sceneFile.loadMs);
                LogGltfSummary(*m_sceneFile.summary);

                const auto buildStart = std::chrono::steady_clock::now();
                m_scene = BuildSceneFromGltf(document->GetData(), m_sceneFile.path.parent_path());
                m_sceneFile.buildMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - buildStart).count();
                log::Info("Scene built in {:.1f} ms:", m_sceneFile.buildMs);
                LogSceneStats(m_scene);
            }
            else
                log::Error("Scene {} failed to load: {}", ToUtf8(m_sceneFile.path.wstring()), m_sceneFile.error);
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
        m_sceneRenderer = std::make_unique<SceneRenderer>(*m_renderer, *m_shaderCompiler, m_shaderDir, m_scene);
        if (m_sceneRenderer->HasScene())
        {
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
        m_imgui = std::make_unique<ImGuiLayer>(*m_window, *m_renderer);
        m_settingsPanel = std::make_unique<SettingsPanel>(SettingsPanel::Context{
            .renderer = *m_renderer,
            .sceneRenderer = *m_sceneRenderer,
            .input = m_input,
            .renderSettings = m_renderSettings,
            .debugSettings = m_debugSettings,
            .tonemapSettings = m_tonemapSettings,
            .lighting = m_lighting,
            .camera = m_camera,
            .homeCamera = m_homeCamera,
            .cameraController = m_cameraController,
            .scene = m_scene,
            .sceneFile = m_sceneFile,
            .shaderDir = m_shaderDir,
            .lastCapturePath = m_lastCapturePath,
            .reloadShaders = [this] { ReloadShaders(); },
            .requestGpuCapture = [this] { RequestGpuCapture(); },
        });

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
        // Members are destroyed in reverse order of declaration (Application.h): the UI and the scene renderer before
        // the renderer, the renderer before the window, and the live-object report last of all.
        if (m_renderer)
            m_renderer->WaitForGpu();
    }

    Application::LiveObjectReport::~LiveObjectReport()
    {
        if (enabled)
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
                m_settingsPanel->Draw(deltaSeconds, m_showUI);
            }

            // This frame's decisions, from the settings the UI just changed.
            const FrameSettings frame = ResolveFrameSettings(m_renderSettings, m_debugSettings, m_sceneRenderer->GetCaps());

            {
                PIXScopedEvent(pix::kColorCpu, L"Render");
                m_renderer->BeginFrame(m_debugSettings.clearColor);
                // This frame's point lights (animated), then the 3D frame up to the tonemapped back buffer.
                if (m_sceneRenderer->HasScene())
                    m_pointLights.Update(GetAnimationTime(), static_cast<uint32_t>(m_lighting.pointLightCount),
                                         m_lighting.pointLightRange, m_lighting.pointLightIntensity,
                                         m_lighting.animatePointLights, m_pointLightData);
                m_sceneRenderer->Record({ .camera = m_camera, .lighting = m_lighting, .settings = m_renderSettings,
                                          .debug = m_debugSettings, .frame = frame, .tonemap = m_tonemapSettings },
                                        m_pointLightData);
                // --screenshot: the frame as tonemapped, before the UI is drawn over it.
                const bool screenshotFrame = !m_options.screenshot.empty() &&
                                             m_renderer->GetFrameCount() + 1 == m_options.screenshotFrame;
                if (screenshotFrame)
                    m_renderer->CaptureBackBuffer();
                m_renderer->BeginUiPass();
                m_imgui->Render(m_renderer->GetCommandList());
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
        for (ShaderReloadable* pass : m_sceneRenderer->GetShaderReloadables())
            allOk &= pass->ReloadShaders();
        if (allOk)
            log::Info("Shaders reloaded");
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

}
