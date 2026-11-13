#include "renderer/LightMarkerPass.h"

#include "core/Log.h"
#include "gfx/Device.h"
#include "gfx/GpuUtils.h"
#include "gfx/Pix.h"
#include "gfx/RootSignature.h"
#include "gfx/ShaderCompiler.h"

namespace walnut
{
    namespace
    {
        // Root constants for lightmarkers.hlsl (b0). Must match DrawConstants there.
        struct LightMarkerDrawConstants
        {
            float markerRadius;
        };
        static_assert(sizeof(LightMarkerDrawConstants) <= kNumDrawConstants * sizeof(uint32_t));
    }

    LightMarkerPass::LightMarkerPass(Device& device, const ShaderCompiler& compiler, std::filesystem::path shaderPath,
                                     ID3D12RootSignature* rootSignature, DXGI_FORMAT colorFormat, DXGI_FORMAT depthFormat)
        : m_device(device.Get()), m_compiler(compiler), m_shaderPath(std::move(shaderPath)),
          m_rootSignature(rootSignature), m_colorFormat(colorFormat), m_depthFormat(depthFormat)
    {
        if (!ReloadShaders())
            throw std::runtime_error("Failed to build light marker pipeline:\n" + m_lastError);
    }

    bool LightMarkerPass::ReloadShaders()
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
        desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE; // billboards always face the camera anyway
        // Additive: the marker's glow is added to the scene color behind it (dest = src + dest).
        desc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        D3D12_RENDER_TARGET_BLEND_DESC& blend = desc.BlendState.RenderTarget[0];
        blend.BlendEnable = TRUE;
        blend.SrcBlend = D3D12_BLEND_ONE;
        blend.DestBlend = D3D12_BLEND_ONE;
        blend.BlendOp = D3D12_BLEND_OP_ADD;
        blend.SrcBlendAlpha = D3D12_BLEND_ONE;
        blend.DestBlendAlpha = D3D12_BLEND_ONE;
        blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        // Tested against the scene's depth (closer geometry hides a marker), but not written: markers are see-through
        // glows, and nothing after them needs their depth.
        desc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER; // reversed-Z
        desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        desc.DSVFormat = m_depthFormat;
        desc.SampleMask = UINT_MAX;
        desc.NumRenderTargets = 1;
        desc.RTVFormats[0] = m_colorFormat;
        desc.SampleDesc.Count = 1;

        ComPtr<ID3D12PipelineState> pipeline;
        const HRESULT hr = m_device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline));
        if (FAILED(hr))
        {
            m_lastError = std::format("CreateGraphicsPipelineState failed (0x{:08X})", static_cast<uint32_t>(hr));
            log::Error("{}", m_lastError);
            return false;
        }
        SetDebugName(pipeline.Get(), L"LightMarkerPSO");

        m_pipeline = std::move(pipeline);
        m_lastError.clear();
        return true;
    }

    void LightMarkerPass::Render(ID3D12GraphicsCommandList* commandList, uint32_t lightCount, float markerRadius) const
    {
        if (lightCount == 0)
            return;
        PIXScopedEvent(commandList, pix::kColorGpu, L"Light markers");

        const LightMarkerDrawConstants drawConstants{ .markerRadius = markerRadius };
        commandList->SetPipelineState(m_pipeline.Get());
        commandList->SetGraphicsRoot32BitConstants(RootParam::DrawConstants, sizeof(drawConstants) / sizeof(uint32_t),
                                                   &drawConstants, 0);
        commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        // 6 vertices (one square) per light, one instance per light: SV_InstanceID picks the light.
        commandList->DrawInstanced(6, lightCount, 0, 0);
    }
}
