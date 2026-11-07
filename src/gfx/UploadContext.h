#pragma once

#include "gfx/Buffer.h"
#include "gfx/D3D12Common.h"

#include <cstdint>
#include <deque>
#include <span>
#include <vector>

namespace walnut
{
    class CommandQueue;
    class Texture;

    // One subresource (e.g. one mip level) of source pixel data in CPU memory, rows tightly packed or not.
    struct TextureSubresourceData
    {
        const void* data = nullptr;
        uint64_t rowPitch = 0; // bytes from the start of one row to the next in 'data'
    };

    // Gets data into DEFAULT-heap (GPU-only) resources, which the CPU can't write directly:
    //   1. copy the data into staging memory (UPLOAD heap, CPU memcpy),
    //   2. record a GPU copy from staging into the destination,
    //   3. Flush(): submit the copies and wait for the GPU.
    //
    // Staging memory is one permanent, persistently mapped ring buffer:
    //
    //   [ free | batch 1 (in flight) | batch 2 (in flight) | current batch (recording) | free ]
    //            ^ tail                                                                ^ head
    //
    // New staging data is written at 'head'. When the ring is full, the current batch is submitted and
    // the CPU waits for the oldest in-flight batch; its fence proves the GPU finished reading its staging
    // bytes, so 'tail' moves past them and that memory is reused. Uploads bigger than the ring are split
    // into chunks.
    //
    // Meant for load-time data. Batch many uploads and call Flush() once.
    class UploadContext
    {
    public:
        UploadContext(ID3D12Device10* device, CommandQueue& queue, uint64_t ringCapacity);
        ~UploadContext();

        UploadContext(const UploadContext&) = delete;
        UploadContext& operator=(const UploadContext&) = delete;

        // Records a copy of 'size' bytes from 'data' into 'destination' at 'destinationOffset'.
        // The data is copied to staging memory immediately, so 'data' may be freed after this returns.
        // The destination must not be used by the GPU until Flush() has returned.
        void UploadBuffer(const Buffer& destination, const void* data, uint64_t size, uint64_t destinationOffset = 0);

        // Records copies of 'subresources' (mip 0, 1, ... in order) into 'destination', which must be in the
        // COPY_DEST layout, then a barrier that leaves it in SHADER_RESOURCE, ready for shaders to sample.
        // Same rules as UploadBuffer: the data is staged immediately; the texture is usable after Flush().
        void UploadTexture(const Texture& destination, std::span<const TextureSubresourceData> subresources);

        // Submits all recorded copies and blocks until the GPU has finished them. No-op if nothing is pending.
        void Flush();

        uint64_t GetRingCapacity() const { return m_ringCapacity; }

    private:
        struct InFlightBatch
        {
            ComPtr<ID3D12CommandAllocator> allocator; // can't be reset until 'fenceValue' completes
            uint64_t fenceValue = 0;
            uint64_t ringEnd = 0;   // 'tail' moves here when the batch retires
            uint64_t ringBytes = 0; // staging bytes (incl. alignment padding and wrap waste) freed on retire
        };

        void BeginRecordingIfNeeded();
        void SubmitCurrentBatch();
        void RetireOldestBatch(bool wait);
        void RetireCompletedBatches();

        // Returns the ring offset of 'size' contiguous bytes, submitting/waiting if the ring is full.
        uint64_t AllocateStaging(uint64_t size, uint64_t alignment);

        ID3D12Device10* m_device = nullptr;
        CommandQueue& m_queue;
        ComPtr<ID3D12GraphicsCommandList7> m_commandList;
        ComPtr<ID3D12CommandAllocator> m_currentAllocator;
        std::vector<ComPtr<ID3D12CommandAllocator>> m_freeAllocators;
        bool m_recording = false;

        Buffer m_ring;
        uint64_t m_ringCapacity = 0;
        uint64_t m_head = 0;        // next free byte
        uint64_t m_tail = 0;        // oldest byte the GPU may still read
        uint64_t m_used = 0;        // bytes between tail and head (wrapping)
        uint64_t m_batchBytes = 0;  // bytes allocated by the batch being recorded
        std::deque<InFlightBatch> m_inFlight;

        // Statistics for the next Flush() log line.
        uint64_t m_statBytes = 0;
        uint64_t m_statCopies = 0;
        uint64_t m_statBatches = 0;
        uint64_t m_statWaits = 0;
    };
}
