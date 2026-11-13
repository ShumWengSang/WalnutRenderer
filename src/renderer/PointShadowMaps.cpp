#include "renderer/PointShadowMaps.h"

#include "gfx/Barriers.h"
#include "gfx/Device.h"
#include "gfx/GpuUtils.h"
#include "gfx/Views.h"
#include "renderer/ShadowMap.h"

namespace walnut
{
    namespace
    {
        constexpr float kClearDepth = 0.0f; // reversed-Z: 0 = as far as the light reaches, "nothing here"
        constexpr uint32_t kSliceCount = PointShadowMaps::kFaceCount * PointShadowMaps::kMaxShadowedLights;
    }

    PointShadowMaps::PointShadowMaps(Device& device, DescriptorHeap& srvHeap)
        : m_srvHeap(srvHeap)
    {
        ID3D12Device10* d3d = device.Get();
        const CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);
        const CD3DX12_RESOURCE_DESC1 desc = CD3DX12_RESOURCE_DESC1::Tex2D(
            ShadowMap::kResourceFormat, kFaceSize, kFaceSize, static_cast<UINT16>(kSliceCount), 1 /*mip levels*/,
            1 /*samples*/, 0, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
        D3D12_CLEAR_VALUE clearValue{};
        clearValue.Format = ShadowMap::kDepthFormat;
        clearValue.DepthStencil.Depth = kClearDepth;
        WR_CHECK(d3d->CreateCommittedResource3(&heapProps, D3D12_HEAP_FLAG_NONE, &desc, D3D12_BARRIER_LAYOUT_UNDEFINED,
                                               &clearValue, nullptr, 0, nullptr, IID_PPV_ARGS(&m_texture)));
        SetDebugName(m_texture.Get(), L"PointShadowMaps");
        D3D12_RESOURCE_ALLOCATION_INFO1 info{};
        m_bytes = d3d->GetResourceAllocationInfo2(0, 1, &desc, &info).SizeInBytes;

        m_dsvHeap = std::make_unique<DescriptorHeap>(d3d, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, kSliceCount, false, L"PointShadowDsvHeap");
        for (uint32_t slice = 0; slice < kSliceCount; ++slice)
        {
            // A view of ONE slice of the array: the face being rendered.
            const D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = Texture2DArraySliceDsv(ShadowMap::kDepthFormat, slice);
            m_dsvs.push_back(m_dsvHeap->Allocate());
            d3d->CreateDepthStencilView(m_texture.Get(), &dsvDesc, m_dsvs.back().cpu);

            // And of the same slice for the UI, as gray (like ShadowMap's debug view).
            const D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = Texture2DArraySrv(ShadowMap::kReadFormat, slice, 1, Swizzle::RedAsGray);
            m_faceDebugSrvs.push_back(m_srvHeap.Allocate());
            d3d->CreateShaderResourceView(m_texture.Get(), &srvDesc, m_faceDebugSrvs.back().cpu);
        }

        // The same 2D array, seen as cubes: every 6 slices are one cube, in the order +X, -X, +Y, -Y, +Z, -Z.
        const D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = TextureCubeArraySrv(ShadowMap::kReadFormat, kMaxShadowedLights);
        m_cubeArraySrv = m_srvHeap.Allocate();
        d3d->CreateShaderResourceView(m_texture.Get(), &srvDesc, m_cubeArraySrv.cpu);
    }

    PointShadowMaps::~PointShadowMaps()
    {
        for (const DescriptorHandle& srv : m_faceDebugSrvs)
            m_srvHeap.Free(srv);
        m_srvHeap.Free(m_cubeArraySrv);
    }

    DirectX::XMMATRIX PointShadowMaps::GetFaceViewProjection(const DirectX::XMFLOAT3& position, float range, uint32_t face)
    {
        using namespace DirectX;
        // The D3D cube map layout: for each face, the direction it looks and which way is "up" in its image. With
        // these, texel (u, v) of face +X shows direction (1, -(2v - 1), -(2u - 1)), which is what cube sampling expects.
        static const XMFLOAT3 kLook[kFaceCount] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
        static const XMFLOAT3 kUp[kFaceCount] = { { 0, 1, 0 }, { 0, 1, 0 }, { 0, 0, -1 }, { 0, 0, 1 }, { 0, 1, 0 }, { 0, 1, 0 } };
        const XMMATRIX view = XMMatrixLookToLH(XMLoadFloat3(&position), XMLoadFloat3(&kLook[face]), XMLoadFloat3(&kUp[face]));
        // Reversed-Z: passing far as "near" and near as "far" maps the near plane to depth 1 and the range to 0.
        const float nearZ = kNearScale * range;
        const XMMATRIX projection = XMMatrixPerspectiveFovLH(XM_PIDIV2, 1.0f, range, nearZ);
        return XMMatrixMultiply(view, projection);
    }

    void PointShadowMaps::Begin(ID3D12GraphicsCommandList7* commandList) const
    {
        // Last frame's reads were in an earlier ExecuteCommandLists; every used face is cleared before it's drawn.
        BarrierBatch().Texture(m_texture.Get(), access::Undefined, access::DepthWrite, true /*discard*/).Submit(commandList);
    }

    void PointShadowMaps::BeginFace(ID3D12GraphicsCommandList7* commandList, uint32_t slot, uint32_t face) const
    {
        const D3D12_CPU_DESCRIPTOR_HANDLE dsv = m_dsvs[slot * kFaceCount + face].cpu;
        commandList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, kClearDepth, 0, 0, nullptr);
        commandList->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
        const CD3DX12_VIEWPORT viewport(0.0f, 0.0f, static_cast<float>(kFaceSize), static_cast<float>(kFaceSize));
        const CD3DX12_RECT scissor(0, 0, static_cast<LONG>(kFaceSize), static_cast<LONG>(kFaceSize));
        commandList->RSSetViewports(1, &viewport);
        commandList->RSSetScissorRects(1, &scissor);
    }

    void PointShadowMaps::End(ID3D12GraphicsCommandList7* commandList) const
    {
        // Wait for every face's depth writes, then the shading samples the cubes (like ShadowMap::End).
        BarrierBatch().Texture(m_texture.Get(), access::DepthWrite, access::PixelShaderRead).Submit(commandList);
    }
}
