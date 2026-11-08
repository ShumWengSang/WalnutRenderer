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
#include <numbers>
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

    AppOptions ParseCommandLine()
    {
        AppOptions options;
#ifdef _DEBUG
        options.debugLayer = true;
#endif
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (!argv)
            return options;

        constexpr std::wstring_view kExitAfterFrames = L"--exit-after-frames=";
        constexpr std::wstring_view kExitAfterSeconds = L"--exit-after-seconds=";
        constexpr std::wstring_view kPixCaptureFrame = L"--pix-capture-frame=";
        constexpr std::wstring_view kScene = L"--scene=";
        for (int i = 1; i < argc; ++i)
        {
            const std::wstring_view arg = argv[i];
            if (arg == L"--debug-layer")
                options.debugLayer = true;
            else if (arg == L"--no-debug-layer")
                options.debugLayer = false;
            else if (arg == L"--gpu-validation")
                options.gpuValidation = true;
            else if (arg == L"--pix")
                options.pixGpuCapturer = true;
            else if (arg.starts_with(kPixCaptureFrame))
            {
                options.pixGpuCapturer = true;
                options.pixCaptureFrame = std::wcstoull(std::wstring(arg.substr(kPixCaptureFrame.size())).c_str(), nullptr, 10);
            }
            else if (arg.starts_with(kExitAfterFrames))
                options.exitAfterFrames = std::wcstoull(std::wstring(arg.substr(kExitAfterFrames.size())).c_str(), nullptr, 10);
            else if (arg.starts_with(kExitAfterSeconds))
                options.exitAfterSeconds = std::wcstod(std::wstring(arg.substr(kExitAfterSeconds.size())).c_str(), nullptr);
            else if (arg.starts_with(kScene))
                options.scene = arg.substr(kScene.size());
            else
                log::Warn("Unknown command-line argument: {}", ToUtf8(arg));
        }
        LocalFree(argv);
        return options;
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
                m_scene = BuildSceneFromGltf(document->GetData());
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
        std::copy(std::begin(Renderer::kDefaultClearColor), std::end(Renderer::kDefaultClearColor), m_clearColor);

#ifdef _DEBUG
        constexpr bool kShaderDebugInfo = true;
#else
        constexpr bool kShaderDebugInfo = false;
#endif
        m_shaderCompiler = std::make_unique<ShaderCompiler>(kShaderDebugInfo, m_exeDir / "shaders");
        m_trianglePass = std::make_unique<TrianglePass>(m_renderer->GetDevice(), m_renderer->GetUploadContext(),
                                                        m_renderer->GetSrvHeap(), *m_shaderCompiler,
                                                        m_exeDir / "shaders" / "triangle.hlsl", m_renderer->GetRootSignature(),
                                                        Renderer::kSceneColorFormat, Renderer::kDepthFormat);
        m_tonemapPass = std::make_unique<TonemapPass>(m_renderer->GetDevice(), *m_shaderCompiler,
                                                      m_exeDir / "shaders" / "tonemap.hlsl", m_renderer->GetRootSignature(),
                                                      SwapChain::kSrgbFormat);

        // Execute every upload recorded while loading, before the first frame reads that data.
        m_renderer->GetUploadContext().Flush();

        m_imgui = std::make_unique<ImGuiLayer>(*m_window, *m_renderer);
    }

    Application::~Application()
    {
        if (m_renderer)
            m_renderer->WaitForGpu();

        // Tear down in dependency order: everything that holds GPU objects goes before the renderer.
        m_imgui.reset();
        m_trianglePass.reset();
        m_tonemapPass.reset();
        m_shaderCompiler.reset();
        m_renderer.reset();
        m_window.reset();

        if (m_options.debugLayer || m_options.gpuValidation)
            ReportLiveObjects();
    }

    int Application::Run()
    {
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

            // The aspect ratio comes from the back buffer, so the image doesn't stretch when the window resizes.
            const SwapChain& swapChain = m_renderer->GetSwapChain();
            const float aspect = static_cast<float>(swapChain.GetWidth()) / static_cast<float>(swapChain.GetHeight());
            const DirectX::XMMATRIX viewProjection = m_camera.GetViewProjectionMatrix(aspect);

            {
                PIXScopedEvent(pix::kColorCpu, L"Render");
                m_renderer->BeginFrame(m_clearColor);
                ID3D12GraphicsCommandList* commandList = m_renderer->GetCommandList();
                FrameAllocator& frameAllocator = m_renderer->GetFrameAllocator();
                // Deliberately drawn near-to-far: the far triangle is drawn last. Without the depth test it
                // paints over the near one; with it, its hidden pixels fail the GREATER test and are discarded.
                // The brightness multiplier can push their colors above 1.0 (HDR) to show what tonemapping does.
                TriangleParams front = m_triangle;
                TriangleParams back = m_backTriangle;
                for (int channel = 0; channel < 3; ++channel)
                {
                    front.tint[channel] *= m_triangleBrightness;
                    back.tint[channel] *= m_triangleBrightness;
                }
                m_trianglePass->Render(commandList, frameAllocator, front, viewProjection, m_depthTest, m_textureView);
                m_trianglePass->Render(commandList, frameAllocator, back, viewProjection, m_depthTest, m_textureView);

                // The 3D scene is done; from here on we draw into the back buffer.
                m_renderer->BeginBackBufferPass();
                m_tonemapPass->Render(commandList, m_renderer->GetSceneColorSrvIndex(), m_tonemapSettings);
                m_renderer->BeginUiPass();
                m_imgui->Render(commandList);
                m_renderer->EndFrame();
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
        if (m_input.WasKeyPressed(VK_SPACE))
            m_autoRotate = !m_autoRotate;

        m_cameraController.Update(m_camera, m_input, deltaSeconds);

        if (m_autoRotate)
            m_triangle.angle += m_rotationSpeed * deltaSeconds;
        m_triangle.angle = std::remainder(m_triangle.angle, 2.0f * std::numbers::pi_v<float>);
    }

    void Application::ReloadShaders()
    {
        // The old PSOs may still be referenced by in-flight frames.
        m_renderer->WaitForGpu();
        const bool triangleOk = m_trianglePass->ReloadShaders();
        const bool tonemapOk = m_tonemapPass->ReloadShaders();
        if (triangleOk && tonemapOk)
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

    void Application::DrawUI(float deltaSeconds)
    {
        const float frameMs = deltaSeconds * 1000.0f;
        m_smoothedFrameMs = (m_smoothedFrameMs == 0.0f) ? frameMs : m_smoothedFrameMs * 0.95f + frameMs * 0.05f;

        const SwapChain& swapChain = m_renderer->GetSwapChain();
        Device& device = m_renderer->GetDevice();

        ImGui::SetNextWindowPos(ImVec2(10.0f, 10.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(450.0f, 0.0f), ImGuiCond_FirstUseEver);
        ImGui::Begin("WalnutRenderer");

        ImGui::Text("Adapter: %s", ToUtf8(device.GetAdapterName()).c_str());
        ImGui::Text("%.2f ms/frame (%.0f FPS)", m_smoothedFrameMs, m_smoothedFrameMs > 0.0f ? 1000.0f / m_smoothedFrameMs : 0.0f);
        ImGui::Text("Back buffer: %u x %u, frame %llu", swapChain.GetWidth(), swapChain.GetHeight(),
                    static_cast<unsigned long long>(m_renderer->GetFrameCount()));
        ImGui::Text("Debug layer: %s", device.IsDebugLayerEnabled() ? "on" : "off");
        ImGui::Text("Shader Model %s (compiling for %ls), Resource Binding Tier %d",
                    ToString(device.GetShaderModel()).c_str(), kShaderModelSuffix,
                    static_cast<int>(device.GetResourceBindingTier()));

        if (ImGui::CollapsingHeader("Renderer", ImGuiTreeNodeFlags_DefaultOpen))
        {
            bool vsync = m_renderer->GetVsync();
            if (ImGui::Checkbox("VSync", &vsync))
                m_renderer->SetVsync(vsync);
            ImGui::SameLine();
            ImGui::TextDisabled(swapChain.IsTearingSupported() ? "(tearing supported)" : "(tearing unsupported)");
            // Float + HDR: scene colors are linear light and may go above 1.0.
            ImGui::ColorEdit3("Clear color (linear)", m_clearColor, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);

            // Read before this frame's allocations, so this shows what the previous frame used.
            const FrameAllocator& frameAllocator = m_renderer->GetFrameAllocator();
            ImGui::Text("Frame upload memory: %llu / %llu bytes",
                        static_cast<unsigned long long>(frameAllocator.GetUsedBytes()),
                        static_cast<unsigned long long>(frameAllocator.GetBytesPerFrame()));
            const DescriptorHeap& srvHeap = m_renderer->GetSrvHeap();
            ImGui::Text("Bindless descriptors: %u / %u used", srvHeap.GetAllocatedCount(), srvHeap.GetCapacity());
        }

        if (ImGui::CollapsingHeader("Scene", ImGuiTreeNodeFlags_DefaultOpen))
        {
            const std::string fileName = ToUtf8(m_scenePath.filename().wstring());
            if (m_sceneSummary)
            {
                const GltfSummary& s = *m_sceneSummary;
                ImGui::Text("%s (parsed in %.1f ms, built in %.1f ms)", fileName.c_str(), m_sceneLoadMs, m_sceneBuildMs);
                ImGui::SetItemTooltip("%s", ToUtf8(m_scenePath.wstring()).c_str());
                ImGui::Text("%zu nodes, %zu meshes, %zu materials, %zu images", s.nodes, s.meshes, s.materials, s.images);
                ImGui::Text("%zu instances of %zu primitives", m_scene.instances.size(), m_scene.primitives.size());
                ImGui::Text("%zu vertices, %zu triangles", m_scene.vertices.size(), m_scene.indices.size() / 3);
                const DirectX::XMFLOAT3& lo = m_scene.boundsMin;
                const DirectX::XMFLOAT3& hi = m_scene.boundsMax;
                ImGui::Text("Size %.1f x %.1f x %.1f", hi.x - lo.x, hi.y - lo.y, hi.z - lo.z);
                ImGui::SetItemTooltip("World bounds (%.2f, %.2f, %.2f) to (%.2f, %.2f, %.2f)", lo.x, lo.y, lo.z, hi.x, hi.y, hi.z);
                ImGui::TextDisabled("CPU only: not on the GPU yet");
            }
            else
            {
                ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "%s %s", fileName.c_str(), m_sceneError.c_str());
                ImGui::SetItemTooltip("%s", ToUtf8(m_scenePath.wstring()).c_str());
            }
        }

        if (ImGui::CollapsingHeader("Tonemapping", ImGuiTreeNodeFlags_DefaultOpen))
        {
            const char* operators[] = { "Clamp (none)", "Reinhard", "ACES (fitted)" };
            int op = static_cast<int>(m_tonemapSettings.op);
            if (ImGui::Combo("Curve", &op, operators, IM_ARRAYSIZE(operators)))
                m_tonemapSettings.op = static_cast<TonemapOperator>(op);
            ImGui::SliderFloat("Exposure", &m_tonemapSettings.exposureStops, -6.0f, 6.0f, "%+.1f stops");
            ImGui::SliderFloat("Triangle brightness", &m_triangleBrightness, 0.0f, 16.0f, "%.2fx",
                               ImGuiSliderFlags_Logarithmic);
            ImGui::TextDisabled("Output: back buffer _SRGB view (hardware linear -> sRGB)");
        }

        if (ImGui::CollapsingHeader("Camera", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::DragFloat3("Position", &m_camera.position.x, 0.01f);
            ImGui::SliderAngle("Yaw", &m_camera.yaw, -180.0f, 180.0f);
            ImGui::SliderAngle("Pitch", &m_camera.pitch, -89.0f, 89.0f);
            ImGui::SliderAngle("Vertical FOV", &m_camera.verticalFov, 10.0f, 120.0f);
            ImGui::SliderFloat("Near plane", &m_camera.nearZ, 0.01f, 2.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
            ImGui::SliderFloat("Move speed", &m_cameraController.moveSpeed, 0.05f, 500.0f, "%.2f m/s", ImGuiSliderFlags_Logarithmic);
            if (ImGui::Button("Reset camera"))
                m_camera = Camera{};
        }

        if (ImGui::CollapsingHeader("Triangle", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::Checkbox("Depth test", &m_depthTest);
            ImGui::SameLine();
            ImGui::TextDisabled("(darker triangle is behind, but drawn last)");
            const char* filterModes[] = { "Point, no mips", "Bilinear, no mips", "Trilinear (mips)",
                                          "Anisotropic 16x (mips)", "Show mip level" };
            int filterMode = static_cast<int>(m_textureView.filterMode);
            if (ImGui::Combo("Texture filter", &filterMode, filterModes, IM_ARRAYSIZE(filterModes)))
                m_textureView.filterMode = static_cast<TextureFilterMode>(filterMode);
            ImGui::SliderFloat("UV tiling", &m_textureView.uvScale, 1.0f, 64.0f, "%.1fx", ImGuiSliderFlags_Logarithmic);
            ImGui::ColorEdit4("Tint", m_triangle.tint);
            ImGui::SliderFloat3("Offset", m_triangle.offset, -1.0f, 1.0f);
            ImGui::SliderAngle("Angle", &m_triangle.angle, -180.0f, 180.0f);
            ImGui::SliderFloat("Scale", &m_triangle.scale, 0.1f, 4.0f);
            ImGui::Checkbox("Auto-rotate", &m_autoRotate);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(120.0f);
            ImGui::SliderFloat("Speed", &m_rotationSpeed, -6.0f, 6.0f, "%.1f rad/s");
            if (ImGui::Button("Reload shaders (F5)"))
                ReloadShaders();
            for (const std::string* error : { &m_trianglePass->GetLastError(), &m_tonemapPass->GetLastError() })
            {
                if (error->empty())
                    continue;
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
                ImGui::TextWrapped("%s", error->c_str());
                ImGui::PopStyleColor();
            }
        }

        if (ImGui::CollapsingHeader("Input", ImGuiTreeNodeFlags_DefaultOpen))
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
            ImGui::TextDisabled("Shift: fast   wheel: speed   Space: auto-rotate   Esc: quit");
        }

        DrawPixUI();

        ImGui::Checkbox("Show ImGui demo window", &m_showDemoWindow);
        ImGui::End();

        if (m_showDemoWindow)
            ImGui::ShowDemoWindow(&m_showDemoWindow);
    }
}
