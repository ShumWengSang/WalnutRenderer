#pragma once

#include "gfx/Buffer.h"
#include "gfx/D3D12Common.h"
#include "gfx/DescriptorHeap.h"
#include "gfx/PipelineBuilder.h"
#include "shared/Constants.h"

#include <cstdint>
#include <filesystem>
#include <string>

namespace walnut
{
    class Device;
    class ShaderCompiler;

    // The cluster grid of clustered lighting (shaders/clusters.hlsl, layout in shaders/common/Clusters.hlsli): every
    // frame a compute shader lists, for each of the 16 x 9 x 24 view-space clusters, the point lights that reach it.
    class ClusterGrid : public ShaderReloadable
    {
    public:
        // The grid's size (kClusterCountX/Y/Z, kClusterCount) is shared with the shaders: shared/Constants.h.
        // Room in the shared index list. Every (cluster, light) pair takes one slot: 3456 clusters x ~300 lights.
        static constexpr uint32_t kIndexCapacity = 1u << 20;

        ClusterGrid(Device& device, DescriptorHeap& srvHeap, const ShaderCompiler& compiler,
                    std::filesystem::path shaderPath, ID3D12RootSignature* rootSignature, uint32_t framesInFlight);
        ~ClusterGrid();

        ClusterGrid(const ClusterGrid&) = delete;
        ClusterGrid& operator=(const ClusterGrid&) = delete;


        // Records the build (expects the bindless state and the frame constants, with the point lights and the
        // cluster fields, to be bound). Afterwards the grid is readable by pixel shaders. Call after
        // Renderer::BeginFrame: it also reads back how full the index list got the last time this frame slot ran.
        void Build(ID3D12GraphicsCommandList7* commandList, uint32_t frameIndex);

        // (cluster, light) pairs written by a recent build (a frame or two old: read back from the GPU), and the most
        // that fit. Beyond the capacity, clusters lose lights.
        uint32_t GetUsedIndices() const { return m_usedIndices; }

        uint32_t GetGridSrvIndex() const { return m_gridSrv.index; }
        uint32_t GetIndicesSrvIndex() const { return m_indicesSrv.index; }
        uint64_t GetBytes() const { return m_grid.GetSize() + m_indices.GetSize() + m_counter.GetSize(); }

    private:
        void BuildPipelines(PipelineBuilder& builder) override;

        ID3D12Device10* m_device = nullptr;
        DescriptorHeap& m_srvHeap;

        Buffer m_grid;    // uint2 per cluster: (first index, count)
        Buffer m_indices; // uint per (cluster, light) pair
        Buffer m_counter; // one uint: how much of m_indices is in use
        Buffer m_counterReadback; // one uint per frame in flight, copied from m_counter for the CPU
        uint32_t m_usedIndices = 0;
        OwnedDescriptor m_gridSrv, m_gridUav, m_indicesSrv, m_indicesUav, m_counterUav;

        ComPtr<ID3D12PipelineState> m_resetPipeline;
        ComPtr<ID3D12PipelineState> m_buildPipeline;
    };
}
