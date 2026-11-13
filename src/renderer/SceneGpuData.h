#pragma once

#include "gfx/Buffer.h"
#include "gfx/D3D12Common.h"
#include "gfx/DescriptorHeap.h"
#include "gfx/Texture.h"
#include "scene/Bounds.h"

#include <cstdint>
#include <vector>

namespace walnut
{
    class Device;
    class UploadContext;
    struct Scene;

    // A Scene's data on the GPU, uploaded once: the merged vertex and index arrays, an instance buffer (world
    // matrices), a material buffer and the textures, each with its SRV in the bindless heap, plus one draw per
    // instance. ScenePass draws it; AccelerationStructures builds the ray tracing structures from it.
    class SceneGpuData
    {
    public:
        // Everything one draw call needs: which indices, and which instance (world matrix) to place them with.
        struct DrawItem
        {
            uint32_t indexCount;
            uint32_t firstIndex;
            uint32_t instanceIndex;
            bool doubleSided; // drawn without back-face culling
            bool alphaTest;   // alpha-mask material: drawn with the discarding pixel shader
            Aabb worldBounds; // for culling
        };

        // The bindless SRV indices of the scene's buffers, for shaders that read them (draws, ray hits).
        struct Bindings
        {
            uint32_t vertices;  // StructuredBuffer<Vertex>
            uint32_t instances; // StructuredBuffer<Instance>
            uint32_t materials; // StructuredBuffer<Material>
            uint32_t indices;   // the index buffer as StructuredBuffer<uint>
        };

        // The uploads are only recorded here; the caller must Flush() the UploadContext before drawing.
        SceneGpuData(Device& device, UploadContext& upload, DescriptorHeap& srvHeap, const Scene& scene);
        ~SceneGpuData();

        SceneGpuData(const SceneGpuData&) = delete;
        SceneGpuData& operator=(const SceneGpuData&) = delete;

        // Sorted so that draws using the same PSO are next to each other (see the constructor).
        const std::vector<DrawItem>& GetDraws() const { return m_draws; }
        Bindings GetBindings() const;
        D3D12_INDEX_BUFFER_VIEW GetIndexBufferView() const;
        // The merged geometry, for building ray tracing acceleration structures from it.
        const Buffer& GetVertexBuffer() const { return m_vertexBuffer; }
        const Buffer& GetIndexBuffer() const { return m_indexBuffer; }

        uint32_t GetDrawCount() const { return static_cast<uint32_t>(m_draws.size()); }
        uint32_t GetTextureCount() const { return static_cast<uint32_t>(m_textures.size()); }
        uint64_t GetBufferBytes() const;
        uint64_t GetTextureBytes() const { return m_textureBytes; } // GPU memory of all textures, mips included

    private:
        Buffer CreateStructuredBuffer(UploadContext& upload, const void* data, uint32_t count, uint32_t stride,
                                      const wchar_t* name, OwnedDescriptor& srv);
        // Uploads every loaded scene texture; returns each one's bindless SRV index (kNoTexture if it wasn't loaded).
        std::vector<uint32_t> CreateTextures(UploadContext& upload, const Scene& scene);

        ID3D12Device10* m_device = nullptr;
        DescriptorHeap& m_srvHeap;

        Buffer m_vertexBuffer;   // StructuredBuffer<SceneVertex>, read by the vertex shader through m_vertexSrv
        Buffer m_instanceBuffer; // StructuredBuffer<Instance>, read through m_instanceSrv
        Buffer m_materialBuffer; // StructuredBuffer<Material>, read by the pixel shader through m_materialSrv
        Buffer m_indexBuffer;    // 32-bit indices, bound the classic way (IASetIndexBuffer)
        OwnedDescriptor m_vertexSrv;    // their slots in the bindless heap
        OwnedDescriptor m_instanceSrv;
        OwnedDescriptor m_materialSrv;
        OwnedDescriptor m_indexSrv;
        std::vector<Texture> m_textures;
        std::vector<OwnedDescriptor> m_textureSrvs;
        uint64_t m_textureBytes = 0;
        std::vector<DrawItem> m_draws;
    };
}
