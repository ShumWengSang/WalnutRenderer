#pragma once

#include "gfx/Buffer.h"
#include "gfx/D3D12Common.h"

#include <filesystem>
#include <string>

namespace walnut
{
    class Device;
    class FrameAllocator;
    class ShaderCompiler;
    class UploadContext;

    // Constant buffer data for triangle.hlsl. Must match the cbuffer layout there.
    struct TriangleConstants
    {
        float tint[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        float offset[2] = { 0.0f, 0.0f };
        float angle = 0.0f;
        float scale = 1.0f;
        float aspect = 1.0f;
    };
    static_assert(sizeof(TriangleConstants) % 4 == 0);

    // The classic first D3D12 draw: root signature, PSO, vertex buffer, DrawInstanced.
    class TrianglePass
    {
    public:
        // The vertex upload is only recorded here; the caller must Flush() the UploadContext before drawing.
        TrianglePass(Device& device, UploadContext& upload, const ShaderCompiler& compiler,
                     std::filesystem::path shaderPath, DXGI_FORMAT rtvFormat);

        // Recompiles the shaders and rebuilds the PSO. On failure the previous PSO is kept.
        // The old PSO is released on success, so the GPU must be idle before calling this.
        bool ReloadShaders();
        const std::string& GetLastError() const { return m_lastError; }

        void Render(ID3D12GraphicsCommandList* commandList, FrameAllocator& frameAllocator,
                    const TriangleConstants& constants) const;

    private:
        void CreateRootSignature();
        void CreateVertexBuffer(UploadContext& upload);

        ID3D12Device10* m_device = nullptr;
        const ShaderCompiler& m_compiler;
        std::filesystem::path m_shaderPath;
        DXGI_FORMAT m_rtvFormat;

        ComPtr<ID3D12RootSignature> m_rootSignature;
        ComPtr<ID3D12PipelineState> m_pipelineState;
        Buffer m_vertexBuffer;
        D3D12_VERTEX_BUFFER_VIEW m_vertexBufferView{};
        std::string m_lastError;
    };
}
