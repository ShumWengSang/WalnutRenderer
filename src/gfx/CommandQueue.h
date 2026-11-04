#pragma once

#include "gfx/D3D12Common.h"

#include <cstdint>

namespace walnut
{
    // A command queue plus the fence used to track GPU progress on it.
    class CommandQueue
    {
    public:
        CommandQueue(ID3D12Device* device, D3D12_COMMAND_LIST_TYPE type, const wchar_t* name);
        ~CommandQueue();

        CommandQueue(const CommandQueue&) = delete;
        CommandQueue& operator=(const CommandQueue&) = delete;

        ID3D12CommandQueue* Get() const { return m_queue.Get(); }

        void Execute(ID3D12CommandList* commandList);

        // Enqueues a fence signal and returns the value that marks "work so far is done".
        uint64_t Signal();
        bool IsFenceComplete(uint64_t value) const;
        void WaitForFence(uint64_t value);

        // Blocks the CPU until all submitted work has finished.
        void Flush();

    private:
        ComPtr<ID3D12CommandQueue> m_queue;
        ComPtr<ID3D12Fence> m_fence;
        HANDLE m_fenceEvent = nullptr;
        uint64_t m_nextFenceValue = 1;
    };
}
