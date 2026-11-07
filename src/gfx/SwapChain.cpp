#include "gfx/SwapChain.h"

#include "gfx/CommandQueue.h"
#include "gfx/Device.h"
#include "gfx/GpuUtils.h"

#include <string>

namespace walnut
{
    SwapChain::SwapChain(Device& device, CommandQueue& presentQueue, HWND hwnd, uint32_t width, uint32_t height)
        : m_device(device.Get()),
          m_rtvHeap(device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kBufferCount * 2, false, L"SwapChainRtvHeap"),
          m_width(width),
          m_height(height),
          m_tearingSupported(device.IsTearingSupported())
    {
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = width;
        desc.Height = height;
        desc.Format = kFormat;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = kBufferCount;
        desc.Scaling = DXGI_SCALING_STRETCH;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD; // flip model is required for D3D12
        desc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
        desc.Flags = GetSwapChainFlags();

        // For D3D12 the swapchain is bound to a command queue, not the device.
        ComPtr<IDXGISwapChain1> swapChain1;
        WR_CHECK(device.GetFactory()->CreateSwapChainForHwnd(presentQueue.Get(), hwnd, &desc, nullptr, nullptr, &swapChain1));
        WR_CHECK(swapChain1.As(&m_swapChain));

        // Exclusive fullscreen via Alt+Enter is out of scope for now.
        WR_CHECK(device.GetFactory()->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER));

        CreateRenderTargetViews();
    }

    void SwapChain::Resize(uint32_t width, uint32_t height)
    {
        if (width == 0 || height == 0 || (width == m_width && height == m_height))
            return;

        // All references to the back buffers must be released before ResizeBuffers.
        for (auto& buffer : m_backBuffers)
            buffer.Reset();

        WR_CHECK(m_swapChain->ResizeBuffers(kBufferCount, width, height, kFormat, GetSwapChainFlags()));
        m_width = width;
        m_height = height;
        CreateRenderTargetViews();
    }

    HRESULT SwapChain::Present(bool vsync)
    {
        // Tearing (uncapped, variable refresh) is only allowed with sync interval 0.
        const UINT syncInterval = vsync ? 1 : 0;
        const UINT flags = (!vsync && m_tearingSupported) ? DXGI_PRESENT_ALLOW_TEARING : 0;
        return m_swapChain->Present(syncInterval, flags);
    }

    void SwapChain::CreateRenderTargetViews()
    {
        D3D12_RENDER_TARGET_VIEW_DESC srgbDesc{};
        srgbDesc.Format = kSrgbFormat;
        srgbDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;

        for (uint32_t i = 0; i < kBufferCount; ++i)
        {
            WR_CHECK(m_swapChain->GetBuffer(i, IID_PPV_ARGS(&m_backBuffers[i])));
            const std::wstring name = L"BackBuffer" + std::to_wstring(i);
            SetDebugName(m_backBuffers[i].Get(), name.c_str());
            // Slots [0, kBufferCount): plain views (nullptr desc = the resource's own format, UNORM).
            m_device->CreateRenderTargetView(m_backBuffers[i].Get(), nullptr, m_rtvHeap.GetHandle(i).cpu);
            // Slots [kBufferCount, 2 * kBufferCount): _SRGB views of the same buffers.
            m_device->CreateRenderTargetView(m_backBuffers[i].Get(), &srgbDesc, m_rtvHeap.GetHandle(kBufferCount + i).cpu);
        }
    }

    UINT SwapChain::GetSwapChainFlags() const
    {
        return m_tearingSupported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    }
}
