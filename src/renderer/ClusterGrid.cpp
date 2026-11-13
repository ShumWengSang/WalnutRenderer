#include "renderer/ClusterGrid.h"

#include "core/Log.h"
#include "gfx/Device.h"
#include "gfx/GpuUtils.h"
#include "gfx/Pix.h"
#include "gfx/RootSignature.h"
#include "gfx/ShaderCompiler.h"

namespace walnut
{
    namespace
    {
        // Root constants for clusters.hlsl (b0). Must match DrawConstants there.
        struct ClusterBuildConstants
        {
            uint32_t gridUavIndex;
            uint32_t indicesUavIndex;
            uint32_t counterUavIndex;
            uint32_t indexCapacity;
        };
        static_assert(sizeof(ClusterBuildConstants) <= kNumDrawConstants * sizeof(uint32_t));

        constexpr uint32_t kThreadsPerGroup = 64; // [numthreads] of CSBuildClusters

        void CreateStructuredViews(ID3D12Device* device, ID3D12Resource* buffer, uint32_t count, uint32_t stride,
                                   const DescriptorHandle* srv, const DescriptorHandle& uav)
        {
            if (srv)
            {
                D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
                srvDesc.Format = DXGI_FORMAT_UNKNOWN;
                srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
                srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                srvDesc.Buffer.NumElements = count;
                srvDesc.Buffer.StructureByteStride = stride;
                device->CreateShaderResourceView(buffer, &srvDesc, srv->cpu);
            }
            // An unordered access view: read AND write from shaders, in any order ("unordered": the GPU doesn't sort
            // out conflicting writes for you, unlike render targets).
            D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
            uavDesc.Format = DXGI_FORMAT_UNKNOWN;
            uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
            uavDesc.Buffer.NumElements = count;
            uavDesc.Buffer.StructureByteStride = stride;
            device->CreateUnorderedAccessView(buffer, nullptr, &uavDesc, uav.cpu);
        }
    }

    ClusterGrid::ClusterGrid(Device& device, DescriptorHeap& srvHeap, const ShaderCompiler& compiler,
                             std::filesystem::path shaderPath, ID3D12RootSignature* rootSignature, uint32_t framesInFlight)
        : m_device(device.Get()), m_srvHeap(srvHeap), m_compiler(compiler), m_shaderPath(std::move(shaderPath)),
          m_rootSignature(rootSignature)
    {
        const auto makeBuffer = [&](uint64_t size, const wchar_t* name) {
            return Buffer(m_device, BufferDesc{ .size = size, .flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, .name = name });
        };
        m_grid = makeBuffer(uint64_t(kClusterCount) * 2 * sizeof(uint32_t), L"ClusterGrid");
        m_indices = makeBuffer(uint64_t(kIndexCapacity) * sizeof(uint32_t), L"ClusterLightIndices");
        m_counter = makeBuffer(sizeof(uint32_t), L"ClusterIndexCounter");
        m_counterReadback = Buffer(m_device, BufferDesc{ .size = framesInFlight * sizeof(uint32_t),
                                                         .heapType = D3D12_HEAP_TYPE_READBACK, .name = L"ClusterCounterReadback" });

        m_gridSrv = m_srvHeap.Allocate();
        m_gridUav = m_srvHeap.Allocate();
        m_indicesSrv = m_srvHeap.Allocate();
        m_indicesUav = m_srvHeap.Allocate();
        m_counterUav = m_srvHeap.Allocate();
        CreateStructuredViews(m_device, m_grid.Get(), kClusterCount, 2 * sizeof(uint32_t), &m_gridSrv, m_gridUav);
        CreateStructuredViews(m_device, m_indices.Get(), kIndexCapacity, sizeof(uint32_t), &m_indicesSrv, m_indicesUav);
        CreateStructuredViews(m_device, m_counter.Get(), 1, sizeof(uint32_t), nullptr, m_counterUav);

        if (!ReloadShaders())
            throw std::runtime_error("Failed to build cluster pipelines:\n" + m_lastError);
    }

    ClusterGrid::~ClusterGrid()
    {
        for (const DescriptorHandle& handle : { m_gridSrv, m_gridUav, m_indicesSrv, m_indicesUav, m_counterUav })
            m_srvHeap.Free(handle);
    }

    bool ClusterGrid::ReloadShaders()
    {
        const ShaderCompileResult reset = m_compiler.Compile({ m_shaderPath, L"CSResetCounter", ShaderStage::Compute });
        const ShaderCompileResult build = m_compiler.Compile({ m_shaderPath, L"CSBuildClusters", ShaderStage::Compute });
        if (!reset.Succeeded() || !build.Succeeded())
        {
            m_lastError = reset.errors + build.errors;
            return false;
        }

        // A compute pipeline is just the root signature and one shader: no rasterizer, blend or depth state.
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = m_rootSignature;
        ComPtr<ID3D12PipelineState> resetPipeline, buildPipeline;
        desc.CS = reset.GetBytecode();
        HRESULT hr = m_device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&resetPipeline));
        if (SUCCEEDED(hr))
        {
            desc.CS = build.GetBytecode();
            hr = m_device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&buildPipeline));
        }
        if (FAILED(hr))
        {
            m_lastError = std::format("CreateComputePipelineState failed (0x{:08X})", static_cast<uint32_t>(hr));
            log::Error("{}", m_lastError);
            return false;
        }
        SetDebugName(resetPipeline.Get(), L"ClusterResetPSO");
        SetDebugName(buildPipeline.Get(), L"ClusterBuildPSO");

        m_resetPipeline = std::move(resetPipeline);
        m_buildPipeline = std::move(buildPipeline);
        m_lastError.clear();
        return true;
    }

    void ClusterGrid::Build(ID3D12GraphicsCommandList7* commandList, uint32_t frameIndex)
    {
        PIXScopedEvent(commandList, pix::kColorGpu, L"Light clusters");

        // This slot's readback value was written by the last frame that used it, which the Renderer has waited for.
        uint32_t* readback = static_cast<uint32_t*>(m_counterReadback.GetMappedData());
        m_usedIndices = readback[frameIndex];

        // Last frame's pixel shaders read these buffers in an earlier ExecuteCommandLists, which is finished: no
        // barrier is needed before writing them again.
        const ClusterBuildConstants constants{
            .gridUavIndex = m_gridUav.index,
            .indicesUavIndex = m_indicesUav.index,
            .counterUavIndex = m_counterUav.index,
            .indexCapacity = kIndexCapacity,
        };
        commandList->SetComputeRoot32BitConstants(RootParam::DrawConstants, sizeof(constants) / sizeof(uint32_t), &constants, 0);

        commandList->SetPipelineState(m_resetPipeline.Get());
        commandList->Dispatch(1, 1, 1);

        // The build's InterlockedAdds must see the reset's 0: a UAV barrier orders a compute write before compute
        // reads/writes of the same buffer (without it the two dispatches may overlap on the GPU).
        const CD3DX12_BUFFER_BARRIER counterBarrier(
            D3D12_BARRIER_SYNC_COMPUTE_SHADING, D3D12_BARRIER_SYNC_COMPUTE_SHADING,
            D3D12_BARRIER_ACCESS_UNORDERED_ACCESS, D3D12_BARRIER_ACCESS_UNORDERED_ACCESS, m_counter.Get());
        const CD3DX12_BARRIER_GROUP counterGroup(1, &counterBarrier);
        commandList->Barrier(1, &counterGroup);

        // One thread per cluster, 64 per group: ceil(3456 / 64) = 54 groups.
        commandList->SetPipelineState(m_buildPipeline.Get());
        commandList->Dispatch((kClusterCount + kThreadsPerGroup - 1) / kThreadsPerGroup, 1, 1);

        // Wait for the build before any pixel shader reads the grid, and make its writes visible to them.
        const CD3DX12_BUFFER_BARRIER toRead[2] = {
            CD3DX12_BUFFER_BARRIER(D3D12_BARRIER_SYNC_COMPUTE_SHADING, D3D12_BARRIER_SYNC_PIXEL_SHADING,
                                   D3D12_BARRIER_ACCESS_UNORDERED_ACCESS, D3D12_BARRIER_ACCESS_SHADER_RESOURCE, m_grid.Get()),
            CD3DX12_BUFFER_BARRIER(D3D12_BARRIER_SYNC_COMPUTE_SHADING, D3D12_BARRIER_SYNC_PIXEL_SHADING,
                                   D3D12_BARRIER_ACCESS_UNORDERED_ACCESS, D3D12_BARRIER_ACCESS_SHADER_RESOURCE, m_indices.Get()),
        };
        // The counter goes to the CPU: wait for the build's atomics, then copy it into this slot's readback space.
        const CD3DX12_BUFFER_BARRIER counterToCopy(
            D3D12_BARRIER_SYNC_COMPUTE_SHADING, D3D12_BARRIER_SYNC_COPY,
            D3D12_BARRIER_ACCESS_UNORDERED_ACCESS, D3D12_BARRIER_ACCESS_COPY_SOURCE, m_counter.Get());
        const D3D12_BUFFER_BARRIER barriers[3] = { toRead[0], toRead[1], counterToCopy };
        const CD3DX12_BARRIER_GROUP readGroup(3, barriers);
        commandList->Barrier(1, &readGroup);
        commandList->CopyBufferRegion(m_counterReadback.Get(), frameIndex * sizeof(uint32_t), m_counter.Get(), 0, sizeof(uint32_t));
    }
}
