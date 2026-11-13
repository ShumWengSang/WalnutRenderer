#include "renderer/ShadowMap.h"

#include "gfx/Barriers.h"
#include "gfx/Device.h"
#include "gfx/GpuUtils.h"
#include "gfx/Views.h"

namespace walnut
{
    namespace
    {
        // Reversed-Z, like the main depth buffer: 1 = nearest to the light, 0 = farthest. Cleared to "nothing here".
        constexpr float kClearDepth = 0.0f;
    }

    ShadowMap::ShadowMap(Device& device, DescriptorHeap& srvHeap, uint32_t size)
        : m_size(size)
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
        m_dsv = OwnedDescriptor(*m_dsvHeap);
        const D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = Texture2DDsv(kDepthFormat);
        d3d->CreateDepthStencilView(m_texture.Get(), &dsvDesc, m_dsv.cpu);

        const D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = Texture2DSrv(kReadFormat); // reads (depth, 0, 0, 1)
        m_srv = OwnedDescriptor(srvHeap);
        d3d->CreateShaderResourceView(m_texture.Get(), &srvDesc, m_srv.cpu);

        // The UI's image shader shows whatever RGB it samples. A one-channel texture would come out red, so this
        // view's component mapping ("swizzle") returns the depth in red, green and blue: a gray image.
        const D3D12_SHADER_RESOURCE_VIEW_DESC debugSrvDesc = Texture2DSrv(kReadFormat, 1, Swizzle::RedAsGray);
        m_debugSrv = OwnedDescriptor(srvHeap);
        d3d->CreateShaderResourceView(m_texture.Get(), &debugSrvDesc, m_debugSrv.cpu);
    }

    ShadowMap::~ShadowMap() = default;

    void ShadowMap::Begin(ID3D12GraphicsCommandList7* commandList) const
    {
        // Last frame's reads (scene lighting, UI) happened in an earlier ExecuteCommandLists, so there's nothing to
        // wait for, and the clear below overwrites everything: UNDEFINED + DISCARD, like the scene color target.
        BarrierBatch().Texture(m_texture.Get(), access::Undefined, access::DepthWrite, true /*discard*/).Submit(commandList);

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
        BarrierBatch().Texture(m_texture.Get(), access::DepthWrite, access::PixelShaderRead).Submit(commandList);
    }
}
