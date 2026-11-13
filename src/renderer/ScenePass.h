#pragma once

#include "gfx/Buffer.h"
#include "gfx/D3D12Common.h"
#include "gfx/DescriptorHeap.h"
#include "gfx/PipelineBuilder.h"
#include "gfx/Texture.h"
#include "renderer/DebugViews.h"
#include "shared/Constants.h"

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

    // Where the tangent frame for normal mapping comes from. Values shared with scene.hlsl (shared/Constants.h).
    enum class NormalMapping : uint32_t
    {
        Off = kNormalMappingOff,         // ignore normal maps: the interpolated vertex normal only
        Mesh = kNormalMappingMesh,       // TBN from the glTF TANGENT attribute (derived per pixel for meshes without one)
        Derived = kNormalMappingDerived, // TBN always derived per pixel from position/uv screen-space derivatives
    };

    // How the scene's pixels get their color.
    enum class RenderPath : uint32_t
    {
        Forward,  // one pass: each draw's pixel shader samples the material and lights it
        Deferred, // the draws only write the material to the G-buffer; a fullscreen pass reads it (K2/K3)
    };

    struct SceneViewSettings
    {
        RenderPath renderPath = RenderPath::Forward;
        SceneDebugView debugView = SceneDebugView::Lit;
        NormalMapping normalMapping = NormalMapping::Mesh;
        // Negative: use the material's value. 0..1: every material uses this instead (sent as frame constants).
        float roughnessOverride = -1.0f;
        float metallicOverride = -1.0f;
        bool backfaceCulling = true; // for single-sided materials; double-sided ones are never culled
        bool alphaTest = true;       // cut out alpha-mask materials; off draws them as opaque, to compare
        bool raytracedShadows = false; // sun shadows from ray queries instead of the shadow map (if supported)
        bool raytracedPointShadows = false; // point light shadows from ray queries (also needs the ray-traced variant)
        bool depthPrepass = true;      // draw depth first, then shade each pixel once (depth EQUAL)
    };

    // Draws a Scene: uploads its merged vertex/index arrays and an instance buffer once, then issues one
    // DrawIndexedInstanced per instance. The vertex shader pulls vertices and instance data from the bindless heap.
    class ScenePass : public ShaderReloadable
    {
    public:
        // The uploads are only recorded here; the caller must Flush() the UploadContext before drawing.
        ScenePass(Device& device, UploadContext& upload, DescriptorHeap& srvHeap, const ShaderCompiler& compiler,
                  std::filesystem::path shaderPath, ID3D12RootSignature* rootSignature,
                  DXGI_FORMAT rtvFormat, DXGI_FORMAT dsvFormat, const Scene& scene);
        ~ScenePass();

        ScenePass(const ScenePass&) = delete;
        ScenePass& operator=(const ScenePass&) = delete;

        bool SupportsRaytracedShadows() const { return m_raytracingSupported; }

        // Expects the bindless state (descriptor heap + root signature) and the frame constants to be bound already.
        void Render(ID3D12GraphicsCommandList* commandList, const SceneViewSettings& settings) const;
        // Draws every instance into the bound shadow map: depth only, from the sun (frame constants' sunViewProjection).
        void RenderShadowDepth(ID3D12GraphicsCommandList* commandList, const SceneViewSettings& settings) const;
        // Draws every instance into the bound depth buffer from the camera, depth only. With settings.depthPrepass,
        // Render() must follow with the same settings: it then shades only the fragments matching this depth.
        void RenderDepthPrepass(ID3D12GraphicsCommandList* commandList, const SceneViewSettings& settings) const;
        // Deferred shading: writes every visible pixel's material into the bound G-buffer. Needs the depth pre-pass
        // first (it tests depth EQUAL).
        void RenderGBuffer(ID3D12GraphicsCommandList* commandList, const SceneViewSettings& settings) const;
        // One face of a point light's cube shadow map (M1): depth only, with the face camera's view-projection at b1.
        // Draws only the instances whose bounds reach the light's sphere. Returns how many draws it recorded.
        uint32_t RenderPointShadowDepth(ID3D12GraphicsCommandList* commandList, const SceneViewSettings& settings,
                                        D3D12_GPU_VIRTUAL_ADDRESS faceViewProjection,
                                        const DirectX::XMFLOAT3& lightPosition, float lightRange) const;

        uint32_t GetDrawCount() const { return static_cast<uint32_t>(m_draws.size()); }
        uint32_t GetTextureCount() const { return static_cast<uint32_t>(m_textures.size()); }
        uint64_t GetGpuBytes() const;     // buffers
        uint64_t GetTextureBytes() const { return m_textureBytes; } // GPU memory of all textures, mips included
        // The merged geometry, for building ray tracing acceleration structures from it.
        const Buffer& GetVertexBuffer() const { return m_vertexBuffer; }
        const Buffer& GetIndexBuffer() const { return m_indexBuffer; }
        // Their bindless SRVs, for passes that read the scene outside its own draws (see FrameConstants).
        uint32_t GetVertexSrvIndex() const { return m_vertexSrv.index; }
        uint32_t GetInstanceSrvIndex() const { return m_instanceSrv.index; }
        uint32_t GetMaterialSrvIndex() const { return m_materialSrv.index; }
        uint32_t GetIndexSrvIndex() const { return m_indexSrv.index; }

    private:
        void BuildPipelines(PipelineBuilder& builder) override;

        // Everything one draw call needs: which indices, and which instance (world matrix) to place them with.
        struct DrawItem
        {
            uint32_t indexCount;
            uint32_t firstIndex;
            uint32_t instanceIndex;
            bool doubleSided; // drawn without back-face culling
            bool alphaTest;   // alpha-mask material: drawn with the discarding pixel shader
            DirectX::XMFLOAT3 boundsMin, boundsMax; // world space (the primitive's box, transformed)
        };

        Buffer CreateStructuredBuffer(UploadContext& upload, const void* data, uint32_t count, uint32_t stride,
                                      const wchar_t* name, DescriptorHandle& srv);
        // Uploads every loaded scene texture; returns each one's bindless SRV index (kNoTexture if it wasn't loaded).
        std::vector<uint32_t> CreateTextures(UploadContext& upload, const Scene& scene);
        enum class DrawMode { Scene, ShadowDepth, DepthPrepass, GBuffer, PointShadowDepth };
        // Optional culling: only draws whose bounds touch this sphere.
        struct CullSphere
        {
            DirectX::XMFLOAT3 center;
            float radius;
        };
        // Binds the geometry and issues every draw (or those touching 'cull'), with the PSOs for the given pass.
        // Returns how many draws it recorded.
        uint32_t RecordDraws(ID3D12GraphicsCommandList* commandList, const SceneViewSettings& settings, DrawMode mode,
                             const CullSphere* cull = nullptr) const;

        ID3D12Device10* m_device = nullptr;
        DescriptorHeap& m_srvHeap;
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
        std::array<std::array<ComPtr<ID3D12PipelineState>, 2>, 2> m_prepassPipelines; // [alphaTest][cullNone]
        std::array<std::array<ComPtr<ID3D12PipelineState>, 2>, 2> m_pointShadowPipelines; // [alphaTest][cullNone]
        // Scene pass after a depth pre-pass: depth EQUAL, no depth writes, no discard. [raytraced][cullNone]
        std::array<std::array<ComPtr<ID3D12PipelineState>, 2>, 2> m_equalPipelines;
        std::array<ComPtr<ID3D12PipelineState>, 2> m_gbufferPipelines; // [cullNone]; depth EQUAL, 4 render targets
        bool m_raytracingSupported = false;
    };
}
