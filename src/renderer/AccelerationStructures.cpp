#include "renderer/AccelerationStructures.h"

#include "core/Log.h"
#include "gfx/CommandQueue.h"
#include "gfx/Device.h"
#include "gfx/GpuUtils.h"
#include "gfx/Pix.h"
#include "scene/Scene.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <vector>

namespace walnut
{
    namespace
    {
        constexpr uint64_t kAlignment = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT; // 256

        uint64_t AlignUp(uint64_t value, uint64_t alignment)
        {
            return (value + alignment - 1) / alignment * alignment;
        }

        // Acceleration structures and build scratch memory are written by the GPU (UAV access).
        // Under Enhanced Barriers, buffers have no state, so the RAYTRACING_ACCELERATION_STRUCTURE flag is what marks
        // a buffer as holding acceleration structures (it replaces the old RAYTRACING_ACCELERATION_STRUCTURE state).
        Buffer CreateGpuBuffer(ID3D12Device10* device, uint64_t size, bool accelerationStructure, const wchar_t* name)
        {
            D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            if (accelerationStructure)
                flags |= D3D12_RESOURCE_FLAG_RAYTRACING_ACCELERATION_STRUCTURE;
            return Buffer(device, BufferDesc{ .size = size, .heapType = D3D12_HEAP_TYPE_DEFAULT, .flags = flags, .name = name });
        }
    }

    AccelerationStructures::AccelerationStructures(Device& device, CommandQueue& queue, DescriptorHeap& srvHeap,
                                                   const Scene& scene, const Buffer& vertices, const Buffer& indices)
        : m_srvHeap(srvHeap)
    {
        ID3D12Device10* d3d = device.Get();
        const auto start = std::chrono::steady_clock::now();

        // --- 1. Describe every BLAS: one triangle geometry per scene primitive ---------------------------------
        // The geometry points straight into the scene's merged buffers. Positions are the first 12 bytes of each
        // 48-byte SceneVertex, so the stride skips the rest. The loader already made every index global (it points
        // into the whole vertex buffer), so VertexCount is the whole buffer.
        const size_t primitiveCount = scene.primitives.size();
        std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> geometries(primitiveCount);
        for (size_t i = 0; i < primitiveCount; ++i)
        {
            const ScenePrimitive& primitive = scene.primitives[i];
            D3D12_RAYTRACING_GEOMETRY_DESC& geometry = geometries[i];
            geometry.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
            // OPAQUE lets the hardware accept a hit immediately. Alpha-tested geometry (leaves) is left non-opaque,
            // so a shader gets to decide whether a hit on the transparent part of the texture counts (step I3).
            geometry.Flags = scene.materials[primitive.materialIndex].alphaMode == AlphaMode::Mask
                                 ? D3D12_RAYTRACING_GEOMETRY_FLAG_NONE
                                 : D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
            D3D12_RAYTRACING_GEOMETRY_TRIANGLES_DESC& triangles = geometry.Triangles;
            triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
            triangles.IndexCount = primitive.indexCount;
            triangles.IndexBuffer = indices.GetGpuAddress() + primitive.firstIndex * sizeof(uint32_t);
            triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
            triangles.VertexCount = static_cast<UINT>(scene.vertices.size());
            triangles.VertexBuffer = { vertices.GetGpuAddress(), sizeof(SceneVertex) };
            m_stats.triangles += primitive.indexCount / 3;
        }

        // --- 2. Ask the driver how big each one will be, and pack them into one buffer ---------------------------
        // PREFER_FAST_TRACE: spend more build time for faster rays (the right trade for static geometry).
        // ALLOW_COMPACTION: lets us ask the GPU afterwards how small it could be copied down to.
        std::vector<D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS> blasInputs(primitiveCount);
        std::vector<uint64_t> blasOffsets(primitiveCount), scratchOffsets(primitiveCount);
        uint64_t blasTotal = 0, scratchTotal = 0;
        for (size_t i = 0; i < primitiveCount; ++i)
        {
            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS& inputs = blasInputs[i];
            inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
            inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE |
                           D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_COMPACTION;
            inputs.NumDescs = 1;
            inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
            inputs.pGeometryDescs = &geometries[i];

            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
            d3d->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);
            blasOffsets[i] = blasTotal;
            scratchOffsets[i] = scratchTotal;
            blasTotal += AlignUp(info.ResultDataMaxSizeInBytes, kAlignment);
            // Each build gets its own scratch region, so the GPU can run all builds at once (no barriers between).
            scratchTotal += AlignUp(info.ScratchDataSizeInBytes, kAlignment);
        }

        // --- 3. The TLAS: one instance per scene instance ---------------------------------------------------------
        // Instance descriptions are read by the build from GPU memory; an UPLOAD buffer is fine for that.
        const size_t instanceCount = scene.instances.size();
        Buffer instanceDescs(d3d, BufferDesc{ .size = std::max<uint64_t>(instanceCount, 1) * sizeof(D3D12_RAYTRACING_INSTANCE_DESC),
                                              .heapType = D3D12_HEAP_TYPE_UPLOAD, .name = L"TlasInstanceDescs" });
        auto* descs = static_cast<D3D12_RAYTRACING_INSTANCE_DESC*>(instanceDescs.GetMappedData());

        m_blasBuffer = CreateGpuBuffer(d3d, std::max<uint64_t>(blasTotal, kAlignment), true, L"BLAS (all primitives)");
        for (size_t i = 0; i < instanceCount; ++i)
        {
            const SceneInstance& instance = scene.instances[i];
            D3D12_RAYTRACING_INSTANCE_DESC desc{};
            // DXR wants a 3x4 matrix for COLUMN vectors (world = M * p), ours is for row vectors (p * M): the
            // transpose. XMStoreFloat3x4 stores exactly that transpose (it exists for this purpose).
            DirectX::XMFLOAT3X4 transform;
            DirectX::XMStoreFloat3x4(&transform, DirectX::XMLoadFloat4x4(&instance.world));
            std::memcpy(desc.Transform, &transform, sizeof(desc.Transform));
            desc.InstanceID = static_cast<UINT>(i);  // 24 bits a shader can read back on a hit (CommittedInstanceID)
            desc.InstanceMask = 0xFF;                // rays can skip instances by mask; everything is visible for now
            desc.InstanceContributionToHitGroupIndex = 0;
            desc.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_NONE;
            desc.AccelerationStructure = m_blasBuffer.GetGpuAddress() + blasOffsets[instance.primitiveIndex];
            descs[i] = desc;
        }

        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS tlasInputs{};
        tlasInputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
        tlasInputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        tlasInputs.NumDescs = static_cast<UINT>(instanceCount);
        tlasInputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        tlasInputs.InstanceDescs = instanceDescs.GetGpuAddress();
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO tlasInfo{};
        d3d->GetRaytracingAccelerationStructurePrebuildInfo(&tlasInputs, &tlasInfo);
        const uint64_t tlasScratchOffset = scratchTotal;
        scratchTotal += AlignUp(tlasInfo.ScratchDataSizeInBytes, kAlignment);

        m_tlasBuffer = CreateGpuBuffer(d3d, AlignUp(std::max<uint64_t>(tlasInfo.ResultDataMaxSizeInBytes, 1), kAlignment), true, L"TLAS");
        Buffer scratch = CreateGpuBuffer(d3d, std::max<uint64_t>(scratchTotal, kAlignment), false, L"AccelerationStructureScratch");
        // The GPU writes each BLAS's compacted size here (8 bytes each), then we copy them to CPU-readable memory.
        Buffer postbuild = CreateGpuBuffer(d3d, std::max<uint64_t>(primitiveCount, 1) * sizeof(uint64_t), false, L"BlasPostbuildInfo");
        Buffer readback(d3d, BufferDesc{ .size = postbuild.GetSize(), .heapType = D3D12_HEAP_TYPE_READBACK, .name = L"BlasPostbuildReadback" });

        // --- 4. Record the builds -----------------------------------------------------------------------------------
        ComPtr<ID3D12CommandAllocator> allocator;
        WR_CHECK(d3d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
        ComPtr<ID3D12GraphicsCommandList7> commandList;
        WR_CHECK(d3d->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&commandList)));
        SetDebugName(commandList.Get(), L"AccelerationStructureBuild");
        {
            PIXScopedEvent(commandList.Get(), pix::kColorGpu, L"Build BLASes");
            for (size_t i = 0; i < primitiveCount; ++i)
            {
                D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
                build.Inputs = blasInputs[i];
                build.DestAccelerationStructureData = m_blasBuffer.GetGpuAddress() + blasOffsets[i];
                build.ScratchAccelerationStructureData = scratch.GetGpuAddress() + scratchOffsets[i];
                D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC postbuildDesc{
                    postbuild.GetGpuAddress() + i * sizeof(uint64_t),
                    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_COMPACTED_SIZE };
                commandList->BuildRaytracingAccelerationStructure(&build, 1, &postbuildDesc);
            }
        }

        // The TLAS build reads the BLASes, and the copy reads the postbuild sizes: both must wait for the BLAS
        // builds to finish writing.
        const D3D12_BUFFER_BARRIER barriers[] = {
            CD3DX12_BUFFER_BARRIER(D3D12_BARRIER_SYNC_BUILD_RAYTRACING_ACCELERATION_STRUCTURE,
                                   D3D12_BARRIER_SYNC_BUILD_RAYTRACING_ACCELERATION_STRUCTURE,
                                   D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_WRITE,
                                   D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ, m_blasBuffer.Get()),
            CD3DX12_BUFFER_BARRIER(D3D12_BARRIER_SYNC_BUILD_RAYTRACING_ACCELERATION_STRUCTURE,
                                   D3D12_BARRIER_SYNC_COPY,
                                   D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                                   D3D12_BARRIER_ACCESS_COPY_SOURCE, postbuild.Get()),
        };
        const CD3DX12_BARRIER_GROUP group(_countof(barriers), barriers);
        commandList->Barrier(1, &group);

        commandList->CopyBufferRegion(readback.Get(), 0, postbuild.Get(), 0, postbuild.GetSize());
        {
            PIXScopedEvent(commandList.Get(), pix::kColorGpu, L"Build TLAS");
            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
            build.Inputs = tlasInputs;
            build.DestAccelerationStructureData = m_tlasBuffer.GetGpuAddress();
            build.ScratchAccelerationStructureData = scratch.GetGpuAddress() + tlasScratchOffset;
            commandList->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
        }
        WR_CHECK(commandList->Close());

        // Load-time work: submit and wait, so the scratch, instance and readback buffers can be freed right after.
        queue.Execute(commandList.Get());
        queue.Flush();

        const auto* compactedSizes = static_cast<const uint64_t*>(readback.GetMappedData());
        for (size_t i = 0; i < primitiveCount; ++i)
            m_stats.blasCompactedBytes += AlignUp(compactedSizes[i], kAlignment);

        // Shaders find the TLAS through the bindless heap. An acceleration-structure SRV has no resource: just the
        // GPU address of the TLAS.
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.RaytracingAccelerationStructure.Location = m_tlasBuffer.GetGpuAddress();
        m_tlasSrv = srvHeap.Allocate();
        d3d->CreateShaderResourceView(nullptr, &srvDesc, m_tlasSrv.cpu);

        m_stats.blasCount = static_cast<uint32_t>(primitiveCount);
        m_stats.instanceCount = static_cast<uint32_t>(instanceCount);
        m_stats.blasBytes = blasTotal;
        m_stats.tlasBytes = m_tlasBuffer.GetSize();
        m_stats.scratchBytes = scratchTotal;
        m_stats.buildMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();

        constexpr double kMB = 1024.0 * 1024.0;
        log::Info("Acceleration structures: {} BLASes ({} triangles) {:.1f} MB (compactable to {:.1f} MB), TLAS with "
                  "{} instances {:.1f} KB, scratch {:.1f} MB; built in {:.1f} ms",
                  m_stats.blasCount, m_stats.triangles, static_cast<double>(m_stats.blasBytes) / kMB,
                  static_cast<double>(m_stats.blasCompactedBytes) / kMB, m_stats.instanceCount,
                  static_cast<double>(m_stats.tlasBytes) / 1024.0, static_cast<double>(m_stats.scratchBytes) / kMB,
                  m_stats.buildMs);
    }

    AccelerationStructures::~AccelerationStructures()
    {
        m_srvHeap.Free(m_tlasSrv);
    }
}
