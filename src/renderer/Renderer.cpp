#include "renderer/Renderer.h"

#include "core/Log.h"
#include "core/Window.h"
#include "gfx/GpuUtils.h"
#include "gfx/Pix.h"

#include <string>

namespace walnut
{
    namespace
    {
        // In D3D12_BARRIER_SUBRESOURCE_RANGE, an index of 0xffffffff (with NumMipLevels = 0) means
        // "every subresource".
        constexpr UINT kAllSubresources = 0xffffffff;
    }

    Renderer::Renderer(Window& window, const RendererDesc& desc)
        : m_vsync(desc.vsync)
    {
        m_device = std::make_unique<Device>(desc.device);
        ID3D12Device10* device = m_device->Get();

        m_directQueue = std::make_unique<CommandQueue>(device, D3D12_COMMAND_LIST_TYPE_DIRECT, L"DirectQueue");
        m_uploadContext = std::make_unique<UploadContext>(device, *m_directQueue);
        m_swapChain = std::make_unique<SwapChain>(*m_device, *m_directQueue, window.GetHandle(), window.GetWidth(), window.GetHeight());
        m_srvHeap = std::make_unique<DescriptorHeap>(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kSrvHeapCapacity, true, L"SrvHeap");

        for (uint32_t i = 0; i < kFramesInFlight; ++i)
        {
            WR_CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_frames[i].allocator)));
            const std::wstring name = L"FrameAllocator" + std::to_wstring(i);
            SetDebugName(m_frames[i].allocator.Get(), name.c_str());
        }

        // CreateCommandList1 creates the list closed, ready for BeginFrame to Reset it.
        WR_CHECK(device->CreateCommandList1(0, D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_LIST_FLAG_NONE,
                                            IID_PPV_ARGS(&m_commandList)));
        SetDebugName(m_commandList.Get(), L"FrameCommandList");

        log::Info("Renderer initialized: {}x{}, {} back buffers, {} frames in flight, tearing {}",
                  m_swapChain->GetWidth(), m_swapChain->GetHeight(), SwapChain::kBufferCount, kFramesInFlight,
                  m_device->IsTearingSupported() ? "supported" : "unsupported");
    }

    Renderer::~Renderer()
    {
        WaitForGpu();
    }

    void Renderer::WaitForGpu()
    {
        if (m_directQueue)
            m_directQueue->Flush();
    }

    void Renderer::BeginFrame(const float clearColor[4])
    {
        FrameContext& frame = m_frames[m_frameIndex];

        // Don't reuse this slot's allocator until the GPU has finished the commands recorded with it.
        {
            PIXScopedEvent(pix::kColorWait, L"Wait for frame fence");
            m_directQueue->WaitForFence(frame.fenceValue);
        }

        WR_CHECK(frame.allocator->Reset());
        WR_CHECK(m_commandList->Reset(frame.allocator.Get(), nullptr));
        PIXBeginEvent(m_commandList.Get(), pix::kColorFrame, L"Frame");

        m_backBufferIndex = m_swapChain->GetCurrentBackBufferIndex();
        ID3D12Resource* backBuffer = m_swapChain->GetBackBuffer(m_backBufferIndex);

        PIXScopedEvent(m_commandList.Get(), pix::kColorGpu, L"Clear back buffer");
        // PRESENT -> RENDER_TARGET. Nothing earlier in this command list touched the back buffer, and
        // ExecuteCommandLists boundaries already finish all prior GPU work (including the previous
        // Present), so there is nothing to wait for: SyncBefore = NONE, AccessBefore = NO_ACCESS.
        // SyncAfter/AccessAfter = RENDER_TARGET blocks the clear and draws until the layout change is done.
        const CD3DX12_TEXTURE_BARRIER toRenderTarget(
            D3D12_BARRIER_SYNC_NONE, D3D12_BARRIER_SYNC_RENDER_TARGET,
            D3D12_BARRIER_ACCESS_NO_ACCESS, D3D12_BARRIER_ACCESS_RENDER_TARGET,
            D3D12_BARRIER_LAYOUT_PRESENT, D3D12_BARRIER_LAYOUT_RENDER_TARGET,
            backBuffer, CD3DX12_BARRIER_SUBRESOURCE_RANGE(kAllSubresources));
        const CD3DX12_BARRIER_GROUP toRenderTargetGroup(1, &toRenderTarget);
        m_commandList->Barrier(1, &toRenderTargetGroup);

        const D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_swapChain->GetRtv(m_backBufferIndex);
        m_commandList->ClearRenderTargetView(rtv, clearColor, 0, nullptr);
        m_commandList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

        const CD3DX12_VIEWPORT viewport(0.0f, 0.0f, static_cast<float>(m_swapChain->GetWidth()),
                                        static_cast<float>(m_swapChain->GetHeight()));
        const CD3DX12_RECT scissor(0, 0, static_cast<LONG>(m_swapChain->GetWidth()),
                                   static_cast<LONG>(m_swapChain->GetHeight()));
        m_commandList->RSSetViewports(1, &viewport);
        m_commandList->RSSetScissorRects(1, &scissor);
    }

    void Renderer::EndFrame()
    {
        ID3D12Resource* backBuffer = m_swapChain->GetBackBuffer(m_backBufferIndex);
        // RENDER_TARGET -> PRESENT (= COMMON, the layout Present requires). Wait for every render-target
        // write before it (clear, triangle, ImGui). Nothing later in this list uses the buffer, so
        // SyncAfter = NONE and AccessAfter = NO_ACCESS; Present runs after ExecuteCommandLists completes.
        const CD3DX12_TEXTURE_BARRIER toPresent(
            D3D12_BARRIER_SYNC_RENDER_TARGET, D3D12_BARRIER_SYNC_NONE,
            D3D12_BARRIER_ACCESS_RENDER_TARGET, D3D12_BARRIER_ACCESS_NO_ACCESS,
            D3D12_BARRIER_LAYOUT_RENDER_TARGET, D3D12_BARRIER_LAYOUT_PRESENT,
            backBuffer, CD3DX12_BARRIER_SUBRESOURCE_RANGE(kAllSubresources));
        const CD3DX12_BARRIER_GROUP toPresentGroup(1, &toPresent);
        m_commandList->Barrier(1, &toPresentGroup);
        PIXEndEvent(m_commandList.Get()); // "Frame"

        WR_CHECK(m_commandList->Close());
        m_directQueue->Execute(m_commandList.Get());

        HRESULT presentResult = S_OK;
        {
            // Time spent here is DXGI throttling the CPU (frame queue full / waiting for vsync).
            PIXScopedEvent(pix::kColorWait, L"Present");
            presentResult = m_swapChain->Present(m_vsync);
        }
        if (presentResult == DXGI_ERROR_DEVICE_REMOVED || presentResult == DXGI_ERROR_DEVICE_RESET)
        {
            m_device->LogDeviceRemoved();
            ThrowIfFailed(presentResult, "IDXGISwapChain::Present");
        }
        WR_CHECK(presentResult);

        m_frames[m_frameIndex].fenceValue = m_directQueue->Signal();
        m_frameIndex = (m_frameIndex + 1) % kFramesInFlight;
        ++m_frameCount;
    }

    void Renderer::Resize(uint32_t width, uint32_t height)
    {
        PIXScopedEvent(pix::kColorWait, L"Resize swapchain");
        WaitForGpu();
        m_swapChain->Resize(width, height);
        log::Info("Swapchain resized to {}x{}", width, height);
    }
}
