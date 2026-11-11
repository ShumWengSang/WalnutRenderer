#include "renderer/ScenePass.h"

#include "core/Log.h"
#include "core/StringUtils.h"
#include "gfx/Device.h"
#include "gfx/GpuUtils.h"
#include "gfx/Pix.h"
#include "gfx/RootSignature.h"
#include "gfx/ShaderCompiler.h"
#include "gfx/UploadContext.h"
#include "renderer/ShadowMap.h"
#include "scene/Scene.h"

#include <algorithm>
#include <array>

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
            uint32_t materialIndex;
            uint32_t padding[3]; // keeps the stride a multiple of 16 bytes, which GPUs read most efficiently
        };
        static_assert(sizeof(InstanceGpuData) == 144);

        // One element of the material buffer. Must match struct Material in scene.hlsl.
        struct MaterialGpuData
        {
            DirectX::XMFLOAT4 baseColorFactor;
            DirectX::XMFLOAT3 emissiveFactor;
            float metallicFactor;
            float roughnessFactor;
            float alphaCutoff;
            uint32_t alphaMode; // AlphaMode
            uint32_t doubleSided;
            uint32_t baseColorTexture; // bindless SRV index, or kNoTexture
            uint32_t padding[3];
        };
        static_assert(sizeof(MaterialGpuData) == 64);

        // Root constants (b0). Must match DrawConstants in scene.hlsl.
        struct SceneDrawConstants
        {
            uint32_t vertexBufferIndex;
            uint32_t instanceBufferIndex;
            uint32_t materialBufferIndex;
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
            const SceneMaterial& material = scene.materials[primitive.materialIndex];
            gpu.materialIndex = primitive.materialIndex;
            m_draws.push_back({ primitive.indexCount, primitive.firstIndex, static_cast<uint32_t>(m_draws.size()),
                                material.doubleSided, material.alphaMode == AlphaMode::Mask });
        }
        // Sorted by PSO: opaque single-sided, opaque double-sided, masked single-sided, masked double-sided. Each PSO
        // is then set once per frame. Opaque goes first on purpose: alpha-tested pixels are slower (see scene.hlsl),
        // and drawing them last lets the depth test reject the ones hidden behind walls before they're shaded.
        std::ranges::stable_sort(m_draws, {}, [](const DrawItem& draw) { return draw.alphaTest * 2 + draw.doubleSided; });

        // Textures first: materials refer to them by their slot in the bindless heap, which is only known once
        // each texture's SRV exists.
        const std::vector<uint32_t> textureSrvIndex = CreateTextures(upload, scene);

        std::vector<MaterialGpuData> materials;
        for (const SceneMaterial& material : scene.materials)
        {
            materials.push_back({
                .baseColorFactor = material.baseColorFactor,
                .emissiveFactor = material.emissiveFactor,
                .metallicFactor = material.metallicFactor,
                .roughnessFactor = material.roughnessFactor,
                .alphaCutoff = material.alphaCutoff,
                .alphaMode = static_cast<uint32_t>(material.alphaMode),
                .doubleSided = material.doubleSided ? 1u : 0u,
                .baseColorTexture = material.baseColorTexture != kNoTexture ? textureSrvIndex[material.baseColorTexture] : kNoTexture,
            });
        }

        // Static geometry: DEFAULT heap, filled once through the staging ring.
        m_vertexBuffer = CreateStructuredBuffer(upload, scene.vertices.data(), static_cast<uint32_t>(scene.vertices.size()),
                                                sizeof(SceneVertex), L"SceneVertices", m_vertexSrv);
        m_instanceBuffer = CreateStructuredBuffer(upload, instances.data(), static_cast<uint32_t>(instances.size()),
                                                  sizeof(InstanceGpuData), L"SceneInstances", m_instanceSrv);
        m_materialBuffer = CreateStructuredBuffer(upload, materials.data(), static_cast<uint32_t>(materials.size()),
                                                  sizeof(MaterialGpuData), L"SceneMaterials", m_materialSrv);

        // Indices stay a real index buffer instead of being pulled by the shader too: the input assembler reads
        // them, and the GPU can then reuse the vertex shader result of an index it has just seen (the
        // post-transform cache). Each SV_VertexID the shader receives is the index value read from here.
        const uint64_t indexBytes = scene.indices.size() * sizeof(uint32_t);
        m_indexBuffer = Buffer(m_device, BufferDesc{ .size = indexBytes, .heapType = D3D12_HEAP_TYPE_DEFAULT, .name = L"SceneIndices" });
        upload.UploadBuffer(m_indexBuffer, scene.indices.data(), indexBytes);

        if (!ReloadShaders())
            throw std::runtime_error("Failed to build scene pipeline:\n" + m_lastError);
        log::Info("ScenePass: {} draws ({} double-sided, {} alpha-tested), {} materials, {:.1f} MB of buffers, {} textures ({:.1f} MB) on the GPU",
                  m_draws.size(), std::ranges::count_if(m_draws, [](const DrawItem& draw) { return draw.doubleSided; }),
                  std::ranges::count_if(m_draws, [](const DrawItem& draw) { return draw.alphaTest; }),
                  materials.size(), static_cast<double>(GetGpuBytes()) / (1024.0 * 1024.0), m_textures.size(),
                  static_cast<double>(m_textureBytes) / (1024.0 * 1024.0));
    }

    std::vector<uint32_t> ScenePass::CreateTextures(UploadContext& upload, const Scene& scene)
    {
        std::vector<uint32_t> srvIndices(scene.textures.size(), kNoTexture);
        for (size_t i = 0; i < scene.textures.size(); ++i)
        {
            const SceneTexture& source = scene.textures[i];
            if (source.mips.empty())
                continue;

            // The same bytes, read two ways: _SRGB makes the sampler decode sRGB to linear light (before filtering,
            // so blending between texels is done in linear light too); plain UNORM returns the bytes / 255.
            Texture texture(m_device, TextureDesc{
                .width = source.mips[0].width,
                .height = source.mips[0].height,
                .mipLevels = static_cast<uint16_t>(source.mips.size()),
                .format = source.srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM,
                .initialLayout = D3D12_BARRIER_LAYOUT_COPY_DEST,
                .name = ToWide(source.name),
            });

            std::vector<TextureSubresourceData> subresources;
            for (const ImageRgba8& mip : source.mips)
                subresources.push_back({ .data = mip.pixels.data(), .rowPitch = mip.width * 4ull });
            upload.UploadTexture(texture, subresources); // copied into the staging ring now; the CPU pixels can go

            // What the GPU really allocates for it (rows padded, mips placed by the driver), not just width * height * 4.
            D3D12_RESOURCE_ALLOCATION_INFO1 info{};
            m_textureBytes += m_device->GetResourceAllocationInfo2(0, 1, &texture.GetResourceDesc(), &info).SizeInBytes;

            const DescriptorHandle srv = m_srvHeap.Allocate();
            texture.CreateSrv(m_device, srv.cpu);
            srvIndices[i] = srv.index;
            m_textureSrvs.push_back(srv);
            m_textures.push_back(std::move(texture));
        }
        return srvIndices;
    }

    ScenePass::~ScenePass()
    {
        m_srvHeap.Free(m_vertexSrv);
        m_srvHeap.Free(m_instanceSrv);
        m_srvHeap.Free(m_materialSrv);
        for (const DescriptorHandle& srv : m_textureSrvs)
            m_srvHeap.Free(srv);
    }

    uint64_t ScenePass::GetGpuBytes() const
    {
        return m_vertexBuffer.GetSize() + m_indexBuffer.GetSize() + m_instanceBuffer.GetSize() + m_materialBuffer.GetSize();
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
        // One vertex shader; two pixel shaders built from the same source. The alpha-tested one is compiled with
        // ALPHA_TEST=1, which adds the 'discard'. Opaque materials use the one without it.
        const ShaderCompileResult vs = m_compiler.Compile({ m_shaderPath, L"VSMain", ShaderStage::Vertex });
        const ShaderCompileResult ps = m_compiler.Compile({ m_shaderPath, L"PSMain", ShaderStage::Pixel });
        const ShaderCompileResult psAlphaTest = m_compiler.Compile({ m_shaderPath, L"PSMain", ShaderStage::Pixel, { { L"ALPHA_TEST", L"1" } } });
        const ShaderCompileResult vsShadow = m_compiler.Compile({ m_shaderPath, L"VSShadow", ShaderStage::Vertex });
        if (!vs.Succeeded() || !ps.Succeeded() || !psAlphaTest.Succeeded() || !vsShadow.Succeeded())
        {
            m_lastError = vs.errors + ps.errors + psAlphaTest.errors + vsShadow.errors;
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

        // Every combination of pixel shader and cull mode: [alphaTest][cullNone].
        std::array<std::array<ComPtr<ID3D12PipelineState>, 2>, 2> pipelines;
        HRESULT hr = S_OK;
        for (int alphaTest = 0; alphaTest < 2 && SUCCEEDED(hr); ++alphaTest)
        {
            for (int cullNone = 0; cullNone < 2 && SUCCEEDED(hr); ++cullNone)
            {
                desc.PS = alphaTest ? psAlphaTest.GetBytecode() : ps.GetBytecode();
                desc.RasterizerState.CullMode = cullNone ? D3D12_CULL_MODE_NONE : D3D12_CULL_MODE_BACK;
                hr = m_device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipelines[alphaTest][cullNone]));
                if (SUCCEEDED(hr))
                    SetDebugName(pipelines[alphaTest][cullNone].Get(),
                                 std::format(L"ScenePSO_{}_{}", alphaTest ? L"AlphaTest" : L"Opaque", cullNone ? L"CullNone" : L"CullBack").c_str());
            }
        }

        // Shadow map pipelines: depth only. No pixel shader and no render targets: the rasterizer writes depth by
        // itself, which is the cheapest way to draw. Same culling rules as the scene. [cullNone]
        desc.VS = vsShadow.GetBytecode();
        desc.PS = {};
        desc.NumRenderTargets = 0;
        desc.RTVFormats[0] = DXGI_FORMAT_UNKNOWN;
        desc.DSVFormat = ShadowMap::kDepthFormat;
        std::array<ComPtr<ID3D12PipelineState>, 2> shadowPipelines;
        for (int cullNone = 0; cullNone < 2 && SUCCEEDED(hr); ++cullNone)
        {
            desc.RasterizerState.CullMode = cullNone ? D3D12_CULL_MODE_NONE : D3D12_CULL_MODE_BACK;
            hr = m_device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&shadowPipelines[cullNone]));
            if (SUCCEEDED(hr))
                SetDebugName(shadowPipelines[cullNone].Get(), cullNone ? L"ShadowPSO_CullNone" : L"ShadowPSO_CullBack");
        }
        if (FAILED(hr))
        {
            m_lastError = std::format("CreateGraphicsPipelineState failed (0x{:08X})", static_cast<uint32_t>(hr));
            log::Error("{}", m_lastError);
            return false;
        }

        m_pipelines = std::move(pipelines);
        m_shadowPipelines = std::move(shadowPipelines);
        m_lastError.clear();
        return true;
    }

    void ScenePass::Render(ID3D12GraphicsCommandList* commandList, const SceneViewSettings& settings) const
    {
        PIXScopedEvent(commandList, pix::kColorGpu, L"ScenePass");
        RecordDraws(commandList, settings, false);
    }

    void ScenePass::RenderShadowDepth(ID3D12GraphicsCommandList* commandList, const SceneViewSettings& settings) const
    {
        PIXScopedEvent(commandList, pix::kColorGpu, L"Shadow casters");
        RecordDraws(commandList, settings, true);
    }

    void ScenePass::RecordDraws(ID3D12GraphicsCommandList* commandList, const SceneViewSettings& settings, bool shadow) const
    {
        // The camera and lights come from the frame constants (b2), which the renderer has already bound.
        // Everything else shared by all draws is set once here.
        const SceneDrawConstants drawConstants{
            .vertexBufferIndex = m_vertexSrv.index,
            .instanceBufferIndex = m_instanceSrv.index,
            .materialBufferIndex = m_materialSrv.index,
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
        ID3D12PipelineState* currentPipeline = nullptr;
        for (const DrawItem& draw : m_draws)
        {
            // Double-sided materials are never culled; masked ones use the discarding pixel shader. The draws are
            // sorted by these two flags, so each PSO is set once.
            const bool alphaTest = settings.alphaTest && draw.alphaTest;
            const bool cullNone = !settings.backfaceCulling || draw.doubleSided;
            ID3D12PipelineState* pipeline = shadow ? m_shadowPipelines[cullNone].Get() : m_pipelines[alphaTest][cullNone].Get();
            if (pipeline != currentPipeline)
            {
                commandList->SetPipelineState(pipeline);
                currentPipeline = pipeline;
            }
            commandList->SetGraphicsRoot32BitConstant(RootParam::DrawConstants, draw.instanceIndex, kInstanceIndexConstant);
            // BaseVertexLocation = 0: the loader already added each primitive's first vertex to its indices.
            commandList->DrawIndexedInstanced(draw.indexCount, 1, draw.firstIndex, 0, 0);
        }
    }
}
