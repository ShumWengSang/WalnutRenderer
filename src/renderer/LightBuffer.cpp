#include "renderer/LightBuffer.h"

#include "gfx/Device.h"
#include "gfx/FrameAllocator.h"

#include <algorithm>
#include <cstring>

namespace walnut
{
    LightBuffer::LightBuffer(Device& device, DescriptorHeap& srvHeap, uint32_t framesInFlight)
        : m_device(device.Get()), m_srvHeap(srvHeap)
    {
        for (uint32_t i = 0; i < framesInFlight; ++i)
            m_srvs.push_back(m_srvHeap.Allocate());
    }

    LightBuffer::~LightBuffer()
    {
        for (const DescriptorHandle& srv : m_srvs)
            m_srvHeap.Free(srv);
    }

    uint32_t LightBuffer::Upload(FrameAllocator& allocator, uint32_t frameIndex, std::span<const PointLightGpu> lights)
    {
        constexpr uint64_t kStride = sizeof(PointLightGpu);
        // At least one element: a buffer view can't be empty. The shader loops over FrameConstants.pointLightCount
        // lights, so an unused element is never read.
        const uint64_t count = std::max<uint64_t>(lights.size(), 1);
        // Aligned to the stride: a structured buffer view starts at a whole element (FirstElement = offset / stride).
        const FrameAllocation allocation = allocator.Allocate(count * kStride, kStride);
        if (!lights.empty())
            std::memcpy(allocation.cpu, lights.data(), lights.size_bytes());

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = DXGI_FORMAT_UNKNOWN;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Buffer.FirstElement = allocation.offset / kStride;
        srvDesc.Buffer.NumElements = static_cast<UINT>(count);
        srvDesc.Buffer.StructureByteStride = static_cast<UINT>(kStride);
        const DescriptorHandle& srv = m_srvs[frameIndex];
        m_device->CreateShaderResourceView(allocator.GetResource(), &srvDesc, srv.cpu);
        return srv.index;
    }
}
