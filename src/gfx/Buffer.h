#pragma once

#include "gfx/D3D12Common.h"

#include <cstdint>
#include <string>

namespace walnut
{
    struct BufferDesc
    {
        uint64_t size = 0;

        // DEFAULT:  GPU-only memory (VRAM on discrete GPUs). Fastest for the GPU; the CPU can't touch it.
        // UPLOAD:   CPU-writable, GPU-readable system memory. Good for data the CPU rewrites often,
        //           or as a staging area to copy into DEFAULT memory.
        // READBACK: GPU-writable, CPU-readable. For getting results back to the CPU.
        D3D12_HEAP_TYPE heapType = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE; // e.g. ALLOW_UNORDERED_ACCESS for UAVs
        std::wstring name;
    };

    // Owns one committed buffer resource. Move-only, like the resource itself.
    //
    // UPLOAD and READBACK buffers are mapped for their whole lifetime ("persistent mapping"). That's
    // allowed in D3D12 and avoids a Map/Unmap per update; the CPU pointer stays valid until destruction.
    class Buffer
    {
    public:
        Buffer() = default;
        Buffer(ID3D12Device10* device, const BufferDesc& desc);
        ~Buffer();

        Buffer(Buffer&& other) noexcept;
        Buffer& operator=(Buffer&& other) noexcept;
        Buffer(const Buffer&) = delete;
        Buffer& operator=(const Buffer&) = delete;

        bool IsValid() const { return m_resource != nullptr; }
        ID3D12Resource* Get() const { return m_resource.Get(); }
        uint64_t GetSize() const { return m_size; }
        D3D12_HEAP_TYPE GetHeapType() const { return m_heapType; }

        // Address of the first byte as seen by the GPU. Used for vertex/index buffer views and root CBVs.
        D3D12_GPU_VIRTUAL_ADDRESS GetGpuAddress() const { return m_resource->GetGPUVirtualAddress(); }

        // CPU pointer to the buffer's memory. Only for UPLOAD and READBACK heaps; nullptr for DEFAULT.
        void* GetMappedData() const { return m_mapped; }

    private:
        void Release();

        ComPtr<ID3D12Resource> m_resource;
        uint64_t m_size = 0;
        D3D12_HEAP_TYPE m_heapType = D3D12_HEAP_TYPE_DEFAULT;
        void* m_mapped = nullptr;
    };
}
