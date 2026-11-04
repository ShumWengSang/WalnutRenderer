#include "gfx/CommandQueue.h"

#include "gfx/GpuUtils.h"

namespace walnut
{
    CommandQueue::CommandQueue(ID3D12Device* device, D3D12_COMMAND_LIST_TYPE type, const wchar_t* name)
    {
        D3D12_COMMAND_QUEUE_DESC desc{};
        desc.Type = type;
        desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
        desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
        WR_CHECK(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&m_queue)));
        SetDebugName(m_queue.Get(), name);

        WR_CHECK(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)));
        SetDebugName(m_fence.Get(), L"CommandQueueFence");

        m_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!m_fenceEvent)
            ThrowIfFailed(HRESULT_FROM_WIN32(GetLastError()), "CreateEventW");
    }

    CommandQueue::~CommandQueue()
    {
        if (m_fenceEvent)
            CloseHandle(m_fenceEvent);
    }

    void CommandQueue::Execute(ID3D12CommandList* commandList)
    {
        ID3D12CommandList* lists[] = { commandList };
        m_queue->ExecuteCommandLists(1, lists);
    }

    uint64_t CommandQueue::Signal()
    {
        const uint64_t value = m_nextFenceValue++;
        WR_CHECK(m_queue->Signal(m_fence.Get(), value));
        return value;
    }

    bool CommandQueue::IsFenceComplete(uint64_t value) const
    {
        return m_fence->GetCompletedValue() >= value;
    }

    void CommandQueue::WaitForFence(uint64_t value)
    {
        if (IsFenceComplete(value))
            return;
        WR_CHECK(m_fence->SetEventOnCompletion(value, m_fenceEvent));
        WaitForSingleObject(m_fenceEvent, INFINITE);
    }

    void CommandQueue::Flush()
    {
        WaitForFence(Signal());
    }
}
