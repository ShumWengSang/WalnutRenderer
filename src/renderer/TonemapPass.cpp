#include "renderer/TonemapPass.h"

#include "gfx/Device.h"
#include "gfx/Pix.h"
#include "gfx/RootSignature.h"

#include <cmath>

namespace walnut
{
    namespace
    {
        // Root constants for tonemap.hlsl (b0). Must match DrawConstants there. Root constants are just 32-bit
        // values; the shader decides whether each one is a uint or a float.
        struct TonemapDrawConstants
        {
            uint32_t sceneColorIndex; // descriptor heap slot of the scene color texture's SRV
            float exposure;           // linear multiplier, 2^stops
            uint32_t op;              // TonemapOperator
        };
        static_assert(sizeof(TonemapDrawConstants) <= kNumDrawConstants * sizeof(uint32_t));
    }

    TonemapPass::TonemapPass(Device& device, const ShaderCompiler& compiler, std::filesystem::path shaderPath,
                             ID3D12RootSignature* rootSignature, DXGI_FORMAT outputFormat)
        : ShaderReloadable("tonemap", device.Get(), compiler, rootSignature, std::move(shaderPath)),
          m_outputFormat(outputFormat)
    {
        BuildPipelinesOrThrow();
    }

    void TonemapPass::BuildPipelines(PipelineBuilder& builder)
    {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = FullscreenPassDesc(m_outputFormat);
        desc.VS = builder.Shader(L"VSMain", ShaderStage::Vertex);
        desc.PS = builder.Shader(L"PSMain", ShaderStage::Pixel);
        ComPtr<ID3D12PipelineState> pipeline = builder.Graphics(desc, L"TonemapPSO");
        if (!builder.Failed())
            m_pipeline = std::move(pipeline);
    }

    void TonemapPass::Render(ID3D12GraphicsCommandList* commandList, uint32_t sceneColorSrvIndex,
                             const TonemapSettings& settings) const
    {
        PIXScopedEvent(commandList, pix::kColorGpu, L"Tonemap");

        const TonemapDrawConstants drawConstants{
            .sceneColorIndex = sceneColorSrvIndex,
            .exposure = std::exp2(settings.exposureStops),
            .op = static_cast<uint32_t>(settings.op),
        };
        commandList->SetPipelineState(m_pipeline.Get());
        commandList->SetGraphicsRoot32BitConstants(RootParam::DrawConstants, sizeof(drawConstants) / sizeof(uint32_t),
                                                   &drawConstants, 0);
        commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        // One triangle big enough to cover the whole screen; the vertex shader makes up its corners from
        // SV_VertexID, so there is no vertex buffer at all.
        commandList->DrawInstanced(3, 1, 0, 0);
    }
}
