#include "renderer/Renderer.h"

#include "core/Log.h"
#include "core/Window.h"
#include "gfx/GpuUtils.h"

#include <string>

namespace walnut
{
    Renderer::Renderer(Window& window, const RendererDesc& desc)
        : m_vsync(desc.vsync)
    {
        m_device = std::make_unique<Device>(desc.device);
        ID3D12Device* device = m_device->Get();

        m_directQueue = std::make_unique<CommandQueue>(device, D3D12_COMMAND_LIST_TYPE_DIRECT, L"DirectQueue");
        m_swapChain = std::make_unique<SwapChain>(*m_device, *m_directQueue, window.GetHandle(), window.GetWidth(), window.GetHeight());
        m_srvHeap = std::make_unique<DescriptorHeap>(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kSrvHeapCapacity, true, L"SrvHeap");

        for (uint32_t i = 0; i < kFramesInFlight; ++i)
        {
            WR_CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_frames[i].allocator)));
            const std::wstring name = L"FrameAllocator" + std::to_wstring(i);
            SetDebugName(m_frames[i].allocator.Get(), name.c_str());
        }

        // Command lists are created in the recording state; close it so BeginFrame can Reset it.
        WR_CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_frames[0].allocator.Get(), nullptr,
                                           IID_PPV_ARGS(&m_commandList)));
        SetDebugName(m_commandList.Get(), L"FrameCommandList");
        WR_CHECK(m_commandList->Close());

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
        m_directQueue->WaitForFence(frame.fenceValue);

        WR_CHECK(frame.allocator->Reset());
        WR_CHECK(m_commandList->Reset(frame.allocator.Get(), nullptr));

        m_backBufferIndex = m_swapChain->GetCurrentBackBufferIndex();
        ID3D12Resource* backBuffer = m_swapChain->GetBackBuffer(m_backBufferIndex);

        const auto toRenderTarget = CD3DX12_RESOURCE_BARRIER::Transition(
            backBuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        m_commandList->ResourceBarrier(1, &toRenderTarget);

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
        const auto toPresent = CD3DX12_RESOURCE_BARRIER::Transition(
            backBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        m_commandList->ResourceBarrier(1, &toPresent);

        WR_CHECK(m_commandList->Close());
        m_directQueue->Execute(m_commandList.Get());

        const HRESULT presentResult = m_swapChain->Present(m_vsync);
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
        WaitForGpu();
        m_swapChain->Resize(width, height);
        log::Info("Swapchain resized to {}x{}", width, height);
    }
}
