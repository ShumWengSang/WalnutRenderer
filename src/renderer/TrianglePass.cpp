#include "renderer/TrianglePass.h"

#include "core/Log.h"
#include "gfx/Device.h"
#include "gfx/FrameAllocator.h"
#include "gfx/GpuUtils.h"
#include "gfx/Pix.h"
#include "gfx/RootSignature.h"
#include "gfx/ShaderCompiler.h"
#include "gfx/UploadContext.h"

#include <algorithm>
#include <cstddef>
#include <iterator>

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

        // Constant buffer data for triangle.hlsl. Must match the cbuffer layout there.
        struct TriangleGpuConstants
        {
            DirectX::XMFLOAT4X4 worldViewProjection;
            float tint[4];
        };
    }

    TrianglePass::TrianglePass(Device& device, UploadContext& upload, const ShaderCompiler& compiler,
                               std::filesystem::path shaderPath, ID3D12RootSignature* rootSignature,
                               DXGI_FORMAT rtvFormat, DXGI_FORMAT dsvFormat)
        : m_device(device.Get()), m_compiler(compiler), m_shaderPath(std::move(shaderPath)),
          m_rootSignature(rootSignature), m_rtvFormat(rtvFormat), m_dsvFormat(dsvFormat)
    {
        CreateVertexBuffer(upload);
        if (!ReloadShaders())
            throw std::runtime_error("Failed to build triangle pipeline:\n" + m_lastError);
    }

    void TrianglePass::CreateVertexBuffer(UploadContext& upload)
    {
        // Static data lives in a DEFAULT heap (VRAM): fastest for the GPU to read every frame.
        // The CPU can't write DEFAULT memory, so the vertices go through the UploadContext's staging copy.
        const uint64_t size = sizeof(kTriangleVertices);
        m_vertexBuffer = Buffer(m_device, BufferDesc{
            .size = size,
            .heapType = D3D12_HEAP_TYPE_DEFAULT,
            .name = L"TriangleVertexBuffer",
        });
        upload.UploadBuffer(m_vertexBuffer, kTriangleVertices, size);

        m_vertexBufferView.BufferLocation = m_vertexBuffer.GetGpuAddress();
        m_vertexBufferView.SizeInBytes = static_cast<UINT>(size);
        m_vertexBufferView.StrideInBytes = sizeof(Vertex);
    }

    bool TrianglePass::ReloadShaders()
    {
        const ShaderCompileResult vs = m_compiler.Compile({ m_shaderPath, L"VSMain", ShaderStage::Vertex });
        const ShaderCompileResult ps = m_compiler.Compile({ m_shaderPath, L"PSMain", ShaderStage::Pixel });
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
        desc.pRootSignature = m_rootSignature;
        desc.VS = vs.GetBytecode();
        desc.PS = ps.GetBytecode();
        desc.InputLayout = { inputLayout, _countof(inputLayout) };
        desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        desc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        desc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        desc.SampleMask = UINT_MAX;
        desc.NumRenderTargets = 1;
        desc.RTVFormats[0] = m_rtvFormat;
        desc.DSVFormat = m_dsvFormat; // must match the bound depth buffer
        desc.SampleDesc.Count = 1;

        // Depth test: a pixel is drawn only if its depth is GREATER than what's in the depth buffer (reversed-Z:
        // greater = closer), and then its depth is written so later, farther pixels behind it are rejected.
        desc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        desc.DepthStencilState.DepthEnable = TRUE;
        desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER;
        desc.DepthStencilState.StencilEnable = FALSE;

        ComPtr<ID3D12PipelineState> depthTestPipeline;
        HRESULT hr = m_device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&depthTestPipeline));

        // Same pipeline without the depth test, so the UI can show what depth testing changes. Depth is part
        // of the PSO, so switching it at runtime means switching PSOs. A PSO that doesn't use depth declares
        // DSVFormat UNKNOWN (the debug layer warns otherwise); it then ignores whatever depth buffer is bound.
        desc.DepthStencilState.DepthEnable = FALSE;
        desc.DSVFormat = DXGI_FORMAT_UNKNOWN;
        ComPtr<ID3D12PipelineState> noDepthPipeline;
        if (SUCCEEDED(hr))
            hr = m_device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&noDepthPipeline));

        if (FAILED(hr))
        {
            m_lastError = std::format("CreateGraphicsPipelineState failed (0x{:08X})", static_cast<uint32_t>(hr));
            log::Error("{}", m_lastError);
            return false;
        }
        SetDebugName(depthTestPipeline.Get(), L"TrianglePSO_DepthTest");
        SetDebugName(noDepthPipeline.Get(), L"TrianglePSO_NoDepth");

        m_depthTestPipeline = std::move(depthTestPipeline);
        m_noDepthPipeline = std::move(noDepthPipeline);
        m_lastError.clear();
        return true;
    }

    void TrianglePass::Render(ID3D12GraphicsCommandList* commandList, FrameAllocator& frameAllocator,
                              const TriangleParams& params, DirectX::FXMMATRIX viewProjection, bool depthTest) const
    {
        using namespace DirectX;
        PIXScopedEvent(commandList, pix::kColorGpu, L"TrianglePass");

        // Object -> world: scale, then rotate, then move (row vectors apply left to right).
        const XMMATRIX world = XMMatrixScaling(params.scale, params.scale, params.scale)
                             * XMMatrixRotationZ(params.angle)
                             * XMMatrixTranslation(params.offset[0], params.offset[1], params.offset[2]);

        TriangleGpuConstants constants{};
        XMStoreFloat4x4(&constants.worldViewProjection, world * viewProjection);
        std::copy(std::begin(params.tint), std::end(params.tint), constants.tint);

        // Copy this frame's constants into this frame's region of upload memory, then point b1 (the per-draw
        // root CBV of the shared root signature) at them. The root signature and descriptor heap are already
        // bound by Renderer::BindBindlessState.
        const D3D12_GPU_VIRTUAL_ADDRESS constantsAddress = frameAllocator.AllocateConstants(constants);

        commandList->SetPipelineState(depthTest ? m_depthTestPipeline.Get() : m_noDepthPipeline.Get());
        commandList->SetGraphicsRootConstantBufferView(RootParam::DrawData, constantsAddress);
        commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        commandList->IASetVertexBuffers(0, 1, &m_vertexBufferView);
        commandList->DrawInstanced(3, 1, 0, 0);
    }
}
