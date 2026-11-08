#pragma once

#include "gfx/Buffer.h"
#include "gfx/D3D12Common.h"
#include "gfx/DescriptorHeap.h"

#include <DirectXMath.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace walnut
{
    class Device;
    class FrameAllocator;
    class ShaderCompiler;
    class UploadContext;
    struct Scene;

    // What the scene shader outputs until textures and lighting exist. Values must match scene.hlsl.
    enum class SceneDebugView : uint32_t
    {
        Normals = 0,         // world-space normal, mapped from -1..1 to 0..1 colors
        Texcoords = 1,       // UV as red/green
        Instances = 2,       // a random color per instance (= per draw)
        Materials = 3,       // a random color per material
        BaseColorFactor = 4, // the material's constant base color
        AlphaMode = 5,       // opaque gray, mask yellow, blend red; back faces of double-sided materials darker
    };

    struct SceneViewSettings
    {
        SceneDebugView debugView = SceneDebugView::Normals;
        bool backfaceCulling = true; // for single-sided materials; double-sided ones are never culled
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

        // Expects the bindless state (descriptor heap + root signature) to be bound already.
        void Render(ID3D12GraphicsCommandList* commandList, FrameAllocator& frameAllocator,
                    DirectX::FXMMATRIX viewProjection, const SceneViewSettings& settings) const;

        uint32_t GetDrawCount() const { return static_cast<uint32_t>(m_draws.size()); }
        uint64_t GetGpuBytes() const;

    private:
        // Everything one draw call needs: which indices, and which instance (world matrix) to place them with.
        struct DrawItem
        {
            uint32_t indexCount;
            uint32_t firstIndex;
            uint32_t instanceIndex;
            bool doubleSided; // drawn without back-face culling
        };

        Buffer CreateStructuredBuffer(UploadContext& upload, const void* data, uint32_t count, uint32_t stride,
                                      const wchar_t* name, DescriptorHandle& srv);

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
        std::vector<DrawItem> m_draws;

        ComPtr<ID3D12PipelineState> m_cullBackPipeline;
        ComPtr<ID3D12PipelineState> m_cullNonePipeline;
        std::string m_lastError;
    };
}
