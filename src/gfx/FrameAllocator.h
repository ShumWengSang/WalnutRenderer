#pragma once

#include "gfx/Buffer.h"
#include "gfx/D3D12Common.h"

#include <cstdint>
#include <cstring>
#include <type_traits>

namespace walnut
{
    struct FrameAllocation
    {
        void* cpu = nullptr;                    // where the CPU writes (write-combined: write once, never read)
        D3D12_GPU_VIRTUAL_ADDRESS gpu = 0;      // what the GPU reads, e.g. passed to SetGraphicsRootConstantBufferView
        uint64_t size = 0;
    };

    // Short-lived, CPU-written GPU memory for one frame: constants, small dynamic data.
    //
    // One persistently mapped UPLOAD buffer split into one region per frame in flight:
    //
    //   [ region 0 (frame slot 0) | region 1 (frame slot 1) ]
    //
    // Allocation is a "bump pointer": hand out the next aligned slice of the current region. Nothing is freed
    // individually; BeginFrame() resets the whole region at once. That is only safe because the Renderer
    // waits on the slot's fence first, so the GPU has finished reading everything from that region.
    class FrameAllocator
    {
    public:
        FrameAllocator(ID3D12Device10* device, uint32_t framesInFlight, uint64_t bytesPerFrame);

        // Switches to the region of 'frameIndex' and empties it. Call after waiting on that frame's fence.
        void BeginFrame(uint32_t frameIndex);

        // Throws if the frame's region is full.
        FrameAllocation Allocate(uint64_t size, uint64_t alignment);

        // Copies 'data' into a new allocation aligned for use as a constant buffer (256 bytes).
        template <typename T>
        D3D12_GPU_VIRTUAL_ADDRESS AllocateConstants(const T& data)
        {
            static_assert(std::is_trivially_copyable_v<T>);
            const FrameAllocation allocation = Allocate(sizeof(T), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
            std::memcpy(allocation.cpu, &data, sizeof(T));
            return allocation.gpu;
        }

        uint64_t GetUsedBytes() const { return m_offset - m_regionStart; }
        uint64_t GetBytesPerFrame() const { return m_bytesPerFrame; }

    private:
        Buffer m_buffer;
        uint64_t m_bytesPerFrame = 0;
        uint64_t m_regionStart = 0;
        uint64_t m_offset = 0;
    };
}
