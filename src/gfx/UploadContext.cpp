#include "gfx/UploadContext.h"

#include "core/Log.h"
#include "core/Timer.h"
#include "gfx/CommandQueue.h"
#include "gfx/GpuUtils.h"
#include "gfx/Pix.h"
#include "gfx/Texture.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace walnut
{
    namespace
    {
        // CopyBufferRegion has no alignment rule, but aligned staging keeps memcpy fast.
        constexpr uint64_t kBufferStagingAlignment = 16;

        constexpr uint64_t AlignUp(uint64_t value, uint64_t alignment)
        {
            return (value + alignment - 1) & ~(alignment - 1);
        }
    }

    UploadContext::UploadContext(ID3D12Device10* device, CommandQueue& queue, uint64_t ringCapacity)
        : m_device(device), m_queue(queue), m_ringCapacity(ringCapacity)
    {
        m_ring = Buffer(device, BufferDesc{
            .size = ringCapacity,
            .heapType = D3D12_HEAP_TYPE_UPLOAD,
            .name = L"UploadStagingRing",
        });

        // Copies are recorded on a DIRECT list and run on the main queue. A dedicated COPY queue can
        // overlap uploads with rendering, but needs cross-queue synchronization; that comes later.
        WR_CHECK(device->CreateCommandList1(0, D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_LIST_FLAG_NONE,
                                            IID_PPV_ARGS(&m_commandList)));
        SetDebugName(m_commandList.Get(), L"UploadCommandList");
    }

    UploadContext::~UploadContext()
    {
        if (m_recording)
            log::Warn("UploadContext destroyed with unflushed uploads; they were never executed");

        // Batches submitted because the ring filled up may still be reading the ring. Don't free it under the GPU.
        try
        {
            for (const InFlightBatch& batch : m_inFlight)
                m_queue.WaitForFence(batch.fenceValue);
        }
        catch (...)
        {
        }
    }

    void UploadContext::BeginRecordingIfNeeded()
    {
        if (m_recording)
            return;

        RetireCompletedBatches();

        // Each in-flight batch keeps its allocator until its fence completes, so reuse a retired one or make a new one.
        if (!m_freeAllocators.empty())
        {
            m_currentAllocator = std::move(m_freeAllocators.back());
            m_freeAllocators.pop_back();
        }
        else
        {
            WR_CHECK(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_currentAllocator)));
            SetDebugName(m_currentAllocator.Get(), L"UploadAllocator");
        }

        WR_CHECK(m_currentAllocator->Reset());
        WR_CHECK(m_commandList->Reset(m_currentAllocator.Get(), nullptr));
        PIXBeginEvent(m_commandList.Get(), pix::kColorGpu, L"Upload");
        m_recording = true;
    }

    void UploadContext::SubmitCurrentBatch()
    {
        if (!m_recording)
            return;

        PIXEndEvent(m_commandList.Get());
        WR_CHECK(m_commandList->Close());
        m_queue.Execute(m_commandList.Get());

        m_inFlight.push_back(InFlightBatch{
            .allocator = std::move(m_currentAllocator),
            .fenceValue = m_queue.Signal(),
            .ringEnd = m_head,
            .ringBytes = m_batchBytes,
        });
        m_batchBytes = 0;
        m_recording = false;
        ++m_statBatches;
    }

    void UploadContext::RetireOldestBatch(bool wait)
    {
        InFlightBatch& batch = m_inFlight.front();
        if (wait)
            m_queue.WaitForFence(batch.fenceValue);

        // The GPU finished every copy in this batch, so its staging bytes are free again.
        m_tail = batch.ringEnd;
        m_used -= batch.ringBytes;
        m_freeAllocators.push_back(std::move(batch.allocator));
        m_inFlight.pop_front();

        // An empty ring can restart at offset 0, which avoids needless wrap-around.
        if (m_used == 0)
            m_head = m_tail = 0;
    }

    void UploadContext::RetireCompletedBatches()
    {
        while (!m_inFlight.empty() && m_queue.IsFenceComplete(m_inFlight.front().fenceValue))
            RetireOldestBatch(false);
    }

    uint64_t UploadContext::AllocateStaging(uint64_t size, uint64_t alignment)
    {
        if (size > m_ringCapacity)
            throw std::logic_error("UploadContext: staging allocation larger than the ring (chunking failed)");

        for (;;)
        {
            const uint64_t offset = AlignUp(m_head, alignment);
            if (offset + size <= m_ringCapacity)
            {
                // Fits before the end of the ring. 'm_used' also guarantees it doesn't run into the tail.
                const uint64_t needed = (offset - m_head) + size;
                if (m_used + needed <= m_ringCapacity)
                {
                    m_head = offset + size;
                    m_used += needed;
                    m_batchBytes += needed;
                    return offset;
                }
            }
            else
            {
                // Doesn't fit at the end: wrap to offset 0. The skipped bytes at the end count as used until
                // the tail passes them.
                const uint64_t waste = m_ringCapacity - m_head;
                if (m_used + waste + size <= m_ringCapacity)
                {
                    m_used += waste;
                    m_batchBytes += waste;
                    m_head = 0;
                    continue;
                }
            }

            // The ring is full. Free space by retiring the oldest batch. If nothing is in flight, everything in
            // the ring belongs to the batch being recorded, so submit it first so that it can retire.
            if (m_inFlight.empty())
            {
                if (!m_recording)
                    throw std::logic_error("UploadContext: ring accounting error");
                SubmitCurrentBatch();
            }
            ++m_statWaits;
            RetireOldestBatch(true);
        }
    }

    void UploadContext::UploadBuffer(const Buffer& destination, const void* data, uint64_t size, uint64_t destinationOffset)
    {
        if (size == 0)
            return;
        if (destinationOffset + size > destination.GetSize())
            throw std::out_of_range("UploadContext::UploadBuffer: write past the end of the destination buffer");

        // Chunks of at most half the ring, so one batch can be copying on the GPU while the next is being filled.
        const uint64_t maxChunk = m_ringCapacity / 2;
        const auto* source = static_cast<const uint8_t*>(data);
        auto* ring = static_cast<uint8_t*>(m_ring.GetMappedData());

        for (uint64_t done = 0; done < size;)
        {
            const uint64_t chunk = std::min(size - done, maxChunk);
            const uint64_t ringOffset = AllocateStaging(chunk, kBufferStagingAlignment);
            BeginRecordingIfNeeded();

            // CPU: write into staging. GPU: copy staging -> destination.
            //
            // No barrier is needed before or after this copy. Buffers have no layout, and every
            // ExecuteCommandLists boundary completes all prior work and flushes caches. So:
            //  - the destination's first use in this command list (as a copy destination) needs no barrier, and
            //  - the draws that read it run in a later ExecuteCommandLists and see the finished data.
            std::memcpy(ring + ringOffset, source + done, chunk);
            m_commandList->CopyBufferRegion(destination.Get(), destinationOffset + done, m_ring.Get(), ringOffset, chunk);

            done += chunk;
            ++m_statCopies;
        }
        m_statBytes += size;
    }

    void UploadContext::UploadTexture(const Texture& destination, std::span<const TextureSubresourceData> subresources)
    {
        if (subresources.empty())
            return;
        const D3D12_RESOURCE_DESC1& desc = destination.GetResourceDesc();
        if (subresources.size() > desc.MipLevels)
            throw std::out_of_range("UploadContext::UploadTexture: more subresources than the texture has");

        auto* ring = static_cast<uint8_t*>(m_ring.GetMappedData());
        for (UINT subresource = 0; subresource < subresources.size(); ++subresource)
        {
            // A GPU texture's memory layout is hardware-specific ("swizzled" or tiled for cache efficiency), so we
            // can't write it directly. Instead we stage the pixels in a simple row-by-row layout that the copy
            // engine understands. GetCopyableFootprints describes that layout for this subresource:
            //  - RowPitch: bytes between rows in staging, rounded up to a multiple of 256
            //    (D3D12_TEXTURE_DATA_PITCH_ALIGNMENT) even if a row of pixels is shorter,
            //  - numRows / rowSizeInBytes: how many rows and how many real bytes per row to copy,
            //  - totalBytes: how much staging memory the whole subresource needs.
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
            UINT numRows = 0;
            UINT64 rowSizeInBytes = 0;
            UINT64 totalBytes = 0;
            m_device->GetCopyableFootprints1(&desc, subresource, 1, 0, &footprint, &numRows, &rowSizeInBytes, &totalBytes);
            if (totalBytes > m_ringCapacity)
                throw std::runtime_error("UploadContext::UploadTexture: subresource larger than the staging ring");

            // Each subresource's staging data must start on a 512-byte boundary (D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT).
            const uint64_t ringOffset = AllocateStaging(totalBytes, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
            BeginRecordingIfNeeded();

            // CPU: copy row by row, because the source rows (rowPitch) and staging rows (footprint RowPitch) can be
            // spaced differently.
            const TextureSubresourceData& source = subresources[subresource];
            const auto* sourceBytes = static_cast<const uint8_t*>(source.data);
            for (UINT row = 0; row < numRows; ++row)
            {
                std::memcpy(ring + ringOffset + row * static_cast<uint64_t>(footprint.Footprint.RowPitch),
                            sourceBytes + row * source.rowPitch, rowSizeInBytes);
            }

            // GPU: copy staging -> texture subresource. The source location is the ring buffer described by the
            // footprint (moved to where we put the data); the destination is "subresource N of the texture".
            footprint.Offset = ringOffset;
            const CD3DX12_TEXTURE_COPY_LOCATION sourceLocation(m_ring.Get(), footprint);
            const CD3DX12_TEXTURE_COPY_LOCATION destinationLocation(destination.Get(), subresource);
            m_commandList->CopyTextureRegion(&destinationLocation, 0, 0, 0, &sourceLocation, nullptr);

            m_statBytes += rowSizeInBytes * numRows;
            ++m_statCopies;
        }

        // Unlike buffers, textures have a layout, so this one does need a barrier: COPY_DEST -> SHADER_RESOURCE.
        //  - SyncBefore/AccessBefore = COPY/COPY_DEST: wait for the copies above and flush their writes.
        //  - SyncAfter/AccessAfter = NONE/NO_ACCESS: nothing later in this command list reads it. The draws that
        //    sample it run in a later ExecuteCommandLists, which the boundary already orders.
        //  - SHADER_RESOURCE (not DIRECT_QUEUE_SHADER_RESOURCE): readable from any queue, so a future copy or
        //    compute queue could use it too.
        const CD3DX12_TEXTURE_BARRIER toShaderResource(
            D3D12_BARRIER_SYNC_COPY, D3D12_BARRIER_SYNC_NONE,
            D3D12_BARRIER_ACCESS_COPY_DEST, D3D12_BARRIER_ACCESS_NO_ACCESS,
            D3D12_BARRIER_LAYOUT_COPY_DEST, D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,
            destination.Get(), CD3DX12_BARRIER_SUBRESOURCE_RANGE(0xffffffff));
        const CD3DX12_BARRIER_GROUP group(1, &toShaderResource);
        m_commandList->Barrier(1, &group);
    }

    void UploadContext::Flush()
    {
        if (!m_recording && m_inFlight.empty())
            return;

        Timer timer;
        SubmitCurrentBatch();
        while (!m_inFlight.empty())
            RetireOldestBatch(true);

        log::Info("UploadContext: uploaded {} bytes in {} copies, {} batch(es), {} wait(s) for ring space; final flush {:.2f} ms",
                  m_statBytes, m_statCopies, m_statBatches, m_statWaits, timer.Tick() * 1000.0f);
        m_statBytes = m_statCopies = m_statBatches = m_statWaits = 0;
    }
}
