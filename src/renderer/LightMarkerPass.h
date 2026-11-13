#pragma once

#include "gfx/D3D12Common.h"
#include "gfx/PipelineBuilder.h"

#include <cstdint>
#include <filesystem>
#include <string>

namespace walnut
{
    class Device;
    class ShaderCompiler;

    // Draws a small glowing billboard at every point light (shaders/lightmarkers.hlsl), on top of the shaded scene
    // but hidden behind closer geometry.
    class LightMarkerPass : public ShaderReloadable
    {
    public:
        LightMarkerPass(Device& device, const ShaderCompiler& compiler, std::filesystem::path shaderPath,
                        ID3D12RootSignature* rootSignature, DXGI_FORMAT colorFormat, DXGI_FORMAT depthFormat);


        // Expects the bindless state and frame constants (with the point light buffer) to be bound, and
        // Renderer::BeginOverlayPass.
        void Render(ID3D12GraphicsCommandList* commandList, uint32_t lightCount, float markerRadius) const;

    private:
        void BuildPipelines(PipelineBuilder& builder) override;

        DXGI_FORMAT m_colorFormat;
        DXGI_FORMAT m_depthFormat;
        ComPtr<ID3D12PipelineState> m_pipeline;
    };
}
