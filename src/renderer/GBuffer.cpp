#include "renderer/GBuffer.h"

#include "core/Log.h"
#include "gfx/Device.h"
#include "gfx/GpuUtils.h"

namespace walnut
{
    namespace
    {
        constexpr UINT kAllSubresources = 0xffffffff;
        constexpr const wchar_t* kTargetNames[GBuffer::kCount] = {
            L"GBuffer_BaseColor", L"GBuffer_Normals", L"GBuffer_Material", L"GBuffer_Emissive" };
    }

    GBuffer::GBuffer(Device& device, DescriptorHeap& srvHeap, uint32_t width, uint32_t height)
        : m_device(device.Get()), m_srvHeap(srvHeap)
    {
        m_rtvHeap = std::make_unique<DescriptorHeap>(m_device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kCount, false, L"GBufferRtvHeap");
        for (uint32_t i = 0; i < kCount; ++i)
        {
            m_rtvs[i] = m_rtvHeap->Allocate();
            m_rtvHandles[i] = m_rtvs[i].cpu;
            m_srvs[i] = m_srvHeap.Allocate();
        }
        CreateTargets(width, height);
    }

    GBuffer::~GBuffer()
    {
        for (const DescriptorHandle& srv : m_srvs)
            m_srvHeap.Free(srv);
    }

    void GBuffer::Resize(uint32_t width, uint32_t height)
    {
        CreateTargets(width, height);
    }

    void GBuffer::CreateTargets(uint32_t width, uint32_t height)
    {
        m_bytes = 0;
        const CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);
        for (uint32_t i = 0; i < kCount; ++i)
        {
            m_targets[i].Reset();
            const CD3DX12_RESOURCE_DESC1 desc = CD3DX12_RESOURCE_DESC1::Tex2D(
                kFormats[i], width, height, 1 /*array size*/, 1 /*mip levels*/, 1 /*samples*/, 0,
                D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
            // Never cleared, so no optimized clear value. Created UNDEFINED: every frame starts with a discard.
            WR_CHECK(m_device->CreateCommittedResource3(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
                                                        D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr,
                                                        IID_PPV_ARGS(&m_targets[i])));
            SetDebugName(m_targets[i].Get(), kTargetNames[i]);
            D3D12_RESOURCE_ALLOCATION_INFO1 info{};
            m_bytes += m_device->GetResourceAllocationInfo2(0, 1, &desc, &info).SizeInBytes;

            m_device->CreateRenderTargetView(m_targets[i].Get(), nullptr, m_rtvs[i].cpu);
            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
            srvDesc.Format = kFormats[i];
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Texture2D.MipLevels = 1;
            m_device->CreateShaderResourceView(m_targets[i].Get(), &srvDesc, m_srvs[i].cpu);
        }
        log::Info("G-buffer: {}x{}, {} targets, {:.1f} MB", width, height, static_cast<uint32_t>(kCount),
                  static_cast<double>(m_bytes) / (1024.0 * 1024.0));
    }

    void GBuffer::BeginWrite(ID3D12GraphicsCommandList7* commandList) const
    {
        // Last frame's reads (the deferred pass) were in an earlier ExecuteCommandLists: nothing to wait for.
        std::array<D3D12_TEXTURE_BARRIER, kCount> barriers;
        for (uint32_t i = 0; i < kCount; ++i)
            barriers[i] = CD3DX12_TEXTURE_BARRIER(
                D3D12_BARRIER_SYNC_NONE, D3D12_BARRIER_SYNC_RENDER_TARGET,
                D3D12_BARRIER_ACCESS_NO_ACCESS, D3D12_BARRIER_ACCESS_RENDER_TARGET,
                D3D12_BARRIER_LAYOUT_UNDEFINED, D3D12_BARRIER_LAYOUT_RENDER_TARGET,
                m_targets[i].Get(), CD3DX12_BARRIER_SUBRESOURCE_RANGE(kAllSubresources), D3D12_TEXTURE_BARRIER_FLAG_DISCARD);
        const CD3DX12_BARRIER_GROUP group(kCount, barriers.data());
        commandList->Barrier(1, &group);
    }

    void GBuffer::EndWrite(ID3D12GraphicsCommandList7* commandList) const
    {
        // Wait for the G-buffer pass's writes, make them visible to the fullscreen pass's pixel shader.
        std::array<D3D12_TEXTURE_BARRIER, kCount> barriers;
        for (uint32_t i = 0; i < kCount; ++i)
            barriers[i] = CD3DX12_TEXTURE_BARRIER(
                D3D12_BARRIER_SYNC_RENDER_TARGET, D3D12_BARRIER_SYNC_PIXEL_SHADING,
                D3D12_BARRIER_ACCESS_RENDER_TARGET, D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                D3D12_BARRIER_LAYOUT_RENDER_TARGET, D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE,
                m_targets[i].Get(), CD3DX12_BARRIER_SUBRESOURCE_RANGE(kAllSubresources));
        const CD3DX12_BARRIER_GROUP group(kCount, barriers.data());
        commandList->Barrier(1, &group);
    }
}
