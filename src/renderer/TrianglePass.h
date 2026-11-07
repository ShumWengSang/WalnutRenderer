#pragma once

#include "gfx/Buffer.h"
#include "gfx/D3D12Common.h"
#include "gfx/DescriptorHeap.h"

#include <DirectXMath.h>

#include <filesystem>
#include <string>

namespace walnut
{
    class Device;
    class FrameAllocator;
    class ShaderCompiler;
    class UploadContext;

    // How the triangle is placed in the world (edited by the UI and input). TrianglePass turns this into
    // a world matrix and combines it with the camera's view-projection.
    struct TriangleParams
    {
        float tint[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        float offset[3] = { 0.0f, 0.0f, 0.0f }; // world units
        float angle = 0.0f;                     // radians around +Z
        float scale = 1.0f;
    };

    // The classic first D3D12 draw, done the bindless way: the vertex shader pulls its vertices from a
    // StructuredBuffer found through the descriptor heap (no input layout, no vertex buffer binding).
    class TrianglePass
    {
    public:
        // The vertex upload is only recorded here; the caller must Flush() the UploadContext before drawing.
        TrianglePass(Device& device, UploadContext& upload, DescriptorHeap& srvHeap, const ShaderCompiler& compiler,
                     std::filesystem::path shaderPath, ID3D12RootSignature* rootSignature,
                     DXGI_FORMAT rtvFormat, DXGI_FORMAT dsvFormat);
        ~TrianglePass();

        TrianglePass(const TrianglePass&) = delete;
        TrianglePass& operator=(const TrianglePass&) = delete;

        // Recompiles the shaders and rebuilds the PSO. On failure the previous PSO is kept.
        // The old PSO is released on success, so the GPU must be idle before calling this.
        bool ReloadShaders();
        const std::string& GetLastError() const { return m_lastError; }

        // Expects the bindless state (descriptor heap + root signature) to be bound already.
        void Render(ID3D12GraphicsCommandList* commandList, FrameAllocator& frameAllocator,
                    const TriangleParams& params, DirectX::FXMMATRIX viewProjection, bool depthTest) const;

    private:
        void CreateVertexBuffer(UploadContext& upload);

        ID3D12Device10* m_device = nullptr;
        DescriptorHeap& m_srvHeap;
        const ShaderCompiler& m_compiler;
        std::filesystem::path m_shaderPath;
        ID3D12RootSignature* m_rootSignature = nullptr; // owned by the Renderer
        DXGI_FORMAT m_rtvFormat;
        DXGI_FORMAT m_dsvFormat;

        ComPtr<ID3D12PipelineState> m_depthTestPipeline;
        ComPtr<ID3D12PipelineState> m_noDepthPipeline;
        Buffer m_vertexBuffer;
        DescriptorHandle m_vertexBufferSrv; // the vertex buffer's slot in the bindless heap
        std::string m_lastError;
    };
}
