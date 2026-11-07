#include "ui/ImGuiLayer.h"

#include "core/Window.h"
#include "gfx/Pix.h"
#include "renderer/Renderer.h"

#include <imgui.h>
#include <imgui_impl_dx12.h>
#include <imgui_impl_win32.h>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace walnut
{
    ImGuiLayer::ImGuiLayer(Window& window, Renderer& renderer)
        : m_renderer(renderer)
    {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();

        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

        ImGui::StyleColorsDark();
        const float dpiScale = window.GetDpiScale();
        ImGuiStyle& style = ImGui::GetStyle();
        style.ScaleAllSizes(dpiScale);
        style.FontScaleDpi = dpiScale;

        ImGui_ImplWin32_Init(window.GetHandle());
        window.AddMessageHandler([](HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
            if (!ImGui::GetCurrentContext())
                return false;
            return ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam) != 0;
        });

        ImGui_ImplDX12_InitInfo info;
        info.Device = renderer.GetDevice().Get();
        info.CommandQueue = renderer.GetDirectQueue().Get();
        info.NumFramesInFlight = static_cast<int>(Renderer::kFramesInFlight);
        info.RTVFormat = SwapChain::kFormat;
        info.DSVFormat = DXGI_FORMAT_UNKNOWN; // ImGui doesn't use depth, so its PSO ignores the bound depth buffer
        info.UserData = &renderer.GetSrvHeap();
        info.SrvDescriptorHeap = renderer.GetSrvHeap().Get();
        // ImGui asks us for SRV slots (font atlas and other textures) via these callbacks.
        info.SrvDescriptorAllocFn = [](ImGui_ImplDX12_InitInfo* initInfo, D3D12_CPU_DESCRIPTOR_HANDLE* outCpu, D3D12_GPU_DESCRIPTOR_HANDLE* outGpu) {
            const DescriptorHandle handle = static_cast<DescriptorHeap*>(initInfo->UserData)->Allocate();
            *outCpu = handle.cpu;
            *outGpu = handle.gpu;
        };
        info.SrvDescriptorFreeFn = [](ImGui_ImplDX12_InitInfo* initInfo, D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE) {
            static_cast<DescriptorHeap*>(initInfo->UserData)->Free(cpu);
        };
        ImGui_ImplDX12_Init(&info);
    }

    ImGuiLayer::~ImGuiLayer()
    {
        m_renderer.WaitForGpu();
        ImGui_ImplDX12_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
    }

    void ImGuiLayer::BeginFrame()
    {
        ImGui_ImplDX12_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
    }

    void ImGuiLayer::Render(ID3D12GraphicsCommandList* commandList)
    {
        {
            PIXScopedEvent(pix::kColorCpu, L"ImGui::Render");
            ImGui::Render();
        }
        PIXScopedEvent(commandList, pix::kColorGpu, L"ImGui");
        ID3D12DescriptorHeap* heaps[] = { m_renderer.GetSrvHeap().Get() };
        commandList->SetDescriptorHeaps(1, heaps);
        ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), commandList);
    }

    bool ImGuiLayer::WantsKeyboard() const
    {
        return ImGui::GetIO().WantCaptureKeyboard;
    }

    bool ImGuiLayer::WantsMouse() const
    {
        return ImGui::GetIO().WantCaptureMouse;
    }
}
