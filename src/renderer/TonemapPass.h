#pragma once

#include "gfx/D3D12Common.h"

#include <filesystem>
#include <string>

namespace walnut
{
    class Device;
    class ShaderCompiler;

    // Fullscreen pass that reads the HDR scene color texture and writes the back buffer.
    // For now it copies the color as-is (clamped to 0..1); the tonemapping step turns it into a real tonemapper.
    class TonemapPass
    {
    public:
        TonemapPass(Device& device, const ShaderCompiler& compiler, std::filesystem::path shaderPath,
                    ID3D12RootSignature* rootSignature, DXGI_FORMAT outputFormat);

        // Recompiles the shaders and rebuilds the PSO. On failure the previous PSO is kept.
        // The old PSO is released on success, so the GPU must be idle before calling this.
        bool ReloadShaders();
        const std::string& GetLastError() const { return m_lastError; }

        // Expects the bindless state to be bound and the back buffer to be the render target
        // (Renderer::BeginBackBufferPass).
        void Render(ID3D12GraphicsCommandList* commandList, uint32_t sceneColorSrvIndex) const;

    private:
        ID3D12Device10* m_device = nullptr;
        const ShaderCompiler& m_compiler;
        std::filesystem::path m_shaderPath;
        ID3D12RootSignature* m_rootSignature = nullptr; // owned by the Renderer
        DXGI_FORMAT m_outputFormat;

        ComPtr<ID3D12PipelineState> m_pipeline;
        std::string m_lastError;
    };
}
