#include "renderer/DeferredPass.h"

#include "core/Log.h"
#include "gfx/Device.h"
#include "gfx/GpuUtils.h"
#include "gfx/Pix.h"
#include "gfx/RootSignature.h"
#include "gfx/ShaderCompiler.h"
#include "renderer/ScenePass.h"

namespace walnut
{
    namespace
    {
        // Root constants for deferred.hlsl (b0). Must match DrawConstants there.
        struct DeferredDrawConstants
        {
            DeferredInputs inputs;
            uint32_t debugView; // SceneDebugView
        };
        static_assert(sizeof(DeferredDrawConstants) <= kNumDrawConstants * sizeof(uint32_t));
    }

    DeferredPass::DeferredPass(Device& device, const ShaderCompiler& compiler, std::filesystem::path shaderPath,
                               ID3D12RootSignature* rootSignature, DXGI_FORMAT outputFormat, bool raytracingSupported)
        : m_device(device.Get()), m_compiler(compiler), m_shaderPath(std::move(shaderPath)),
          m_rootSignature(rootSignature), m_outputFormat(outputFormat), m_raytracingSupported(raytracingSupported)
    {
        if (!ReloadShaders())
            throw std::runtime_error("Failed to build deferred pipeline:\n" + m_lastError);
    }

    bool DeferredPass::ReloadShaders()
    {
        const ShaderCompileResult vs = m_compiler.Compile({ m_shaderPath, L"VSMain", ShaderStage::Vertex });
        const ShaderCompileResult ps = m_compiler.Compile({ m_shaderPath, L"PSMain", ShaderStage::Pixel });
        ShaderCompileResult psRt;
        if (m_raytracingSupported)
            psRt = m_compiler.Compile({ m_shaderPath, L"PSMain", ShaderStage::Pixel, { { L"RAYTRACED_SHADOWS", L"1" } } });
        if (!vs.Succeeded() || !ps.Succeeded() || (m_raytracingSupported && !psRt.Succeeded()))
        {
            m_lastError = vs.errors + ps.errors + psRt.errors;
            return false;
        }

        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = m_rootSignature;
        desc.VS = vs.GetBytecode();
        desc.PS = ps.GetBytecode();
        desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        desc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        desc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        desc.DepthStencilState.DepthEnable = FALSE; // reads the depth buffer as a texture instead
        desc.DSVFormat = DXGI_FORMAT_UNKNOWN;
        desc.SampleMask = UINT_MAX;
        desc.NumRenderTargets = 1;
        desc.RTVFormats[0] = m_outputFormat;
        desc.SampleDesc.Count = 1;

        ComPtr<ID3D12PipelineState> pipeline;
        ComPtr<ID3D12PipelineState> rtPipeline;
        HRESULT hr = m_device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline));
        if (SUCCEEDED(hr) && m_raytracingSupported)
        {
            desc.PS = psRt.GetBytecode();
            hr = m_device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&rtPipeline));
        }
        if (FAILED(hr))
        {
            m_lastError = std::format("CreateGraphicsPipelineState failed (0x{:08X})", static_cast<uint32_t>(hr));
            log::Error("{}", m_lastError);
            return false;
        }
        SetDebugName(pipeline.Get(), L"DeferredPSO_ShadowMap");
        if (rtPipeline)
            SetDebugName(rtPipeline.Get(), L"DeferredPSO_RtShadow");

        m_pipeline = std::move(pipeline);
        m_rtPipeline = std::move(rtPipeline);
        m_lastError.clear();
        return true;
    }

    void DeferredPass::Render(ID3D12GraphicsCommandList* commandList, const DeferredInputs& inputs, SceneDebugView view,
                              bool raytracedShadows) const
    {
        PIXScopedEvent(commandList, pix::kColorGpu, L"Deferred lighting");

        const DeferredDrawConstants drawConstants{ .inputs = inputs, .debugView = static_cast<uint32_t>(view) };
        commandList->SetPipelineState(raytracedShadows && m_rtPipeline ? m_rtPipeline.Get() : m_pipeline.Get());
        commandList->SetGraphicsRoot32BitConstants(RootParam::DrawConstants, sizeof(drawConstants) / sizeof(uint32_t),
                                                   &drawConstants, 0);
        commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        commandList->DrawInstanced(3, 1, 0, 0); // one triangle covering the screen
    }
}
