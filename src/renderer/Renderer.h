#pragma once

#include "gfx/CommandQueue.h"
#include "gfx/D3D12Common.h"
#include "gfx/DescriptorHeap.h"
#include "gfx/Device.h"
#include "gfx/FrameAllocator.h"
#include "gfx/SwapChain.h"
#include "gfx/UploadContext.h"

#include <array>
#include <cstdint>
#include <memory>

namespace walnut
{
    class Window;

    struct RendererDesc
    {
        DeviceDesc device;
        bool vsync = true;
    };

    // Owns the core D3D12 objects and drives the per-frame command list.
    //
    //   renderer.BeginFrame(clearColor);  // waits for this frame slot, clears the back buffer
    //   ... record draws into GetCommandList() ...
    //   renderer.EndFrame();              // submits and presents
    class Renderer
    {
    public:
        // Frames the CPU may record ahead of the GPU. Independent of the swapchain buffer count.
        static constexpr uint32_t kFramesInFlight = 2;
        static constexpr uint32_t kSrvHeapCapacity = 256;
        static constexpr uint64_t kFrameAllocatorBytesPerFrame = 1024 * 1024;
        static constexpr uint64_t kUploadRingBytes = 64 * 1024 * 1024;

        Renderer(Window& window, const RendererDesc& desc);
        ~Renderer();

        Renderer(const Renderer&) = delete;
        Renderer& operator=(const Renderer&) = delete;

        void BeginFrame(const float clearColor[4]);
        void EndFrame();
        void Resize(uint32_t width, uint32_t height);
        void WaitForGpu();

        // Command list 7 is the first version with Barrier() (Enhanced Barriers).
        ID3D12GraphicsCommandList7* GetCommandList() const { return m_commandList.Get(); }
        Device& GetDevice() { return *m_device; }
        CommandQueue& GetDirectQueue() { return *m_directQueue; }
        UploadContext& GetUploadContext() { return *m_uploadContext; }
        FrameAllocator& GetFrameAllocator() { return *m_frameAllocator; }
        SwapChain& GetSwapChain() { return *m_swapChain; }
        DescriptorHeap& GetSrvHeap() { return *m_srvHeap; }

        bool GetVsync() const { return m_vsync; }
        void SetVsync(bool vsync) { m_vsync = vsync; }
        uint64_t GetFrameCount() const { return m_frameCount; }

    private:
        struct FrameContext
        {
            ComPtr<ID3D12CommandAllocator> allocator;
            uint64_t fenceValue = 0;
        };

        std::unique_ptr<Device> m_device;
        std::unique_ptr<CommandQueue> m_directQueue;
        std::unique_ptr<UploadContext> m_uploadContext; // after the queue: destroyed before it
        std::unique_ptr<SwapChain> m_swapChain;
        std::unique_ptr<DescriptorHeap> m_srvHeap;
        std::unique_ptr<FrameAllocator> m_frameAllocator;
        std::array<FrameContext, kFramesInFlight> m_frames;
        ComPtr<ID3D12GraphicsCommandList7> m_commandList;

        uint32_t m_frameIndex = 0;
        uint32_t m_backBufferIndex = 0;
        uint64_t m_frameCount = 0;
        bool m_vsync = true;
    };
}
