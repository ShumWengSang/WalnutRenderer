#include "gfx/FrameAllocator.h"

#include <format>
#include <stdexcept>

namespace walnut
{
    namespace
    {
        constexpr uint64_t AlignUp(uint64_t value, uint64_t alignment)
        {
            return (value + alignment - 1) & ~(alignment - 1);
        }
    }

    FrameAllocator::FrameAllocator(ID3D12Device10* device, uint32_t framesInFlight, uint64_t bytesPerFrame)
        : m_bytesPerFrame(AlignUp(bytesPerFrame, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT))
    {
        m_buffer = Buffer(device, BufferDesc{
            .size = m_bytesPerFrame * framesInFlight,
            .heapType = D3D12_HEAP_TYPE_UPLOAD,
            .name = L"FrameAllocator",
        });
    }

    void FrameAllocator::BeginFrame(uint32_t frameIndex)
    {
        m_regionStart = m_bytesPerFrame * frameIndex;
        m_offset = m_regionStart;
    }

    FrameAllocation FrameAllocator::Allocate(uint64_t size, uint64_t alignment)
    {
        const uint64_t start = AlignUp(m_offset, alignment);
        if (start + size > m_regionStart + m_bytesPerFrame)
        {
            throw std::runtime_error(std::format("FrameAllocator: out of memory ({} of {} bytes used this frame, {} requested)",
                                                 m_offset - m_regionStart, m_bytesPerFrame, size));
        }
        m_offset = start + size;

        return FrameAllocation{
            .cpu = static_cast<uint8_t*>(m_buffer.GetMappedData()) + start,
            .gpu = m_buffer.GetGpuAddress() + start,
            .size = size,
        };
    }
}
