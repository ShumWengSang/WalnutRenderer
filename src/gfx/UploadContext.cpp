#include "gfx/UploadContext.h"

#include "core/Log.h"
#include "core/Timer.h"
#include "gfx/CommandQueue.h"
#include "gfx/GpuUtils.h"
#include "gfx/Pix.h"

#include <cstring>
#include <stdexcept>

namespace walnut
{
    UploadContext::UploadContext(ID3D12Device10* device, CommandQueue& queue)
        : m_device(device), m_queue(queue)
    {
        // Copies are recorded on a DIRECT list and run on the main queue. A dedicated COPY queue can
        // overlap uploads with rendering, but needs cross-queue synchronization; that comes later.
        WR_CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_allocator)));
        SetDebugName(m_allocator.Get(), L"UploadAllocator");
        WR_CHECK(device->CreateCommandList1(0, D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_LIST_FLAG_NONE,
                                            IID_PPV_ARGS(&m_commandList)));
        SetDebugName(m_commandList.Get(), L"UploadCommandList");
    }

    UploadContext::~UploadContext()
    {
        if (m_recording)
            log::Warn("UploadContext destroyed with {} unflushed upload(s); they were never executed", m_stagingBuffers.size());
    }

    void UploadContext::BeginRecordingIfNeeded()
    {
        if (m_recording)
            return;
        WR_CHECK(m_allocator->Reset());
        WR_CHECK(m_commandList->Reset(m_allocator.Get(), nullptr));
        PIXBeginEvent(m_commandList.Get(), pix::kColorGpu, L"Upload");
        m_recording = true;
    }

    void UploadContext::UploadBuffer(const Buffer& destination, const void* data, uint64_t size, uint64_t destinationOffset)
    {
        if (size == 0)
            return;
        if (destinationOffset + size > destination.GetSize())
            throw std::out_of_range("UploadContext::UploadBuffer: write past the end of the destination buffer");

        BeginRecordingIfNeeded();

        // 1. CPU: copy into a fresh staging buffer (persistently mapped UPLOAD memory).
        Buffer& staging = m_stagingBuffers.emplace_back(m_device, BufferDesc{
            .size = size,
            .heapType = D3D12_HEAP_TYPE_UPLOAD,
            .name = L"UploadStaging",
        });
        std::memcpy(staging.GetMappedData(), data, size);

        // 2. GPU: copy staging -> destination.
        //
        // No barrier is needed before or after this copy. Buffers have no layout, and every
        // ExecuteCommandLists boundary completes all prior work and flushes caches. So:
        //  - the destination's first use in this command list (as a copy destination) needs no barrier, and
        //  - the draws that read it run in a later ExecuteCommandLists and see the finished data.
        m_commandList->CopyBufferRegion(destination.Get(), destinationOffset, staging.Get(), 0, size);
        m_pendingBytes += size;
    }

    void UploadContext::Flush()
    {
        if (!m_recording)
            return;

        Timer timer;
        PIXEndEvent(m_commandList.Get());
        WR_CHECK(m_commandList->Close());
        m_queue.Execute(m_commandList.Get());

        // Block until the copies are done; only then may the staging memory be released.
        m_queue.WaitForFence(m_queue.Signal());

        log::Info("UploadContext: uploaded {} bytes in {} copies ({:.2f} ms)",
                  m_pendingBytes, m_stagingBuffers.size(), timer.Tick() * 1000.0f);

        m_stagingBuffers.clear();
        m_pendingBytes = 0;
        m_recording = false;
    }
}
