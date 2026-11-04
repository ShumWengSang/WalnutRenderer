#include "gfx/DescriptorHeap.h"

#include "gfx/GpuUtils.h"

namespace walnut
{
    DescriptorHeap::DescriptorHeap(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t capacity,
                                   bool shaderVisible, const wchar_t* name)
        : m_capacity(capacity), m_shaderVisible(shaderVisible)
    {
        D3D12_DESCRIPTOR_HEAP_DESC desc{};
        desc.Type = type;
        desc.NumDescriptors = capacity;
        desc.Flags = shaderVisible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        WR_CHECK(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&m_heap)));
        SetDebugName(m_heap.Get(), name);

        m_increment = device->GetDescriptorHandleIncrementSize(type);
        m_cpuStart = m_heap->GetCPUDescriptorHandleForHeapStart();
        if (shaderVisible)
            m_gpuStart = m_heap->GetGPUDescriptorHandleForHeapStart();

        // Hand out low indices first.
        m_freeList.reserve(capacity);
        for (uint32_t i = capacity; i > 0; --i)
            m_freeList.push_back(i - 1);
    }

    DescriptorHandle DescriptorHeap::Allocate()
    {
        if (m_freeList.empty())
            throw std::runtime_error("DescriptorHeap is full");
        const uint32_t index = m_freeList.back();
        m_freeList.pop_back();
        return GetHandle(index);
    }

    void DescriptorHeap::Free(const DescriptorHandle& handle)
    {
        if (handle.IsValid())
            m_freeList.push_back(handle.index);
    }

    void DescriptorHeap::Free(D3D12_CPU_DESCRIPTOR_HANDLE cpu)
    {
        const uint32_t index = static_cast<uint32_t>((cpu.ptr - m_cpuStart.ptr) / m_increment);
        m_freeList.push_back(index);
    }

    DescriptorHandle DescriptorHeap::GetHandle(uint32_t index) const
    {
        DescriptorHandle handle;
        handle.index = index;
        handle.cpu.ptr = m_cpuStart.ptr + static_cast<SIZE_T>(index) * m_increment;
        if (m_shaderVisible)
            handle.gpu.ptr = m_gpuStart.ptr + static_cast<UINT64>(index) * m_increment;
        return handle;
    }
}
