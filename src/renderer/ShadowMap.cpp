#include "renderer/ShadowMap.h"

#include "gfx/Device.h"
#include "gfx/GpuUtils.h"

namespace walnut
{
    namespace
    {
        // Reversed-Z, like the main depth buffer: 1 = nearest to the light, 0 = farthest. Cleared to "nothing here".
        constexpr float kClearDepth = 0.0f;
    }

    ShadowMap::ShadowMap(Device& device, DescriptorHeap& srvHeap, uint32_t size)
        : m_srvHeap(srvHeap), m_size(size)
    {
        ID3D12Device10* d3d = device.Get();

        // No DENY_SHADER_RESOURCE: this depth texture is meant to be sampled.
        const CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);
        const CD3DX12_RESOURCE_DESC1 desc = CD3DX12_RESOURCE_DESC1::Tex2D(
            kResourceFormat, size, size, 1 /*array size*/, 1 /*mip levels*/, 1 /*samples*/, 0,
            D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
        D3D12_CLEAR_VALUE clearValue{};
        clearValue.Format = kDepthFormat; // must be the format the DSV uses, not the TYPELESS one
        clearValue.DepthStencil.Depth = kClearDepth;
        // UNDEFINED: every frame starts with an UNDEFINED -> DEPTH_STENCIL_WRITE discard barrier (see Begin).
        WR_CHECK(d3d->CreateCommittedResource3(&heapProps, D3D12_HEAP_FLAG_NONE, &desc, D3D12_BARRIER_LAYOUT_UNDEFINED,
                                               &clearValue, nullptr, 0, nullptr, IID_PPV_ARGS(&m_texture)));
        SetDebugName(m_texture.Get(), L"SunShadowMap");

        m_dsvHeap = std::make_unique<DescriptorHeap>(d3d, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1, false, L"ShadowDsvHeap");
        m_dsv = m_dsvHeap->Allocate();
        D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
        dsvDesc.Format = kDepthFormat;
        dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        d3d->CreateDepthStencilView(m_texture.Get(), &dsvDesc, m_dsv.cpu);

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = kReadFormat;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; // (depth, 0, 0, 1)
        srvDesc.Texture2D.MipLevels = 1;
        m_srv = srvHeap.Allocate();
        d3d->CreateShaderResourceView(m_texture.Get(), &srvDesc, m_srv.cpu);

        // The UI's image shader shows whatever RGB it samples. A one-channel texture would come out red, so this
        // view's component mapping ("swizzle") returns the depth in red, green and blue: a gray image. The
        // mapping is applied by the texture unit when it reads, so no shader has to know.
        srvDesc.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(
            D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0, D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0,
            D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0, D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1);
        m_debugSrv = srvHeap.Allocate();
        d3d->CreateShaderResourceView(m_texture.Get(), &srvDesc, m_debugSrv.cpu);
    }

    ShadowMap::~ShadowMap()
    {
        m_srvHeap.Free(m_srv);
        m_srvHeap.Free(m_debugSrv);
    }

    void ShadowMap::Begin(ID3D12GraphicsCommandList7* commandList) const
    {
        // Last frame's reads (scene lighting, UI) happened in an earlier ExecuteCommandLists, so there's nothing to
        // wait for, and the clear below overwrites everything: UNDEFINED + DISCARD, like the scene color target.
        const CD3DX12_TEXTURE_BARRIER toDepthWrite(
            D3D12_BARRIER_SYNC_NONE, D3D12_BARRIER_SYNC_DEPTH_STENCIL,
            D3D12_BARRIER_ACCESS_NO_ACCESS, D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE,
            D3D12_BARRIER_LAYOUT_UNDEFINED, D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE,
            m_texture.Get(), CD3DX12_BARRIER_SUBRESOURCE_RANGE(0xFFFFFFFF), D3D12_TEXTURE_BARRIER_FLAG_DISCARD);
        const CD3DX12_BARRIER_GROUP group(1, &toDepthWrite);
        commandList->Barrier(1, &group);

        commandList->ClearDepthStencilView(m_dsv.cpu, D3D12_CLEAR_FLAG_DEPTH, kClearDepth, 0, 0, nullptr);
        // Depth only: no render targets at all. The pipeline has no pixel shader either.
        commandList->OMSetRenderTargets(0, nullptr, FALSE, &m_dsv.cpu);

        const CD3DX12_VIEWPORT viewport(0.0f, 0.0f, static_cast<float>(m_size), static_cast<float>(m_size));
        const CD3DX12_RECT scissor(0, 0, static_cast<LONG>(m_size), static_cast<LONG>(m_size));
        commandList->RSSetViewports(1, &viewport);
        commandList->RSSetScissorRects(1, &scissor);
    }

    void ShadowMap::End(ID3D12GraphicsCommandList7* commandList) const
    {
        // Wait for every depth write (SyncBefore = DEPTH_STENCIL), make them visible to texture reads in pixel
        // shaders (AccessAfter = SHADER_RESOURCE). DIRECT_QUEUE_SHADER_RESOURCE: only this queue reads it.
        const CD3DX12_TEXTURE_BARRIER toShaderResource(
            D3D12_BARRIER_SYNC_DEPTH_STENCIL, D3D12_BARRIER_SYNC_PIXEL_SHADING,
            D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE, D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
            D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE, D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE,
            m_texture.Get(), CD3DX12_BARRIER_SUBRESOURCE_RANGE(0xFFFFFFFF));
        const CD3DX12_BARRIER_GROUP group(1, &toShaderResource);
        commandList->Barrier(1, &group);
    }
}
