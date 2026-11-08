#include "renderer/ScenePass.h"

#include "core/Log.h"
#include "gfx/Device.h"
#include "gfx/FrameAllocator.h"
#include "gfx/GpuUtils.h"
#include "gfx/Pix.h"
#include "gfx/RootSignature.h"
#include "gfx/ShaderCompiler.h"
#include "gfx/UploadContext.h"
#include "scene/Scene.h"

namespace walnut
{
    namespace
    {
        // One element of the instance buffer. Must match struct Instance in scene.hlsl.
        struct InstanceGpuData
        {
            DirectX::XMFLOAT4X4 world;
            // Normals can't use 'world' when it scales unevenly (they'd lean towards the stretched axis); they need
            // the inverse-transpose of it. Computed once here instead of per vertex.
            DirectX::XMFLOAT4X4 normalMatrix;
        };
        static_assert(sizeof(InstanceGpuData) == 128);

        // Per-pass constants (b1). Must match the cbuffer in scene.hlsl.
        struct ScenePassConstants
        {
            DirectX::XMFLOAT4X4 viewProjection;
        };

        // Root constants (b0). Must match DrawConstants in scene.hlsl.
        struct SceneDrawConstants
        {
            uint32_t vertexBufferIndex;
            uint32_t instanceBufferIndex;
            uint32_t debugView;
            uint32_t instanceIndex; // the only value that changes between draws
        };
        static_assert(sizeof(SceneDrawConstants) <= kNumDrawConstants * sizeof(uint32_t));
        constexpr uint32_t kInstanceIndexConstant = offsetof(SceneDrawConstants, instanceIndex) / sizeof(uint32_t);
    }

    ScenePass::ScenePass(Device& device, UploadContext& upload, DescriptorHeap& srvHeap, const ShaderCompiler& compiler,
                         std::filesystem::path shaderPath, ID3D12RootSignature* rootSignature,
                         DXGI_FORMAT rtvFormat, DXGI_FORMAT dsvFormat, const Scene& scene)
        : m_device(device.Get()), m_srvHeap(srvHeap), m_compiler(compiler), m_shaderPath(std::move(shaderPath)),
          m_rootSignature(rootSignature), m_rtvFormat(rtvFormat), m_dsvFormat(dsvFormat)
    {
        using namespace DirectX;

        // Instance data for the GPU, and one draw per instance.
        std::vector<InstanceGpuData> instances;
        instances.reserve(scene.instances.size());
        for (const SceneInstance& instance : scene.instances)
        {
            const XMMATRIX world = XMLoadFloat4x4(&instance.world);
            InstanceGpuData& gpu = instances.emplace_back();
            gpu.world = instance.world;
            XMStoreFloat4x4(&gpu.normalMatrix, XMMatrixTranspose(XMMatrixInverse(nullptr, world)));

            const ScenePrimitive& primitive = scene.primitives[instance.primitiveIndex];
            m_draws.push_back({ primitive.indexCount, primitive.firstIndex, static_cast<uint32_t>(m_draws.size()) });
        }

        // Static geometry: DEFAULT heap, filled once through the staging ring.
        m_vertexBuffer = CreateStructuredBuffer(upload, scene.vertices.data(), static_cast<uint32_t>(scene.vertices.size()),
                                                sizeof(SceneVertex), L"SceneVertices", m_vertexSrv);
        m_instanceBuffer = CreateStructuredBuffer(upload, instances.data(), static_cast<uint32_t>(instances.size()),
                                                  sizeof(InstanceGpuData), L"SceneInstances", m_instanceSrv);

        // Indices stay a real index buffer instead of being pulled by the shader too: the input assembler reads
        // them, and the GPU can then reuse the vertex shader result of an index it has just seen (the
        // post-transform cache). Each SV_VertexID the shader receives is the index value read from here.
        const uint64_t indexBytes = scene.indices.size() * sizeof(uint32_t);
        m_indexBuffer = Buffer(m_device, BufferDesc{ .size = indexBytes, .heapType = D3D12_HEAP_TYPE_DEFAULT, .name = L"SceneIndices" });
        upload.UploadBuffer(m_indexBuffer, scene.indices.data(), indexBytes);

        if (!ReloadShaders())
            throw std::runtime_error("Failed to build scene pipeline:\n" + m_lastError);
        log::Info("ScenePass: {} draws, {:.1f} MB of geometry and instance data on the GPU", m_draws.size(),
                  static_cast<double>(GetGpuBytes()) / (1024.0 * 1024.0));
    }

    ScenePass::~ScenePass()
    {
        m_srvHeap.Free(m_vertexSrv);
        m_srvHeap.Free(m_instanceSrv);
    }

    uint64_t ScenePass::GetGpuBytes() const
    {
        return m_vertexBuffer.GetSize() + m_indexBuffer.GetSize() + m_instanceBuffer.GetSize();
    }

    Buffer ScenePass::CreateStructuredBuffer(UploadContext& upload, const void* data, uint32_t count, uint32_t stride,
                                             const wchar_t* name, DescriptorHandle& srv)
    {
        const uint64_t size = static_cast<uint64_t>(count) * stride;
        Buffer buffer(m_device, BufferDesc{ .size = size, .heapType = D3D12_HEAP_TYPE_DEFAULT, .name = name });
        upload.UploadBuffer(buffer, data, size);

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = DXGI_FORMAT_UNKNOWN; // structured: elements are StructureByteStride bytes, no format
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Buffer.NumElements = count;
        srvDesc.Buffer.StructureByteStride = stride;
        srv = m_srvHeap.Allocate();
        m_device->CreateShaderResourceView(buffer.Get(), &srvDesc, srv.cpu);
        return buffer;
    }

    bool ScenePass::ReloadShaders()
    {
        const ShaderCompileResult vs = m_compiler.Compile({ m_shaderPath, L"VSMain", ShaderStage::Vertex });
        const ShaderCompileResult ps = m_compiler.Compile({ m_shaderPath, L"PSMain", ShaderStage::Pixel });
        if (!vs.Succeeded() || !ps.Succeeded())
        {
            m_lastError = vs.errors + ps.errors;
            return false;
        }

        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = m_rootSignature;
        desc.VS = vs.GetBytecode();
        desc.PS = ps.GetBytecode();
        desc.InputLayout = { nullptr, 0 }; // vertex pulling: no input layout
        desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        // glTF's front faces are counter-clockwise, and they still are on screen: the loader mirrored the data (z
        // negated), but the left-handed projection mirrors it back, so the image isn't mirrored and keeps its
        // winding. D3D12's default treats clockwise as front, so flip that.
        desc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        desc.RasterizerState.FrontCounterClockwise = TRUE;
        desc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        desc.SampleMask = UINT_MAX;
        desc.NumRenderTargets = 1;
        desc.RTVFormats[0] = m_rtvFormat;
        desc.DSVFormat = m_dsvFormat;
        desc.SampleDesc.Count = 1;
        desc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER; // reversed-Z: greater = closer

        ComPtr<ID3D12PipelineState> cullBack;
        HRESULT hr = m_device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&cullBack));
        desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        ComPtr<ID3D12PipelineState> cullNone;
        if (SUCCEEDED(hr))
            hr = m_device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&cullNone));
        if (FAILED(hr))
        {
            m_lastError = std::format("CreateGraphicsPipelineState failed (0x{:08X})", static_cast<uint32_t>(hr));
            log::Error("{}", m_lastError);
            return false;
        }
        SetDebugName(cullBack.Get(), L"ScenePSO_CullBack");
        SetDebugName(cullNone.Get(), L"ScenePSO_CullNone");

        m_cullBackPipeline = std::move(cullBack);
        m_cullNonePipeline = std::move(cullNone);
        m_lastError.clear();
        return true;
    }

    void ScenePass::Render(ID3D12GraphicsCommandList* commandList, FrameAllocator& frameAllocator,
                           DirectX::FXMMATRIX viewProjection, const SceneViewSettings& settings) const
    {
        PIXScopedEvent(commandList, pix::kColorGpu, L"ScenePass");

        // Everything shared by all draws is set once.
        ScenePassConstants passConstants{};
        DirectX::XMStoreFloat4x4(&passConstants.viewProjection, viewProjection);
        commandList->SetGraphicsRootConstantBufferView(RootParam::DrawData, frameAllocator.AllocateConstants(passConstants));
        commandList->SetPipelineState(settings.backfaceCulling ? m_cullBackPipeline.Get() : m_cullNonePipeline.Get());

        const SceneDrawConstants drawConstants{
            .vertexBufferIndex = m_vertexSrv.index,
            .instanceBufferIndex = m_instanceSrv.index,
            .debugView = static_cast<uint32_t>(settings.debugView),
            .instanceIndex = 0,
        };
        commandList->SetGraphicsRoot32BitConstants(RootParam::DrawConstants, sizeof(drawConstants) / sizeof(uint32_t),
                                                   &drawConstants, 0);

        const D3D12_INDEX_BUFFER_VIEW indexBufferView{
            .BufferLocation = m_indexBuffer.GetGpuAddress(),
            .SizeInBytes = static_cast<UINT>(m_indexBuffer.GetSize()),
            .Format = DXGI_FORMAT_R32_UINT,
        };
        commandList->IASetIndexBuffer(&indexBufferView);
        commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        // Per draw, only one 32-bit root constant changes. SV_InstanceID can't carry the instance index instead:
        // in D3D12 it always counts from 0 within a draw, ignoring StartInstanceLocation.
        for (const DrawItem& draw : m_draws)
        {
            commandList->SetGraphicsRoot32BitConstant(RootParam::DrawConstants, draw.instanceIndex, kInstanceIndexConstant);
            // BaseVertexLocation = 0: the loader already added each primitive's first vertex to its indices.
            commandList->DrawIndexedInstanced(draw.indexCount, 1, draw.firstIndex, 0, 0);
        }
    }
}
