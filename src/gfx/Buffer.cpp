#include "gfx/Buffer.h"

#include "gfx/GpuUtils.h"

#include <utility>

namespace walnut
{
    Buffer::Buffer(ID3D12Device10* device, const BufferDesc& desc)
        : m_size(desc.size), m_heapType(desc.heapType)
    {
        const CD3DX12_HEAP_PROPERTIES heapProps(desc.heapType);
        const CD3DX12_RESOURCE_DESC1 resourceDesc = CD3DX12_RESOURCE_DESC1::Buffer(desc.size, desc.flags);

        // Buffers have no layout in the Enhanced Barriers model, so InitialLayout must be UNDEFINED.
        WR_CHECK(device->CreateCommittedResource3(&heapProps, D3D12_HEAP_FLAG_NONE, &resourceDesc,
                                                  D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr,
                                                  IID_PPV_ARGS(&m_resource)));
        SetDebugName(m_resource.Get(), desc.name.c_str());

        if (desc.heapType == D3D12_HEAP_TYPE_UPLOAD)
        {
            // Empty read range: we promise the CPU won't read this memory. Upload memory is write-combined,
            // so CPU reads from it are extremely slow.
            const CD3DX12_RANGE noRead(0, 0);
            WR_CHECK(m_resource->Map(0, &noRead, &m_mapped));
        }
        else if (desc.heapType == D3D12_HEAP_TYPE_READBACK)
        {
            WR_CHECK(m_resource->Map(0, nullptr, &m_mapped));
        }
    }

    Buffer::~Buffer()
    {
        Release();
    }

    Buffer::Buffer(Buffer&& other) noexcept
        : m_resource(std::move(other.m_resource)),
          m_size(std::exchange(other.m_size, 0)),
          m_heapType(other.m_heapType),
          m_mapped(std::exchange(other.m_mapped, nullptr))
    {
    }

    Buffer& Buffer::operator=(Buffer&& other) noexcept
    {
        if (this != &other)
        {
            Release();
            m_resource = std::move(other.m_resource);
            m_size = std::exchange(other.m_size, 0);
            m_heapType = other.m_heapType;
            m_mapped = std::exchange(other.m_mapped, nullptr);
        }
        return *this;
    }

    void Buffer::Release()
    {
        if (m_mapped && m_resource)
        {
            // The range tells the driver what the CPU wrote: everything for UPLOAD, nothing for READBACK.
            const CD3DX12_RANGE noWrite(0, 0);
            m_resource->Unmap(0, m_heapType == D3D12_HEAP_TYPE_READBACK ? &noWrite : nullptr);
        }
        m_mapped = nullptr;
        m_resource.Reset();
        m_size = 0;
    }
}
