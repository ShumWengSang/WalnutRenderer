#pragma once

#include "gfx/D3D12Common.h"

#include <cstdint>
#include <vector>

namespace walnut
{
    struct DescriptorHandle
    {
        D3D12_CPU_DESCRIPTOR_HANDLE cpu{};
        D3D12_GPU_DESCRIPTOR_HANDLE gpu{}; // only valid for shader-visible heaps
        uint32_t index = UINT32_MAX;

        bool IsValid() const { return index != UINT32_MAX; }
    };

    // Fixed-size descriptor heap with a simple free-list allocator.
    class DescriptorHeap
    {
    public:
        DescriptorHeap(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t capacity,
                       bool shaderVisible, const wchar_t* name);

        DescriptorHeap(const DescriptorHeap&) = delete;
        DescriptorHeap& operator=(const DescriptorHeap&) = delete;

        DescriptorHandle Allocate();
        void Free(const DescriptorHandle& handle);
        void Free(D3D12_CPU_DESCRIPTOR_HANDLE cpu);

        DescriptorHandle GetHandle(uint32_t index) const;

        ID3D12DescriptorHeap* Get() const { return m_heap.Get(); }
        uint32_t GetCapacity() const { return m_capacity; }
        uint32_t GetAllocatedCount() const { return m_capacity - static_cast<uint32_t>(m_freeList.size()); }

    private:
        ComPtr<ID3D12DescriptorHeap> m_heap;
        D3D12_CPU_DESCRIPTOR_HANDLE m_cpuStart{};
        D3D12_GPU_DESCRIPTOR_HANDLE m_gpuStart{};
        uint32_t m_increment = 0;
        uint32_t m_capacity = 0;
        bool m_shaderVisible = false;
        std::vector<uint32_t> m_freeList;
    };
}
