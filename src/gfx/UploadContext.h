#pragma once

#include "gfx/Buffer.h"
#include "gfx/D3D12Common.h"

#include <cstdint>
#include <vector>

namespace walnut
{
    class CommandQueue;

    // Gets data into DEFAULT-heap (GPU-only) resources, which the CPU can't write directly:
    //   1. copy the data into a temporary UPLOAD-heap "staging" buffer (CPU memcpy),
    //   2. record a GPU copy from the staging buffer into the destination,
    //   3. Flush(): submit the copies, wait for the GPU, then free the staging buffers.
    //
    // Meant for load-time data. Batch many uploads and call Flush() once.
    class UploadContext
    {
    public:
        UploadContext(ID3D12Device10* device, CommandQueue& queue);
        ~UploadContext();

        UploadContext(const UploadContext&) = delete;
        UploadContext& operator=(const UploadContext&) = delete;

        // Records a copy of 'size' bytes from 'data' into 'destination' at 'destinationOffset'.
        // The data is copied to staging memory immediately, so 'data' may be freed after this returns.
        // The destination must not be used by the GPU until Flush() has returned.
        void UploadBuffer(const Buffer& destination, const void* data, uint64_t size, uint64_t destinationOffset = 0);

        // Submits all recorded copies and blocks until the GPU has finished them. No-op if nothing is pending.
        void Flush();

    private:
        void BeginRecordingIfNeeded();

        ID3D12Device10* m_device = nullptr;
        CommandQueue& m_queue;
        ComPtr<ID3D12CommandAllocator> m_allocator;
        ComPtr<ID3D12GraphicsCommandList7> m_commandList;
        bool m_recording = false;

        // Staging buffers must stay alive until the GPU has executed the copies that read them.
        std::vector<Buffer> m_stagingBuffers;
        uint64_t m_pendingBytes = 0;
    };
}
