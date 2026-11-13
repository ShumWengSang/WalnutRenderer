#pragma once

#include "gfx/D3D12Common.h"
#include "gfx/DescriptorHeap.h"
#include "scene/PointLights.h"

#include <cstdint>
#include <span>
#include <vector>

namespace walnut
{
    class Device;
    class FrameAllocator;

    // The frame's point lights as a StructuredBuffer<PointLight> in the bindless heap. The lights change every frame
    // (count, animation), so they live in the frame allocator's upload memory, which the GPU reads directly: no copy
    // to a default-heap buffer, and no barrier. Each frame in flight has its own SRV slot, rewritten when that slot is
    // recorded again (the Renderer has waited for its fence by then, so the GPU is done with the old view).
    class LightBuffer
    {
    public:
        LightBuffer(Device& device, DescriptorHeap& srvHeap, uint32_t framesInFlight);
        ~LightBuffer();

        LightBuffer(const LightBuffer&) = delete;
        LightBuffer& operator=(const LightBuffer&) = delete;

        // Call after Renderer::BeginFrame (which resets this frame's upload memory). Returns the SRV's heap index.
        uint32_t Upload(FrameAllocator& allocator, uint32_t frameIndex, std::span<const PointLightGpu> lights);

    private:
        ID3D12Device10* m_device = nullptr;
        DescriptorHeap& m_srvHeap;
        std::vector<DescriptorHandle> m_srvs; // one per frame in flight
    };
}
