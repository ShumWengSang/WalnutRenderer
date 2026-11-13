#include "renderer/Renderer.h"

#include "core/Log.h"
#include "core/StringUtils.h"
#include "core/Window.h"
#include "gfx/GpuUtils.h"
#include "gfx/Pix.h"
#include "gfx/RootSignature.h"

#include <stb_image_write.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

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
        m_uploadContext = std::make_unique<UploadContext>(device, *m_directQueue, kUploadRingBytes);
        m_swapChain = std::make_unique<SwapChain>(*m_device, *m_directQueue, window.GetHandle(), window.GetWidth(), window.GetHeight());
        m_srvHeap = std::make_unique<DescriptorHeap>(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kSrvHeapCapacity, true, L"SrvHeap");
        m_rootSignature = CreateBindlessRootSignature(device);
        m_frameAllocator = std::make_unique<FrameAllocator>(device, kFramesInFlight, kFrameAllocatorBytesPerFrame);

        // Depth-stencil views, like RTVs, live in a CPU-only (non-shader-visible) heap.
        m_dsvHeap = std::make_unique<DescriptorHeap>(device, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1, false, L"DsvHeap");
        m_dsv = m_dsvHeap->Allocate();
        m_depthSrv = m_srvHeap->Allocate();
        CreateDepthBuffer(m_swapChain->GetWidth(), m_swapChain->GetHeight());
        m_gbuffer = std::make_unique<GBuffer>(*m_device, *m_srvHeap, m_swapChain->GetWidth(), m_swapChain->GetHeight());

        m_rtvHeap = std::make_unique<DescriptorHeap>(device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, false, L"SceneRtvHeap");
        m_sceneRtv = m_rtvHeap->Allocate();
        m_sceneSrv = m_srvHeap->Allocate();
        CreateSceneColorTarget(m_swapChain->GetWidth(), m_swapChain->GetHeight());

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

        // The same fence also covers this slot's region of per-frame upload memory.
        m_frameAllocator->BeginFrame(m_frameIndex);

        WR_CHECK(frame.allocator->Reset());
        WR_CHECK(m_commandList->Reset(frame.allocator.Get(), nullptr));
        PIXBeginEvent(m_commandList.Get(), pix::kColorFrame, L"Frame");

        m_backBufferIndex = m_swapChain->GetCurrentBackBufferIndex();

        PIXScopedEvent(m_commandList.Get(), pix::kColorGpu, L"Clear scene");
        // The scene is rendered into the HDR scene color texture, not the back buffer.
        //
        // UNDEFINED -> RENDER_TARGET with DISCARD: we clear it right away, so its old contents don't matter.
        // LayoutBefore = UNDEFINED is legal whatever layout it was left in, and DISCARD lets the driver skip
        // preserving the old data and just (re)initialize its compression metadata. Last frame's reads of it
        // (the tonemap pass) were in an earlier ExecuteCommandLists, so there's nothing to wait for:
        // SyncBefore = NONE, AccessBefore = NO_ACCESS.
        const CD3DX12_TEXTURE_BARRIER sceneToRenderTarget(
            D3D12_BARRIER_SYNC_NONE, D3D12_BARRIER_SYNC_RENDER_TARGET,
            D3D12_BARRIER_ACCESS_NO_ACCESS, D3D12_BARRIER_ACCESS_RENDER_TARGET,
            D3D12_BARRIER_LAYOUT_UNDEFINED, D3D12_BARRIER_LAYOUT_RENDER_TARGET,
            m_sceneColor.Get(), CD3DX12_BARRIER_SUBRESOURCE_RANGE(kAllSubresources), D3D12_TEXTURE_BARRIER_FLAG_DISCARD);
        const CD3DX12_BARRIER_GROUP sceneGroup(1, &sceneToRenderTarget);
        m_commandList->Barrier(1, &sceneGroup);

        m_commandList->ClearRenderTargetView(m_sceneRtv.cpu, clearColor, 0, nullptr);

        // The depth buffer: deferred shading leaves it shader-readable at the end of a frame, forward leaves it
        // DEPTH_STENCIL_WRITE. Either way it's cleared now, so UNDEFINED -> DEPTH_STENCIL_WRITE with DISCARD
        // works from both without tracking which: like the scene color above.
        const CD3DX12_TEXTURE_BARRIER depthToWrite(
            D3D12_BARRIER_SYNC_NONE, D3D12_BARRIER_SYNC_DEPTH_STENCIL,
            D3D12_BARRIER_ACCESS_NO_ACCESS, D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE,
            D3D12_BARRIER_LAYOUT_UNDEFINED, D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE,
            m_depthBuffer.Get(), CD3DX12_BARRIER_SUBRESOURCE_RANGE(kAllSubresources), D3D12_TEXTURE_BARRIER_FLAG_DISCARD);
        const CD3DX12_BARRIER_GROUP depthGroup(1, &depthToWrite);
        m_commandList->Barrier(1, &depthGroup);
        m_depthReadable = false;
        m_commandList->ClearDepthStencilView(m_dsv.cpu, D3D12_CLEAR_FLAG_DEPTH, kDepthClearValue, 0, 0, nullptr);

        BindBindlessState(m_commandList.Get());
    }

    void Renderer::BeginDepthPrepass()
    {
        // Depth only: no render target bound, matching the depth-only PSOs (NumRenderTargets = 0).
        m_commandList->OMSetRenderTargets(0, nullptr, FALSE, &m_dsv.cpu);
        SetWindowViewport();
    }

    void Renderer::BeginScenePass()
    {
        // A separate step from BeginFrame because other passes (the shadow map) render to their own targets first.
        m_commandList->OMSetRenderTargets(1, &m_sceneRtv.cpu, FALSE, &m_dsv.cpu);
        SetWindowViewport();
    }

    void Renderer::BeginGBufferPass()
    {
        m_gbuffer->BeginWrite(m_commandList.Get());
        // Four render targets at once ("MRT"): the pixel shader's SV_Target0..3 go to these, in this order.
        const auto& rtvs = m_gbuffer->GetRtvs();
        m_commandList->OMSetRenderTargets(static_cast<UINT>(rtvs.size()), rtvs.data(), FALSE, &m_dsv.cpu);
        SetWindowViewport();
    }

    void Renderer::BeginDeferredPass()
    {
        m_gbuffer->EndWrite(m_commandList.Get());
        // The depth buffer too: the fullscreen pass reads it as a texture. Wait for the depth tests and writes of
        // the pre-pass and G-buffer pass (SyncBefore = DEPTH_STENCIL) before pixel shaders read it.
        const CD3DX12_TEXTURE_BARRIER depthToRead(
            D3D12_BARRIER_SYNC_DEPTH_STENCIL, D3D12_BARRIER_SYNC_PIXEL_SHADING,
            D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE, D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
            D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE, D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE,
            m_depthBuffer.Get(), CD3DX12_BARRIER_SUBRESOURCE_RANGE(kAllSubresources));
        const CD3DX12_BARRIER_GROUP depthGroup(1, &depthToRead);
        m_commandList->Barrier(1, &depthGroup);
        m_depthReadable = true;

        // Only the scene color: no depth buffer bound (a 2D pass, and the depth buffer is being read).
        m_commandList->OMSetRenderTargets(1, &m_sceneRtv.cpu, FALSE, nullptr);
        SetWindowViewport();
    }

    void Renderer::BeginOverlayPass()
    {
        if (m_depthReadable)
        {
            // The deferred pass read the depth buffer; wait for that, then make it a depth target again.
            const CD3DX12_TEXTURE_BARRIER depthToWrite(
                D3D12_BARRIER_SYNC_PIXEL_SHADING, D3D12_BARRIER_SYNC_DEPTH_STENCIL,
                D3D12_BARRIER_ACCESS_SHADER_RESOURCE, D3D12_BARRIER_ACCESS_DEPTH_STENCIL_READ,
                D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE, D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE,
                m_depthBuffer.Get(), CD3DX12_BARRIER_SUBRESOURCE_RANGE(kAllSubresources));
            const CD3DX12_BARRIER_GROUP depthGroup(1, &depthToWrite);
            m_commandList->Barrier(1, &depthGroup);
            m_depthReadable = false;
        }
        m_commandList->OMSetRenderTargets(1, &m_sceneRtv.cpu, FALSE, &m_dsv.cpu);
        SetWindowViewport();
    }

    void Renderer::SetWindowViewport()
    {
        const CD3DX12_VIEWPORT viewport(0.0f, 0.0f, static_cast<float>(m_swapChain->GetWidth()),
                                        static_cast<float>(m_swapChain->GetHeight()));
        const CD3DX12_RECT scissor(0, 0, static_cast<LONG>(m_swapChain->GetWidth()),
                                   static_cast<LONG>(m_swapChain->GetHeight()));
        m_commandList->RSSetViewports(1, &viewport);
        m_commandList->RSSetScissorRects(1, &scissor);
    }

    void Renderer::SetFrameConstants(const FrameConstants& constants)
    {
        // Root arguments stay bound across PSO changes (they belong to the root signature), so this one call
        // covers every pass that uses the shared root signature this frame.
        const D3D12_GPU_VIRTUAL_ADDRESS address = m_frameAllocator->AllocateConstants(constants);
        m_commandList->SetGraphicsRootConstantBufferView(RootParam::FrameData, address);
        m_commandList->SetComputeRootConstantBufferView(RootParam::FrameData, address);
    }

    void Renderer::BeginBackBufferPass()
    {
        // Two transitions in one Barrier() call, so the driver can batch them:
        D3D12_TEXTURE_BARRIER barriers[2] = {
            // 1. Scene color: stop being a render target, become readable by pixel shaders. Waits for all scene
            //    drawing (SyncBefore = RENDER_TARGET) and makes those writes visible to texture reads
            //    (AccessAfter = SHADER_RESOURCE). DIRECT_QUEUE_SHADER_RESOURCE: only the direct queue reads it,
            //    which can be faster than the generic SHADER_RESOURCE layout.
            CD3DX12_TEXTURE_BARRIER(
                D3D12_BARRIER_SYNC_RENDER_TARGET, D3D12_BARRIER_SYNC_PIXEL_SHADING,
                D3D12_BARRIER_ACCESS_RENDER_TARGET, D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                D3D12_BARRIER_LAYOUT_RENDER_TARGET, D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE,
                m_sceneColor.Get(), CD3DX12_BARRIER_SUBRESOURCE_RANGE(kAllSubresources)),
            // 2. Back buffer: PRESENT -> RENDER_TARGET. Nothing earlier in this command list touched it, and
            //    ExecuteCommandLists boundaries already finish all prior GPU work (including the previous
            //    Present), so there is nothing to wait for: SyncBefore = NONE, AccessBefore = NO_ACCESS.
            CD3DX12_TEXTURE_BARRIER(
                D3D12_BARRIER_SYNC_NONE, D3D12_BARRIER_SYNC_RENDER_TARGET,
                D3D12_BARRIER_ACCESS_NO_ACCESS, D3D12_BARRIER_ACCESS_RENDER_TARGET,
                D3D12_BARRIER_LAYOUT_PRESENT, D3D12_BARRIER_LAYOUT_RENDER_TARGET,
                m_swapChain->GetBackBuffer(m_backBufferIndex), CD3DX12_BARRIER_SUBRESOURCE_RANGE(kAllSubresources)),
        };
        const CD3DX12_BARRIER_GROUP group(_countof(barriers), barriers);
        m_commandList->Barrier(1, &group);

        // No clear: the fullscreen pass writes every pixel. No depth buffer: 2D passes don't need one.
        // The _SRGB view: the tonemap pass outputs linear colors, and the hardware encodes them to sRGB on write.
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_swapChain->GetSrgbRtv(m_backBufferIndex);
        m_commandList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    }

    void Renderer::BeginUiPass()
    {
        // Same back buffer, plain UNORM view: ImGui's colors are already sRGB, so they must be written unchanged.
        // Switching views of the same texture needs no barrier; the layout and contents don't change.
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_swapChain->GetRtv(m_backBufferIndex);
        m_commandList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    }

    void Renderer::CaptureBackBuffer()
    {
        ID3D12Resource* backBuffer = m_swapChain->GetBackBuffer(m_backBufferIndex);
        const D3D12_RESOURCE_DESC desc = backBuffer->GetDesc();
        UINT64 bytes = 0;
        m_device->Get()->GetCopyableFootprints(&desc, 0, 1, 0, &m_screenshotFootprint, nullptr, nullptr, &bytes);
        if (!m_screenshotReadback || m_screenshotReadback->GetDesc().Width < bytes)
        {
            const CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_READBACK);
            const CD3DX12_RESOURCE_DESC1 bufferDesc = CD3DX12_RESOURCE_DESC1::Buffer(bytes);
            m_screenshotReadback.Reset();
            WR_CHECK(m_device->Get()->CreateCommittedResource3(&heapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc,
                                                               D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr,
                                                               IID_PPV_ARGS(&m_screenshotReadback)));
            SetDebugName(m_screenshotReadback.Get(), L"ScreenshotReadback");
        }

        // RENDER_TARGET -> COPY_SOURCE, copy, and back: the UI is drawn into the same back buffer afterwards.
        const auto transition = [&](D3D12_BARRIER_SYNC syncBefore, D3D12_BARRIER_SYNC syncAfter,
                                    D3D12_BARRIER_ACCESS accessBefore, D3D12_BARRIER_ACCESS accessAfter,
                                    D3D12_BARRIER_LAYOUT layoutBefore, D3D12_BARRIER_LAYOUT layoutAfter) {
            const CD3DX12_TEXTURE_BARRIER barrier(syncBefore, syncAfter, accessBefore, accessAfter, layoutBefore, layoutAfter,
                                                  backBuffer, CD3DX12_BARRIER_SUBRESOURCE_RANGE(kAllSubresources));
            const CD3DX12_BARRIER_GROUP group(1, &barrier);
            m_commandList->Barrier(1, &group);
        };
        transition(D3D12_BARRIER_SYNC_RENDER_TARGET, D3D12_BARRIER_SYNC_COPY, D3D12_BARRIER_ACCESS_RENDER_TARGET,
                   D3D12_BARRIER_ACCESS_COPY_SOURCE, D3D12_BARRIER_LAYOUT_RENDER_TARGET, D3D12_BARRIER_LAYOUT_COPY_SOURCE);
        const CD3DX12_TEXTURE_COPY_LOCATION destination(m_screenshotReadback.Get(), m_screenshotFootprint);
        const CD3DX12_TEXTURE_COPY_LOCATION source(backBuffer, 0);
        m_commandList->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        transition(D3D12_BARRIER_SYNC_COPY, D3D12_BARRIER_SYNC_RENDER_TARGET, D3D12_BARRIER_ACCESS_COPY_SOURCE,
                   D3D12_BARRIER_ACCESS_RENDER_TARGET, D3D12_BARRIER_LAYOUT_COPY_SOURCE, D3D12_BARRIER_LAYOUT_RENDER_TARGET);
        m_screenshotPending = true;
    }

    bool Renderer::SaveScreenshot(const std::filesystem::path& path)
    {
        if (!m_screenshotPending)
            return false;
        WaitForGpu();
        m_screenshotPending = false;

        const uint32_t width = m_screenshotFootprint.Footprint.Width;
        const uint32_t height = m_screenshotFootprint.Footprint.Height;
        const uint32_t pitch = m_screenshotFootprint.Footprint.RowPitch;
        void* mapped = nullptr;
        const D3D12_RANGE readRange{ 0, static_cast<SIZE_T>(pitch) * height };
        WR_CHECK(m_screenshotReadback->Map(0, &readRange, &mapped));
        // The back buffer is RGBA8 holding sRGB-encoded values (the tonemap wrote through the _SRGB view): exactly
        // what a PNG stores. Alpha is forced opaque.
        std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4);
        for (uint32_t y = 0; y < height; ++y)
        {
            const uint8_t* row = static_cast<const uint8_t*>(mapped) + static_cast<size_t>(y) * pitch;
            uint8_t* out = pixels.data() + static_cast<size_t>(y) * width * 4;
            std::memcpy(out, row, static_cast<size_t>(width) * 4);
            for (uint32_t x = 0; x < width; ++x)
                out[x * 4 + 3] = 255;
        }
        const D3D12_RANGE noWrite{ 0, 0 };
        m_screenshotReadback->Unmap(0, &noWrite);

        std::vector<uint8_t> png;
        stbi_write_png_to_func([](void* context, void* data, int size) {
            auto* bytes = static_cast<std::vector<uint8_t>*>(context);
            bytes->insert(bytes->end(), static_cast<uint8_t*>(data), static_cast<uint8_t*>(data) + size);
        }, &png, static_cast<int>(width), static_cast<int>(height), 4, pixels.data(), static_cast<int>(width) * 4);
        if (png.empty())
            return false;
        if (path.has_parent_path())
            std::filesystem::create_directories(path.parent_path());
        std::ofstream file(path, std::ios::binary);
        file.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
        const bool ok = file.good();
        log::Info("Screenshot {}x{} -> {}{}", width, height, ToUtf8(path.wstring()), ok ? "" : " (write failed)");
        return ok;
    }

    void Renderer::BindBindlessState(ID3D12GraphicsCommandList* commandList) const
    {
        // Order matters: with CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED, the descriptor heap must already be set when the
        // root signature is set. Setting a root signature also clears all root arguments, so passes set their
        // root constants and CBVs afterwards.
        ID3D12DescriptorHeap* heaps[] = { m_srvHeap->Get() };
        commandList->SetDescriptorHeaps(_countof(heaps), heaps);
        commandList->SetGraphicsRootSignature(m_rootSignature.Get());
        // The same root signature for compute shaders: compute has its own set of root arguments.
        commandList->SetComputeRootSignature(m_rootSignature.Get());
    }

    void Renderer::EndFrame()
    {
        ID3D12Resource* backBuffer = m_swapChain->GetBackBuffer(m_backBufferIndex);
        // RENDER_TARGET -> PRESENT (= COMMON, the layout Present requires). Wait for every render-target
        // write before it (tonemap, ImGui). Nothing later in this list uses the buffer, so
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
        // Both must always match the back buffer size.
        CreateDepthBuffer(width, height);
        CreateSceneColorTarget(width, height);
        m_gbuffer->Resize(width, height);
        log::Info("Swapchain resized to {}x{}", width, height);
    }

    void Renderer::CreateSceneColorTarget(uint32_t width, uint32_t height)
    {
        m_sceneColor.Reset();

        // 16-bit float per channel: values above 1.0 (bright lights, sun) are stored instead of clipped, so
        // the tonemap pass can later decide how to squeeze them into the displayable 0..1 range.
        const CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);
        const CD3DX12_RESOURCE_DESC1 desc = CD3DX12_RESOURCE_DESC1::Tex2D(
            kSceneColorFormat, width, height, 1 /*array size*/, 1 /*mip levels*/, 1 /*samples*/, 0,
            D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);

        // The optimized clear value: clearing to exactly this color is a fast metadata-only clear on most GPUs.
        // It's the default clear color; when the UI changes the clear color, clears still work but take the
        // slower path (the debug layer's warning 820 about that is filtered in Device::ConfigureInfoQueue).
        // InitialLayout UNDEFINED: every frame starts with an UNDEFINED -> RENDER_TARGET discard barrier anyway.
        D3D12_CLEAR_VALUE clearValue{};
        clearValue.Format = kSceneColorFormat;
        std::copy(std::begin(kDefaultClearColor), std::end(kDefaultClearColor), clearValue.Color);
        WR_CHECK(m_device->Get()->CreateCommittedResource3(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
                                                           D3D12_BARRIER_LAYOUT_UNDEFINED, &clearValue,
                                                           nullptr, 0, nullptr, IID_PPV_ARGS(&m_sceneColor)));
        SetDebugName(m_sceneColor.Get(), L"SceneColor");

        // Two views of the same texture: an RTV to draw into it, and an SRV (in the bindless heap) to read it.
        m_device->Get()->CreateRenderTargetView(m_sceneColor.Get(), nullptr, m_sceneRtv.cpu);

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = kSceneColorFormat;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels = 1;
        m_device->Get()->CreateShaderResourceView(m_sceneColor.Get(), &srvDesc, m_sceneSrv.cpu);
    }

    void Renderer::CreateDepthBuffer(uint32_t width, uint32_t height)
    {
        m_depthBuffer.Reset();

        // Typeless, so it has two views: D32_FLOAT as a depth buffer, R32_FLOAT as a texture (deferred shading
        // reads it to rebuild each pixel's position). Readable depth can cost a little: without
        // DENY_SHADER_RESOURCE the driver must keep the compression something a shader can decode.
        const CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);
        const CD3DX12_RESOURCE_DESC1 desc = CD3DX12_RESOURCE_DESC1::Tex2D(
            kDepthResourceFormat, width, height, 1 /*array size*/, 1 /*mip levels*/, 1 /*samples*/, 0,
            D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);

        // The "optimized clear value": clearing to exactly this value is a fast metadata-only operation on
        // most GPUs. Clearing to anything else still works, but the debug layer warns that it's slower.
        D3D12_CLEAR_VALUE clearValue{};
        clearValue.Format = kDepthFormat;
        clearValue.DepthStencil.Depth = kDepthClearValue;

        // Created in the layout every frame starts with.
        WR_CHECK(m_device->Get()->CreateCommittedResource3(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
                                                           D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE, &clearValue,
                                                           nullptr, 0, nullptr, IID_PPV_ARGS(&m_depthBuffer)));
        SetDebugName(m_depthBuffer.Get(), L"DepthBuffer");

        D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
        dsvDesc.Format = kDepthFormat;
        dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        m_device->Get()->CreateDepthStencilView(m_depthBuffer.Get(), &dsvDesc, m_dsv.cpu);

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = kDepthSrvFormat;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels = 1;
        m_device->Get()->CreateShaderResourceView(m_depthBuffer.Get(), &srvDesc, m_depthSrv.cpu);
    }
}
