#include "gfx/UploadContext.h"

#include "core/Log.h"
#include "core/Timer.h"
#include "gfx/CommandQueue.h"
#include "gfx/GpuUtils.h"
#include "gfx/Pix.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace walnut
{
    namespace
    {
        // CopyBufferRegion has no alignment rule, but aligned staging keeps memcpy fast. Texture uploads
        // will need 512 bytes (D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT).
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
