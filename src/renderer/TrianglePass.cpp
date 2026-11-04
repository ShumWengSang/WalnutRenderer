#include "renderer/TrianglePass.h"

#include "core/Log.h"
#include "gfx/Device.h"
#include "gfx/GpuUtils.h"
#include "gfx/Pix.h"
#include "gfx/ShaderCompiler.h"

#include <cstddef>
#include <cstring>

namespace walnut
{
    namespace
    {
        struct Vertex
        {
            float position[3];
            float color[3];
        };

        constexpr Vertex kTriangleVertices[] = {
            { {  0.0f,   0.5f, 0.0f }, { 1.0f, 0.0f, 0.0f } },
            { {  0.433f, -0.25f, 0.0f }, { 0.0f, 1.0f, 0.0f } },
            { { -0.433f, -0.25f, 0.0f }, { 0.0f, 0.0f, 1.0f } },
        };

        constexpr UINT kNumRootConstants = sizeof(TriangleConstants) / 4;
    }

    TrianglePass::TrianglePass(Device& device, const ShaderCompiler& compiler, std::filesystem::path shaderPath, DXGI_FORMAT rtvFormat)
        : m_device(device.Get()), m_compiler(compiler), m_shaderPath(std::move(shaderPath)), m_rtvFormat(rtvFormat)
    {
        CreateRootSignature();
        CreateVertexBuffer();
        if (!ReloadShaders())
            throw std::runtime_error("Failed to build triangle pipeline:\n" + m_lastError);
    }

    void TrianglePass::CreateRootSignature()
    {
        // One root parameter: TriangleConstants as 32-bit root constants bound to b0.
        CD3DX12_ROOT_PARAMETER1 parameters[1];
        parameters[0].InitAsConstants(kNumRootConstants, 0, 0, D3D12_SHADER_VISIBILITY_ALL);

        CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC desc;
        desc.Init_1_1(_countof(parameters), parameters, 0, nullptr,
                      D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

        ComPtr<ID3DBlob> serialized;
        ComPtr<ID3DBlob> errors;
        const HRESULT hr = D3DX12SerializeVersionedRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1_1, &serialized, &errors);
        if (FAILED(hr) && errors)
            log::Error("Root signature serialization failed: {}", static_cast<const char*>(errors->GetBufferPointer()));
        WR_CHECK(hr);

        WR_CHECK(m_device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                               IID_PPV_ARGS(&m_rootSignature)));
        SetDebugName(m_rootSignature.Get(), L"TriangleRootSignature");
    }

    void TrianglePass::CreateVertexBuffer()
    {
        // An upload-heap buffer is CPU-writable and GPU-readable. Fine for tiny static data;
        // larger meshes should be copied into a DEFAULT heap.
        const uint64_t size = sizeof(kTriangleVertices);
        m_vertexBuffer = Buffer(m_device, BufferDesc{
            .size = size,
            .heapType = D3D12_HEAP_TYPE_UPLOAD,
            .name = L"TriangleVertexBuffer",
        });
        std::memcpy(m_vertexBuffer.GetMappedData(), kTriangleVertices, size);

        m_vertexBufferView.BufferLocation = m_vertexBuffer.GetGpuAddress();
        m_vertexBufferView.SizeInBytes = static_cast<UINT>(size);
        m_vertexBufferView.StrideInBytes = sizeof(Vertex);
    }

    bool TrianglePass::ReloadShaders()
    {
        const ShaderCompileResult vs = m_compiler.Compile({ m_shaderPath, L"VSMain", L"vs_6_0" });
        const ShaderCompileResult ps = m_compiler.Compile({ m_shaderPath, L"PSMain", L"ps_6_0" });
        if (!vs.Succeeded() || !ps.Succeeded())
        {
            m_lastError = vs.errors + ps.errors;
            return false;
        }

        const D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(Vertex, position), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "COLOR",    0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(Vertex, color),    D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        };

        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = m_rootSignature.Get();
        desc.VS = vs.GetBytecode();
        desc.PS = ps.GetBytecode();
        desc.InputLayout = { inputLayout, _countof(inputLayout) };
        desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        desc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        desc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        desc.DepthStencilState.DepthEnable = FALSE;
        desc.DepthStencilState.StencilEnable = FALSE;
        desc.SampleMask = UINT_MAX;
        desc.NumRenderTargets = 1;
        desc.RTVFormats[0] = m_rtvFormat;
        desc.SampleDesc.Count = 1;

        ComPtr<ID3D12PipelineState> pipelineState;
        const HRESULT hr = m_device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipelineState));
        if (FAILED(hr))
        {
            m_lastError = std::format("CreateGraphicsPipelineState failed (0x{:08X})", static_cast<uint32_t>(hr));
            log::Error("{}", m_lastError);
            return false;
        }
        SetDebugName(pipelineState.Get(), L"TrianglePSO");

        m_pipelineState = std::move(pipelineState);
        m_lastError.clear();
        return true;
    }

    void TrianglePass::Render(ID3D12GraphicsCommandList* commandList, const TriangleConstants& constants) const
    {
        PIXScopedEvent(commandList, pix::kColorGpu, L"TrianglePass");
        commandList->SetGraphicsRootSignature(m_rootSignature.Get());
        commandList->SetPipelineState(m_pipelineState.Get());
        commandList->SetGraphicsRoot32BitConstants(0, kNumRootConstants, &constants, 0);
        commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        commandList->IASetVertexBuffers(0, 1, &m_vertexBufferView);
        commandList->DrawInstanced(3, 1, 0, 0);
    }
}
