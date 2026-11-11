#pragma once

#include "gfx/CommandQueue.h"
#include "gfx/D3D12Common.h"
#include "gfx/DescriptorHeap.h"
#include "gfx/Device.h"
#include "gfx/FrameAllocator.h"
#include "gfx/SwapChain.h"
#include "gfx/UploadContext.h"
#include "renderer/FrameConstants.h"

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
    //   renderer.BeginFrame(clearColor);   // waits for this frame slot; scene color + depth cleared and bound
    //   ... draw the 3D scene ...          //   (into the HDR scene color target, not the back buffer)
    //   renderer.BeginBackBufferPass();    // scene color becomes readable; back buffer (_SRGB view) is the target
    //   ... tonemap pass ...
    //   renderer.BeginUiPass();            // back buffer's plain (UNORM) view is the target
    //   ... UI ...
    //   renderer.EndFrame();               // submits and presents
    class Renderer
    {
    public:
        // Frames the CPU may record ahead of the GPU. Independent of the swapchain buffer count.
        static constexpr uint32_t kFramesInFlight = 2;
        // The one shader-visible CBV/SRV/UAV heap. Bindless shaders can index any slot, and only one heap of this
        // type can be bound at a time, so everything (scene textures, buffers, ImGui) shares it.
        static constexpr uint32_t kSrvHeapCapacity = 65536;
        static constexpr uint64_t kFrameAllocatorBytesPerFrame = 1024 * 1024;
        static constexpr uint64_t kUploadRingBytes = 64 * 1024 * 1024;

        // 32-bit float depth: required for reversed-Z to pay off (see Camera.cpp).
        static constexpr DXGI_FORMAT kDepthFormat = DXGI_FORMAT_D32_FLOAT;
        // Reversed-Z: 1 = near plane, 0 = infinitely far. "Empty" depth is therefore 0, and a pixel is closer
        // (and wins) when its depth is GREATER.
        static constexpr float kDepthClearValue = 0.0f;
        // The 3D scene is rendered into this "HDR" format first; see CreateSceneColorTarget.
        static constexpr DXGI_FORMAT kSceneColorFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
        // In linear light, like every color in the scene target (it goes through tonemapping and sRGB encoding).
        static constexpr float kDefaultClearColor[4] = { 0.02f, 0.025f, 0.04f, 1.0f };

        Renderer(Window& window, const RendererDesc& desc);
        ~Renderer();

        Renderer(const Renderer&) = delete;
        Renderer& operator=(const Renderer&) = delete;

        void BeginFrame(const float clearColor[4]);
        // Copies 'constants' into this frame's upload memory and binds them at b2 for every following draw.
        // Call after BeginFrame (which binds the root signature; that clears root arguments).
        void SetFrameConstants(const FrameConstants& constants);
        void BeginBackBufferPass();
        void BeginUiPass();
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
        ID3D12RootSignature* GetRootSignature() const { return m_rootSignature.Get(); }
        // Bindless heap index of the scene color texture's SRV (readable after BeginBackBufferPass).
        uint32_t GetSceneColorSrvIndex() const { return m_sceneSrv.index; }

        // Binds the shared descriptor heap and the bindless root signature. BeginFrame does this; call it again
        // after anything that changes the root signature (e.g. ImGui) before drawing with our passes.
        void BindBindlessState(ID3D12GraphicsCommandList* commandList) const;

        bool GetVsync() const { return m_vsync; }
        void SetVsync(bool vsync) { m_vsync = vsync; }
        uint64_t GetFrameCount() const { return m_frameCount; }

    private:
        struct FrameContext
        {
            ComPtr<ID3D12CommandAllocator> allocator;
            uint64_t fenceValue = 0;
        };

        void CreateDepthBuffer(uint32_t width, uint32_t height);
        void CreateSceneColorTarget(uint32_t width, uint32_t height);

        std::unique_ptr<Device> m_device;
        std::unique_ptr<CommandQueue> m_directQueue;
        std::unique_ptr<UploadContext> m_uploadContext; // after the queue: destroyed before it
        std::unique_ptr<SwapChain> m_swapChain;
        std::unique_ptr<DescriptorHeap> m_srvHeap;
        ComPtr<ID3D12RootSignature> m_rootSignature;
        std::unique_ptr<DescriptorHeap> m_dsvHeap;
        DescriptorHandle m_dsv;
        ComPtr<ID3D12Resource> m_depthBuffer;
        std::unique_ptr<DescriptorHeap> m_rtvHeap; // for the scene color target (the swapchain has its own)
        DescriptorHandle m_sceneRtv;
        DescriptorHandle m_sceneSrv;
        ComPtr<ID3D12Resource> m_sceneColor;
        std::unique_ptr<FrameAllocator> m_frameAllocator;
        std::array<FrameContext, kFramesInFlight> m_frames;
        ComPtr<ID3D12GraphicsCommandList7> m_commandList;

        uint32_t m_frameIndex = 0;
        uint32_t m_backBufferIndex = 0;
        uint64_t m_frameCount = 0;
        bool m_vsync = true;
    };
}
