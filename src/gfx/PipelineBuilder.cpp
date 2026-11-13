#include "gfx/PipelineBuilder.h"

#include "core/Log.h"
#include "gfx/GpuUtils.h"

#include <format>
#include <stdexcept>

namespace walnut
{
    PipelineBuilder::PipelineBuilder(ID3D12Device10* device, const ShaderCompiler& compiler,
                                     ID3D12RootSignature* rootSignature, std::filesystem::path shaderPath)
        : m_device(device), m_compiler(compiler), m_rootSignature(rootSignature), m_shaderPath(std::move(shaderPath))
    {
    }

    D3D12_SHADER_BYTECODE PipelineBuilder::Shader(const wchar_t* entryPoint, ShaderStage stage, ShaderDefines defines)
    {
        ShaderCompileResult result = m_compiler.Compile({ m_shaderPath, entryPoint, stage, std::move(defines) });
        m_errors += result.errors;
        if (!result.Succeeded())
        {
            m_failed = true;
            return {};
        }
        const D3D12_SHADER_BYTECODE bytecode = result.GetBytecode();
        m_shaders.push_back(std::move(result.bytecode));
        return bytecode;
    }

    ComPtr<ID3D12PipelineState> PipelineBuilder::Graphics(D3D12_GRAPHICS_PIPELINE_STATE_DESC desc, const std::wstring& debugName)
    {
        if (m_failed)
            return nullptr;
        if (!desc.pRootSignature)
            desc.pRootSignature = m_rootSignature;
        ComPtr<ID3D12PipelineState> pipeline;
        const HRESULT hr = m_device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline));
        if (FAILED(hr))
        {
            const std::string error = std::format("CreateGraphicsPipelineState failed (0x{:08X})", static_cast<uint32_t>(hr));
            log::Error("{}", error);
            m_errors += error;
            m_failed = true;
            return nullptr;
        }
        SetDebugName(pipeline.Get(), debugName.c_str());
        return pipeline;
    }

    ComPtr<ID3D12PipelineState> PipelineBuilder::Compute(D3D12_SHADER_BYTECODE shader, const std::wstring& debugName)
    {
        if (m_failed)
            return nullptr;
        // A compute pipeline is just the root signature and one shader: no rasterizer, blend or depth state.
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = m_rootSignature;
        desc.CS = shader;
        ComPtr<ID3D12PipelineState> pipeline;
        const HRESULT hr = m_device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pipeline));
        if (FAILED(hr))
        {
            const std::string error = std::format("CreateComputePipelineState failed (0x{:08X})", static_cast<uint32_t>(hr));
            log::Error("{}", error);
            m_errors += error;
            m_failed = true;
            return nullptr;
        }
        SetDebugName(pipeline.Get(), debugName.c_str());
        return pipeline;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC FullscreenPassDesc(DXGI_FORMAT renderTargetFormat)
    {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
        desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        desc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        desc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        desc.DepthStencilState.DepthEnable = FALSE; // a 2D pass: no depth buffer
        desc.DSVFormat = DXGI_FORMAT_UNKNOWN;
        desc.SampleMask = UINT_MAX;
        desc.NumRenderTargets = 1;
        desc.RTVFormats[0] = renderTargetFormat;
        desc.SampleDesc.Count = 1;
        return desc;
    }

    ShaderReloadable::ShaderReloadable(const char* name, ID3D12Device10* device, const ShaderCompiler& compiler,
                                       ID3D12RootSignature* rootSignature, std::filesystem::path shaderPath)
        : m_name(name), m_device(device), m_compiler(compiler), m_rootSignature(rootSignature),
          m_shaderPath(std::move(shaderPath))
    {
    }

    bool ShaderReloadable::ReloadShaders()
    {
        PipelineBuilder builder(m_device, m_compiler, m_rootSignature, m_shaderPath);
        BuildPipelines(builder);
        if (builder.Failed())
        {
            m_lastError = builder.GetErrors();
            return false;
        }
        m_lastError.clear();
        return true;
    }

    void ShaderReloadable::BuildPipelinesOrThrow()
    {
        if (!ReloadShaders())
            throw std::runtime_error(std::format("Failed to build {} pipelines:\n{}", m_name, m_lastError));
    }
}
