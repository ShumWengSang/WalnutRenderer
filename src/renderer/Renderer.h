#pragma once

#include "gfx/Barriers.h"
#include "gfx/CommandQueue.h"
#include "gfx/D3D12Common.h"
#include "gfx/DescriptorHeap.h"
#include "gfx/Device.h"
#include "gfx/FrameAllocator.h"
#include "gfx/SwapChain.h"
#include "gfx/UploadContext.h"
#include "renderer/FrameConstants.h"
#include "renderer/GBuffer.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>

namespace walnut
{
    class Window;

    struct RendererDesc
    {
        DeviceDesc device;
        bool vsync = true;
    };

    // Owns the core D3D12 objects and drives the per-frame command list. SceneRenderer::Record makes the calls between
    // BeginFrame and BeginUiPass. Each Begin*Pass asks for the states its targets need (TrackedTexture): the barrier
    // comes from wherever the previous pass left them, so the passes can be skipped or reordered freely.
    //
    //   renderer.BeginFrame(clearColor);   // waits for this frame slot; scene color + depth cleared, b1 zeroed
    //   renderer.SetFrameConstants(...);   // camera + lights at b2 for every pass (graphics and compute)
    //   ... compute (light clusters) ...
    //   ... shadow map + cube shadow passes ...  // (their own depth targets; the cubes set b1, then ResetDrawData)
    //   renderer.BeginDepthPrepass();      // camera depth only, no color target (optional for forward)
    //   ... depth pre-pass ...
    //   either forward:
    //     renderer.BeginScenePass();       // scene color + depth bound
    //     ... draw the 3D scene ...        //   (into the HDR scene color target, not the back buffer)
    //   or deferred:
    //     renderer.BeginGBufferPass();     // the G-buffer's 4 targets + depth
    //     ... G-buffer draws ...
    //     renderer.BeginDeferredPass();    // G-buffer + depth readable; scene color is the target
    //     ... fullscreen lighting ...
    //   renderer.BeginOverlayPass();       // optional: scene color + depth again (light markers)
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
        static constexpr DXGI_FORMAT kDepthFormat = DXGI_FORMAT_D32_FLOAT; // as a depth buffer (DSV, PSOs)
        // The depth texture is created typeless so it can also be read as plain floats (deferred shading).
        static constexpr DXGI_FORMAT kDepthResourceFormat = DXGI_FORMAT_R32_TYPELESS;
        static constexpr DXGI_FORMAT kDepthSrvFormat = DXGI_FORMAT_R32_FLOAT;
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
        // Binds only the depth buffer and a full-window viewport, for the depth pre-pass.
        void BeginDepthPrepass();
        // Binds the scene color + depth targets and a full-window viewport (after any shadow/offscreen passes).
        void BeginScenePass();
        // Points b1 (root parameter DrawData) back at an all-zero block, after a pass that set it per draw.
        void ResetDrawData();
        // Deferred shading: the G-buffer targets + depth (after the depth pre-pass) ...
        void BeginGBufferPass();
        // ... then the G-buffer and depth become shader-readable and only the scene color is bound, for a
        // fullscreen pass.
        void BeginDeferredPass();
        // Scene color + depth again, after the scene is shaded (forward or deferred), for things drawn on top that
        // are still hidden behind closer geometry (light markers). After a deferred pass this switches the depth
        // buffer back from shader-readable to a depth target.
        void BeginOverlayPass();
        void BeginBackBufferPass();
        void BeginUiPass();
        // Screenshots (--screenshot): copies the back buffer as it is now (call after the tonemap pass, before the
        // UI, to leave the UI out) into a readback buffer. SaveScreenshot waits for the GPU and writes a PNG.
        void CaptureBackBuffer();
        bool SaveScreenshot(const std::filesystem::path& path);
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
        uint32_t GetDepthSrvIndex() const { return m_depthSrv.index; }
        const GBuffer& GetGBuffer() const { return *m_gbuffer; }

        // Binds the shared descriptor heap and the bindless root signature. BeginFrame does this; call it again
        // after anything that changes the root signature (e.g. ImGui) before drawing with our passes.
        void BindBindlessState(ID3D12GraphicsCommandList* commandList) const;

        bool GetVsync() const { return m_vsync; }
        void SetVsync(bool vsync) { m_vsync = vsync; }
        uint64_t GetFrameCount() const { return m_frameCount; }
        uint32_t GetFrameIndex() const { return m_frameIndex; } // which of the kFramesInFlight slots is recording

    private:
        struct FrameContext
        {
            ComPtr<ID3D12CommandAllocator> allocator;
            uint64_t fenceValue = 0;
        };

        void CreateDepthBuffer(uint32_t width, uint32_t height);
        // One barrier call: 'texture' from its current state to 'to' (nothing if it's already there).
        void Transition(TrackedTexture& texture, const TextureAccess& to, bool discard = false);
        void SetWindowViewport();
        void CreateSceneColorTarget(uint32_t width, uint32_t height);

        std::unique_ptr<Device> m_device;
        std::unique_ptr<CommandQueue> m_directQueue;
        std::unique_ptr<UploadContext> m_uploadContext; // after the queue: destroyed before it
        std::unique_ptr<SwapChain> m_swapChain;
        std::unique_ptr<DescriptorHeap> m_srvHeap;
        ComPtr<ID3D12RootSignature> m_rootSignature;
        std::unique_ptr<DescriptorHeap> m_dsvHeap;
        OwnedDescriptor m_dsv;
        OwnedDescriptor m_depthSrv; // the depth buffer as Texture2D<float> (bindless)
        ComPtr<ID3D12Resource> m_depthBuffer;
        std::unique_ptr<GBuffer> m_gbuffer;
        std::unique_ptr<DescriptorHeap> m_rtvHeap; // for the scene color target (the swapchain has its own)
        OwnedDescriptor m_sceneRtv;
        OwnedDescriptor m_sceneSrv;
        ComPtr<ID3D12Resource> m_sceneColor;
        std::unique_ptr<FrameAllocator> m_frameAllocator;
        std::array<FrameContext, kFramesInFlight> m_frames;
        ComPtr<ID3D12GraphicsCommandList7> m_commandList;

        uint32_t m_frameIndex = 0;
        uint32_t m_backBufferIndex = 0;
        uint64_t m_frameCount = 0;
        bool m_vsync = true;
        // The states this frame's barriers have left the targets in (reset by BeginFrame).
        TrackedTexture m_sceneColorState;
        TrackedTexture m_depthState;
        TrackedTexture m_backBufferState;
        D3D12_GPU_VIRTUAL_ADDRESS m_zeroDrawData = 0; // this frame's all-zero b1 block
        ComPtr<ID3D12Resource> m_screenshotReadback; // created on first use
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT m_screenshotFootprint{};
        bool m_screenshotPending = false;
    };
}
