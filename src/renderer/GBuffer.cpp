#include "renderer/GBuffer.h"

#include "core/Log.h"
#include "gfx/Barriers.h"
#include "gfx/Device.h"
#include "gfx/GpuUtils.h"
#include "gfx/Views.h"

namespace walnut
{
    namespace
    {

        constexpr const wchar_t* kTargetNames[GBuffer::kCount] = {
            L"GBuffer_BaseColor", L"GBuffer_Normals", L"GBuffer_Material", L"GBuffer_Emissive" };
    }

    GBuffer::GBuffer(Device& device, DescriptorHeap& srvHeap, uint32_t width, uint32_t height)
        : m_device(device.Get()), m_srvHeap(srvHeap)
    {
        m_rtvHeap = std::make_unique<DescriptorHeap>(m_device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kCount, false, L"GBufferRtvHeap");
        for (uint32_t i = 0; i < kCount; ++i)
        {
            m_rtvs[i] = OwnedDescriptor(*m_rtvHeap);
            m_rtvHandles[i] = m_rtvs[i].cpu;
            m_srvs[i] = OwnedDescriptor(m_srvHeap);
        }
        CreateTargets(width, height);
    }

    GBuffer::~GBuffer() = default;

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
            const D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = Texture2DSrv(kFormats[i]);
            m_device->CreateShaderResourceView(m_targets[i].Get(), &srvDesc, m_srvs[i].cpu);
        }
        log::Info("G-buffer: {}x{}, {} targets, {:.1f} MB", width, height, static_cast<uint32_t>(kCount),
                  static_cast<double>(m_bytes) / (1024.0 * 1024.0));
    }

    void GBuffer::BeginWrite(ID3D12GraphicsCommandList7* commandList)
    {
        // Last frame's reads (the deferred pass) were in an earlier ExecuteCommandLists: nothing to wait for.
        BarrierBatch barriers;
        for (uint32_t i = 0; i < kCount; ++i)
        {
            m_states[i].Reset(m_targets[i].Get(), access::Undefined);
            m_states[i].Transition(barriers, access::RenderTarget, true /*discard*/);
        }
        barriers.Submit(commandList);
    }

    void GBuffer::EndWrite(ID3D12GraphicsCommandList7* commandList)
    {
        // Wait for the G-buffer pass's writes, make them visible to the fullscreen pass's pixel shader.
        BarrierBatch barriers;
        for (TrackedTexture& state : m_states)
            state.Transition(barriers, access::PixelShaderRead);
        barriers.Submit(commandList);
    }

    DeferredInputs GBuffer::GetDeferredInputs(uint32_t depthSrvIndex) const
    {
        return { .baseColor = GetSrvIndex(BaseColor), .normals = GetSrvIndex(Normals), .material = GetSrvIndex(Material),
                 .emissive = GetSrvIndex(Emissive), .depth = depthSrvIndex };
    }
}
