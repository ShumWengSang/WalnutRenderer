#include "renderer/DeferredPass.h"

#include "gfx/Device.h"
#include "gfx/Pix.h"
#include "gfx/RootSignature.h"
#include "renderer/ScenePass.h"

namespace walnut
{
    namespace
    {
        // The root constants of deferred.hlsl (b0) are the DeferredInputs: must match DrawConstants there.
        static_assert(sizeof(DeferredInputs) <= kNumDrawConstants * sizeof(uint32_t));
    }

    DeferredPass::DeferredPass(Device& device, const ShaderCompiler& compiler, std::filesystem::path shaderPath,
                               ID3D12RootSignature* rootSignature, DXGI_FORMAT outputFormat, bool raytracingSupported)
        : ShaderReloadable("deferred", device.Get(), compiler, rootSignature, std::move(shaderPath)),
          m_outputFormat(outputFormat), m_raytracingSupported(raytracingSupported)
    {
        BuildPipelinesOrThrow();
    }

    void DeferredPass::BuildPipelines(PipelineBuilder& builder)
    {
        // Reads the depth buffer as a texture instead of testing against it.
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = FullscreenPassDesc(m_outputFormat);
        desc.VS = builder.Shader(L"VSMain", ShaderStage::Vertex);
        desc.PS = builder.Shader(L"PSMain", ShaderStage::Pixel);
        ComPtr<ID3D12PipelineState> pipeline = builder.Graphics(desc, L"DeferredPSO_ShadowMap");
        ComPtr<ID3D12PipelineState> rtPipeline;
        if (m_raytracingSupported)
        {
            desc.PS = builder.Shader(L"PSMain", ShaderStage::Pixel, { { L"RAYTRACED_SHADOWS", L"1" } });
            rtPipeline = builder.Graphics(desc, L"DeferredPSO_RtShadow");
        }
        if (builder.Failed())
            return;
        m_pipeline = std::move(pipeline);
        m_rtPipeline = std::move(rtPipeline);
    }
    void DeferredPass::Render(ID3D12GraphicsCommandList* commandList, const DeferredInputs& inputs,
                              bool raytracedShadows) const
    {
        PIXScopedEvent(commandList, pix::kColorGpu, L"Deferred lighting");

        commandList->SetPipelineState(raytracedShadows && m_rtPipeline ? m_rtPipeline.Get() : m_pipeline.Get());
        commandList->SetGraphicsRoot32BitConstants(RootParam::DrawConstants, sizeof(inputs) / sizeof(uint32_t), &inputs, 0);
        commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        commandList->DrawInstanced(3, 1, 0, 0); // one triangle covering the screen
    }
}
