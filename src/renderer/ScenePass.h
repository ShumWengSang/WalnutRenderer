#pragma once

#include "gfx/Buffer.h"
#include "gfx/D3D12Common.h"
#include "gfx/DescriptorHeap.h"
#include "gfx/Texture.h"

#include <DirectXMath.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace walnut
{
    class Device;
    class ShaderCompiler;
    class UploadContext;
    struct Scene;

    // What the scene shader outputs. Values must match scene.hlsl.
    enum class SceneDebugView : uint32_t
    {
        Lit = 0,          // base color lit by the sun (Lambert diffuse) plus ambient
        LightingOnly = 1, // the same lighting on a white surface: shows the light without the textures
        BaseColor = 2,    // the material's base color: texture * factor (unlit)
        Normals = 3,      // world-space normal, mapped from -1..1 to 0..1 colors
        Texcoords = 4,    // UV as red/green
        Instances = 5,    // a random color per instance (= per draw)
        Materials = 6,    // a random color per material
        AlphaMode = 7,    // opaque gray, mask yellow, blend red; back faces of double-sided materials darker
        SunShadow = 8,    // the shadow map test: white = the sun reaches this point, black = blocked
        ShadowDifference = 9, // ray traced vs shadow map: red = only the map shadows, blue = only the ray shadows
        Roughness = 10,   // metallic-roughness map G * roughnessFactor (black = mirror, white = matte)
        Metallic = 11,    // metallic-roughness map B * metallicFactor (white = metal)
        NormalMap = 12,   // the raw tangent-space normal map (light blue = flat)
        Occlusion = 13,   // ambient occlusion map, scaled by its strength
        Emissive = 14,    // emitted light: emissive map * factor
        SunSpecular = 15, // only the sun's GGX specular reflection (the highlights)
        Ambient = 16,     // only the ambient light: sky/ground diffuse + reflection, times occlusion
    };

    // Where the tangent frame for normal mapping comes from. Values must match scene.hlsl.
    enum class NormalMapping : uint32_t
    {
        Off = 0,     // ignore normal maps: the interpolated vertex normal only
        Mesh = 1,    // TBN from the glTF TANGENT attribute (derived per pixel for meshes without one)
        Derived = 2, // TBN always derived per pixel from position/uv screen-space derivatives
    };

    struct SceneViewSettings
    {
        SceneDebugView debugView = SceneDebugView::Lit;
        NormalMapping normalMapping = NormalMapping::Mesh;
        // Negative: use the material's value. 0..1: every material uses this instead (sent as frame constants).
        float roughnessOverride = -1.0f;
        float metallicOverride = -1.0f;
        bool backfaceCulling = true; // for single-sided materials; double-sided ones are never culled
        bool alphaTest = true;       // cut out alpha-mask materials; off draws them as opaque, to compare
        bool raytracedShadows = false; // sun shadows from ray queries instead of the shadow map (if supported)
    };

    // Draws a Scene: uploads its merged vertex/index arrays and an instance buffer once, then issues one
    // DrawIndexedInstanced per instance. The vertex shader pulls vertices and instance data from the bindless heap.
    class ScenePass
    {
    public:
        // The uploads are only recorded here; the caller must Flush() the UploadContext before drawing.
        ScenePass(Device& device, UploadContext& upload, DescriptorHeap& srvHeap, const ShaderCompiler& compiler,
                  std::filesystem::path shaderPath, ID3D12RootSignature* rootSignature,
                  DXGI_FORMAT rtvFormat, DXGI_FORMAT dsvFormat, const Scene& scene);
        ~ScenePass();

        ScenePass(const ScenePass&) = delete;
        ScenePass& operator=(const ScenePass&) = delete;

        // Recompiles the shaders and rebuilds the PSOs. On failure the previous PSOs are kept.
        // The old PSOs are released on success, so the GPU must be idle before calling this.
        bool ReloadShaders();
        const std::string& GetLastError() const { return m_lastError; }
        bool SupportsRaytracedShadows() const { return m_raytracingSupported; }

        // Expects the bindless state (descriptor heap + root signature) and the frame constants to be bound already.
        void Render(ID3D12GraphicsCommandList* commandList, const SceneViewSettings& settings) const;
        // Draws every instance into the bound shadow map: depth only, from the sun (frame constants' sunViewProjection).
        void RenderShadowDepth(ID3D12GraphicsCommandList* commandList, const SceneViewSettings& settings) const;

        uint32_t GetDrawCount() const { return static_cast<uint32_t>(m_draws.size()); }
        uint32_t GetTextureCount() const { return static_cast<uint32_t>(m_textures.size()); }
        uint64_t GetGpuBytes() const;     // buffers
        uint64_t GetTextureBytes() const { return m_textureBytes; } // GPU memory of all textures, mips included
        // The merged geometry, for building ray tracing acceleration structures from it.
        const Buffer& GetVertexBuffer() const { return m_vertexBuffer; }
        const Buffer& GetIndexBuffer() const { return m_indexBuffer; }

    private:
        // Everything one draw call needs: which indices, and which instance (world matrix) to place them with.
        struct DrawItem
        {
            uint32_t indexCount;
            uint32_t firstIndex;
            uint32_t instanceIndex;
            bool doubleSided; // drawn without back-face culling
            bool alphaTest;   // alpha-mask material: drawn with the discarding pixel shader
        };

        Buffer CreateStructuredBuffer(UploadContext& upload, const void* data, uint32_t count, uint32_t stride,
                                      const wchar_t* name, DescriptorHandle& srv);
        // Uploads every loaded scene texture; returns each one's bindless SRV index (kNoTexture if it wasn't loaded).
        std::vector<uint32_t> CreateTextures(UploadContext& upload, const Scene& scene);
        // Binds the geometry and issues every draw, with the scene PSOs or the depth-only shadow PSOs.
        void RecordDraws(ID3D12GraphicsCommandList* commandList, const SceneViewSettings& settings, bool shadow) const;

        ID3D12Device10* m_device = nullptr;
        DescriptorHeap& m_srvHeap;
        const ShaderCompiler& m_compiler;
        std::filesystem::path m_shaderPath;
        ID3D12RootSignature* m_rootSignature = nullptr; // owned by the Renderer
        DXGI_FORMAT m_rtvFormat;
        DXGI_FORMAT m_dsvFormat;

        Buffer m_vertexBuffer;   // StructuredBuffer<SceneVertex>, read by the vertex shader through m_vertexSrv
        Buffer m_instanceBuffer; // StructuredBuffer<Instance>, read through m_instanceSrv
        Buffer m_materialBuffer; // StructuredBuffer<Material>, read by the pixel shader through m_materialSrv
        Buffer m_indexBuffer;    // 32-bit indices, bound the classic way (IASetIndexBuffer)
        DescriptorHandle m_vertexSrv;    // their slots in the bindless heap
        DescriptorHandle m_instanceSrv;
        DescriptorHandle m_materialSrv;
        DescriptorHandle m_indexSrv;
        std::vector<Texture> m_textures;
        std::vector<DescriptorHandle> m_textureSrvs;
        uint64_t m_textureBytes = 0;
        std::vector<DrawItem> m_draws;

        // [raytracedShadows][alphaTest][cullNone]; the ray-traced half stays empty without ray tracing support.
        using PipelineTable = std::array<std::array<std::array<ComPtr<ID3D12PipelineState>, 2>, 2>, 2>;
        PipelineTable m_pipelines;
        std::array<std::array<ComPtr<ID3D12PipelineState>, 2>, 2> m_shadowPipelines; // [alphaTest][cullNone]
        bool m_raytracingSupported = false;
        std::string m_lastError;
    };
}
