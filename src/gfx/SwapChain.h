#pragma once

#include "gfx/D3D12Common.h"
#include "gfx/DescriptorHeap.h"

#include <array>
#include <cstdint>

namespace walnut
{
    class Device;
    class CommandQueue;

    // Flip-model DXGI swapchain with one RTV per back buffer.
    class SwapChain
    {
    public:
        static constexpr uint32_t kBufferCount = 3;
        static constexpr DXGI_FORMAT kFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

        SwapChain(Device& device, CommandQueue& presentQueue, HWND hwnd, uint32_t width, uint32_t height);

        // The GPU must be idle (no back buffer in use) before calling this.
        void Resize(uint32_t width, uint32_t height);

        // Returns the Present HRESULT so the caller can handle device removal.
        HRESULT Present(bool vsync);

        uint32_t GetCurrentBackBufferIndex() const { return m_swapChain->GetCurrentBackBufferIndex(); }
        ID3D12Resource* GetBackBuffer(uint32_t index) const { return m_backBuffers[index].Get(); }
        D3D12_CPU_DESCRIPTOR_HANDLE GetRtv(uint32_t index) const { return m_rtvHeap.GetHandle(index).cpu; }

        uint32_t GetWidth() const { return m_width; }
        uint32_t GetHeight() const { return m_height; }
        bool IsTearingSupported() const { return m_tearingSupported; }

    private:
        void CreateRenderTargetViews();
        UINT GetSwapChainFlags() const;

        ID3D12Device* m_device = nullptr;
        ComPtr<IDXGISwapChain4> m_swapChain;
        std::array<ComPtr<ID3D12Resource>, kBufferCount> m_backBuffers;
        DescriptorHeap m_rtvHeap;
        uint32_t m_width = 0;
        uint32_t m_height = 0;
        bool m_tearingSupported = false;
    };
}
