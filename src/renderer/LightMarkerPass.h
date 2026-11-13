#pragma once

#include "gfx/D3D12Common.h"

#include <cstdint>
#include <filesystem>
#include <string>

namespace walnut
{
    class Device;
    class ShaderCompiler;

    // Draws a small glowing billboard at every point light (shaders/lightmarkers.hlsl), on top of the shaded scene
    // but hidden behind closer geometry.
    class LightMarkerPass
    {
    public:
        LightMarkerPass(Device& device, const ShaderCompiler& compiler, std::filesystem::path shaderPath,
                        ID3D12RootSignature* rootSignature, DXGI_FORMAT colorFormat, DXGI_FORMAT depthFormat);

        // Recompiles the shaders and rebuilds the PSO. On failure the previous PSO is kept.
        // The old PSO is released on success, so the GPU must be idle before calling this.
        bool ReloadShaders();
        const std::string& GetLastError() const { return m_lastError; }

        // Expects the bindless state and frame constants (with the point light buffer) to be bound, and
        // Renderer::BeginOverlayPass.
        void Render(ID3D12GraphicsCommandList* commandList, uint32_t lightCount, float markerRadius) const;

    private:
        ID3D12Device10* m_device = nullptr;
        const ShaderCompiler& m_compiler;
        std::filesystem::path m_shaderPath;
        ID3D12RootSignature* m_rootSignature = nullptr; // owned by the Renderer
        DXGI_FORMAT m_colorFormat;
        DXGI_FORMAT m_depthFormat;

        ComPtr<ID3D12PipelineState> m_pipeline;
        std::string m_lastError;
    };
}
