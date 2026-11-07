#pragma once

#include "gfx/D3D12Common.h"
#include "gfx/DescriptorHeap.h"

#include <array>
#include <cstdint>

namespace walnut
{
    class Device;
    class CommandQueue;

    // Flip-model DXGI swapchain with two RTVs per back buffer:
    //  - an _SRGB view: the hardware converts linear colors to sRGB as it writes (used by the tonemap pass),
    //  - a plain UNORM view: values are written unchanged (used by ImGui, whose colors are already sRGB).
    // Both views point at the same memory; only how written values are encoded differs.
    class SwapChain
    {
    public:
        static constexpr uint32_t kBufferCount = 3;
        // Flip-model swapchains must use a non-sRGB format for the buffers themselves...
        static constexpr DXGI_FORMAT kFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
        // ...but render target views of them may use the _SRGB variant.
        static constexpr DXGI_FORMAT kSrgbFormat = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;

        SwapChain(Device& device, CommandQueue& presentQueue, HWND hwnd, uint32_t width, uint32_t height);

        // The GPU must be idle (no back buffer in use) before calling this.
        void Resize(uint32_t width, uint32_t height);

        // Returns the Present HRESULT so the caller can handle device removal.
        HRESULT Present(bool vsync);

        uint32_t GetCurrentBackBufferIndex() const { return m_swapChain->GetCurrentBackBufferIndex(); }
        ID3D12Resource* GetBackBuffer(uint32_t index) const { return m_backBuffers[index].Get(); }
        D3D12_CPU_DESCRIPTOR_HANDLE GetRtv(uint32_t index) const { return m_rtvHeap.GetHandle(index).cpu; }
        D3D12_CPU_DESCRIPTOR_HANDLE GetSrgbRtv(uint32_t index) const { return m_rtvHeap.GetHandle(kBufferCount + index).cpu; }

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
