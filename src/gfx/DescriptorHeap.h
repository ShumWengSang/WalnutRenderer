#pragma once

#include "gfx/D3D12Common.h"

#include <cstdint>
#include <utility>
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

    // Fixed-size descriptor heap with a simple free-list allocator. Freeing a slot that isn't allocated (a double
    // free, or a handle from another heap) is reported and ignored, instead of handing the slot out twice.
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
        std::vector<bool> m_allocated; // per slot
    };

    // A descriptor slot that frees itself: move-only, and its destructor returns the slot to the heap (which must
    // outlive it). It is a DescriptorHandle, so .cpu, .gpu and .index read as usual.
    class OwnedDescriptor : public DescriptorHandle
    {
    public:
        OwnedDescriptor() = default;
        explicit OwnedDescriptor(DescriptorHeap& heap)
            : DescriptorHandle(heap.Allocate()), m_heap(&heap) {}
        ~OwnedDescriptor() { Reset(); }

        OwnedDescriptor(OwnedDescriptor&& other) noexcept
            : DescriptorHandle(std::exchange(static_cast<DescriptorHandle&>(other), DescriptorHandle{})),
              m_heap(std::exchange(other.m_heap, nullptr)) {}
        OwnedDescriptor& operator=(OwnedDescriptor&& other) noexcept
        {
            if (this != &other)
            {
                Reset();
                static_cast<DescriptorHandle&>(*this) = std::exchange(static_cast<DescriptorHandle&>(other), DescriptorHandle{});
                m_heap = std::exchange(other.m_heap, nullptr);
            }
            return *this;
        }
        OwnedDescriptor(const OwnedDescriptor&) = delete;
        OwnedDescriptor& operator=(const OwnedDescriptor&) = delete;

        // Frees the slot now (if any).
        void Reset()
        {
            if (m_heap && IsValid())
                m_heap->Free(*this);
            static_cast<DescriptorHandle&>(*this) = DescriptorHandle{};
            m_heap = nullptr;
        }

    private:
        DescriptorHeap* m_heap = nullptr;
    };
}
