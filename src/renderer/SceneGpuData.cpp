#include "renderer/SceneGpuData.h"

#include "core/Log.h"
#include "core/StringUtils.h"
#include "gfx/Device.h"
#include "gfx/UploadContext.h"
#include "gfx/Views.h"
#include "scene/Scene.h"

#include <algorithm>

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
            uint32_t firstIndex; // where its triangles start in the index buffer (ray hits look up their UVs from it)
            uint32_t padding[2]; // keeps the stride a multiple of 16 bytes, which GPUs read most efficiently
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
            // Bindless SRV indices, or kNoTexture.
            uint32_t baseColorTexture;
            uint32_t metallicRoughnessTexture;
            uint32_t normalTexture;
            uint32_t occlusionTexture;
            uint32_t emissiveTexture;
            float normalScale;
            float occlusionStrength;
            uint32_t padding;
        };
        static_assert(sizeof(MaterialGpuData) == 80);
    }

    SceneGpuData::SceneGpuData(Device& device, UploadContext& upload, DescriptorHeap& srvHeap, const Scene& scene)
        : m_device(device.Get()), m_srvHeap(srvHeap)
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
            gpu.firstIndex = primitive.firstIndex;
            m_draws.push_back(DrawItem{ primitive.indexCount, primitive.firstIndex, static_cast<uint32_t>(m_draws.size()),
                                        material.doubleSided, material.alphaMode == AlphaMode::Mask, instance.worldBounds });
        }
        // Sorted by PSO: opaque single-sided, opaque double-sided, masked single-sided, masked double-sided. Each PSO
        // is then set once per frame. Opaque goes first on purpose: alpha-tested pixels are slower (see scene.hlsl),
        // and drawing them last lets the depth test reject the ones hidden behind walls before they're shaded.
        std::ranges::stable_sort(m_draws, {}, [](const DrawItem& draw) { return draw.alphaTest * 2 + draw.doubleSided; });

        // Textures first: materials refer to them by their slot in the bindless heap, which is only known once
        // each texture's SRV exists.
        const std::vector<uint32_t> textureSrvIndex = CreateTextures(upload, scene);

        std::vector<MaterialGpuData> materials;
        const auto srv = [&](uint32_t texture) { return texture != kNoTexture ? textureSrvIndex[texture] : kNoTexture; };
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
                .baseColorTexture = srv(material.baseColorTexture),
                .metallicRoughnessTexture = srv(material.metallicRoughnessTexture),
                .normalTexture = srv(material.normalTexture),
                .occlusionTexture = srv(material.occlusionTexture),
                .emissiveTexture = srv(material.emissiveTexture),
                .normalScale = material.normalScale,
                .occlusionStrength = material.occlusionStrength,
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

        // The same index buffer is also readable by shaders: a ray that hits a triangle gets only its number, and
        // must fetch the triangle's 3 vertex indices itself to look up their UVs (alpha-tested shadow rays).
        const D3D12_SHADER_RESOURCE_VIEW_DESC indexSrvDesc =
            StructuredBufferSrv(static_cast<uint32_t>(scene.indices.size()), sizeof(uint32_t));
        m_indexSrv = OwnedDescriptor(m_srvHeap);
        m_device->CreateShaderResourceView(m_indexBuffer.Get(), &indexSrvDesc, m_indexSrv.cpu);

        log::Info("Scene on the GPU: {} draws ({} double-sided, {} alpha-tested), {} materials, {:.1f} MB of buffers, {} textures ({:.1f} MB) on the GPU",
                  m_draws.size(), std::ranges::count_if(m_draws, [](const DrawItem& draw) { return draw.doubleSided; }),
                  std::ranges::count_if(m_draws, [](const DrawItem& draw) { return draw.alphaTest; }),
                  materials.size(), static_cast<double>(GetBufferBytes()) / (1024.0 * 1024.0), m_textures.size(),
                  static_cast<double>(m_textureBytes) / (1024.0 * 1024.0));
    }

    std::vector<uint32_t> SceneGpuData::CreateTextures(UploadContext& upload, const Scene& scene)
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
                .format = source.IsSrgb() ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM,
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

            const OwnedDescriptor& srv = m_textureSrvs.emplace_back(m_srvHeap);
            texture.CreateSrv(m_device, srv.cpu);
            srvIndices[i] = srv.index;
            m_textures.push_back(std::move(texture));
        }
        return srvIndices;
    }

    SceneGpuData::~SceneGpuData() = default;

    uint64_t SceneGpuData::GetBufferBytes() const
    {
        return m_vertexBuffer.GetSize() + m_indexBuffer.GetSize() + m_instanceBuffer.GetSize() + m_materialBuffer.GetSize();
    }

    Buffer SceneGpuData::CreateStructuredBuffer(UploadContext& upload, const void* data, uint32_t count, uint32_t stride,
                                             const wchar_t* name, OwnedDescriptor& srv)
    {
        const uint64_t size = static_cast<uint64_t>(count) * stride;
        Buffer buffer(m_device, BufferDesc{ .size = size, .heapType = D3D12_HEAP_TYPE_DEFAULT, .name = name });
        upload.UploadBuffer(buffer, data, size);

        const D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = StructuredBufferSrv(count, stride);
        srv = OwnedDescriptor(m_srvHeap);
        m_device->CreateShaderResourceView(buffer.Get(), &srvDesc, srv.cpu);
        return buffer;
    }

    SceneGpuData::Bindings SceneGpuData::GetBindings() const
    {
        return { .vertices = m_vertexSrv.index, .instances = m_instanceSrv.index, .materials = m_materialSrv.index,
                 .indices = m_indexSrv.index };
    }

    D3D12_INDEX_BUFFER_VIEW SceneGpuData::GetIndexBufferView() const
    {
        return { .BufferLocation = m_indexBuffer.GetGpuAddress(),
                 .SizeInBytes = static_cast<UINT>(m_indexBuffer.GetSize()),
                 .Format = DXGI_FORMAT_R32_UINT };
    }
}
