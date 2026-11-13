#pragma once

#include "gfx/Buffer.h"
#include "gfx/D3D12Common.h"
#include "gfx/DescriptorHeap.h"

#include <cstdint>

namespace walnut
{
    class CommandQueue;
    class Device;
    class SceneGpuData;
    struct Scene;

    // The scene in the form ray tracing hardware can search quickly: a two-level tree of bounding boxes.
    //
    //   BLAS (bottom-level acceleration structure): one per scene primitive. Built from its triangles, in object
    //        space. Expensive to build, built once.
    //   TLAS (top-level acceleration structure): one for the whole scene. A list of instances, each pointing at a
    //        BLAS with a world matrix. Cheap to rebuild, which is how moving objects work later.
    //
    // A ray first walks the TLAS's boxes to find which instances it might hit, then descends into those BLASes,
    // and only tests the few triangles left at the end. Shaders get the TLAS through a bindless SRV.
    //
    // The memory layout of both is up to the driver (the GPU vendor's format): D3D12 only reports how big they
    // will be (prebuild info) and builds them on the GPU from vertex and index buffers.
    class AccelerationStructures
    {
    public:
        // Builds everything on the GPU and waits for it. BLAS builds read the triangles straight from the scene's
        // merged vertex and index buffers (already uploaded, in 'gpuData'), no copies.
        AccelerationStructures(Device& device, CommandQueue& queue, DescriptorHeap& srvHeap, const Scene& scene,
                               const SceneGpuData& gpuData);
        ~AccelerationStructures();

        AccelerationStructures(const AccelerationStructures&) = delete;
        AccelerationStructures& operator=(const AccelerationStructures&) = delete;

        uint32_t GetTlasSrvIndex() const { return m_tlasSrv.index; }

        struct Stats
        {
            uint32_t blasCount = 0;
            uint32_t instanceCount = 0;
            uint64_t triangles = 0;
            uint64_t blasBytes = 0;          // all BLASes, as built
            uint64_t blasCompactedBytes = 0; // what they'd need after compaction (measured on the GPU)
            uint64_t tlasBytes = 0;
            uint64_t scratchBytes = 0;       // temporary memory used during the build, freed afterwards
            double buildMs = 0.0;            // CPU wall time for submitting + waiting for the GPU build
        };
        const Stats& GetStats() const { return m_stats; }

    private:
        DescriptorHeap& m_srvHeap;
        Buffer m_blasBuffer; // every BLAS, packed at 256-byte aligned offsets
        Buffer m_tlasBuffer;
        DescriptorHandle m_tlasSrv;
        Stats m_stats;
    };
}
