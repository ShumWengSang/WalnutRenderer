#include "renderer/TonemapPass.h"

#include "core/Log.h"
#include "gfx/Device.h"
#include "gfx/GpuUtils.h"
#include "gfx/Pix.h"
#include "gfx/RootSignature.h"
#include "gfx/ShaderCompiler.h"

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
        : m_device(device.Get()), m_compiler(compiler), m_shaderPath(std::move(shaderPath)),
          m_rootSignature(rootSignature), m_outputFormat(outputFormat)
    {
        if (!ReloadShaders())
            throw std::runtime_error("Failed to build tonemap pipeline:\n" + m_lastError);
    }

    bool TonemapPass::ReloadShaders()
    {
        const ShaderCompileResult vs = m_compiler.Compile({ m_shaderPath, L"VSMain", ShaderStage::Vertex });
        const ShaderCompileResult ps = m_compiler.Compile({ m_shaderPath, L"PSMain", ShaderStage::Pixel });
        if (!vs.Succeeded() || !ps.Succeeded())
        {
            m_lastError = vs.errors + ps.errors;
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
        desc.DepthStencilState.DepthEnable = FALSE; // a 2D pass: no depth buffer
        desc.DSVFormat = DXGI_FORMAT_UNKNOWN;
        desc.SampleMask = UINT_MAX;
        desc.NumRenderTargets = 1;
        desc.RTVFormats[0] = m_outputFormat;
        desc.SampleDesc.Count = 1;

        ComPtr<ID3D12PipelineState> pipeline;
        const HRESULT hr = m_device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline));
        if (FAILED(hr))
        {
            m_lastError = std::format("CreateGraphicsPipelineState failed (0x{:08X})", static_cast<uint32_t>(hr));
            log::Error("{}", m_lastError);
            return false;
        }
        SetDebugName(pipeline.Get(), L"TonemapPSO");

        m_pipeline = std::move(pipeline);
        m_lastError.clear();
        return true;
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
