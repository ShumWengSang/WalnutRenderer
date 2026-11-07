#include "renderer/TrianglePass.h"

#include "core/Log.h"
#include "gfx/DescriptorHeap.h"
#include "gfx/Device.h"
#include "gfx/FrameAllocator.h"
#include "gfx/GpuUtils.h"
#include "gfx/Pix.h"
#include "gfx/RootSignature.h"
#include "gfx/ShaderCompiler.h"
#include "gfx/UploadContext.h"

#include <algorithm>
#include <iterator>
#include <span>
#include <vector>

namespace walnut
{
    namespace
    {
        // Must match struct Vertex in triangle.hlsl (StructuredBuffer elements use plain C-like packing:
        // 8 floats = 32 bytes, no 16-byte padding rules like cbuffers have).
        struct Vertex
        {
            float position[3];
            float color[3];
            float uv[2]; // texture coordinates: (0,0) = top-left of the texture, (1,1) = bottom-right
        };
        static_assert(sizeof(Vertex) == 32);

        constexpr Vertex kTriangleVertices[] = {
            { {  0.0f,   0.5f, 0.0f }, { 1.0f, 0.0f, 0.0f }, { 0.5f, 0.0f } },
            { {  0.433f, -0.25f, 0.0f }, { 0.0f, 1.0f, 0.0f }, { 1.0f, 1.0f } },
            { { -0.433f, -0.25f, 0.0f }, { 0.0f, 0.0f, 1.0f }, { 0.0f, 1.0f } },
        };

        constexpr uint32_t kCheckerSize = 256;  // texels per side
        constexpr uint32_t kCheckerSquare = 32; // texels per checker square -> 8x8 squares

        // Constant buffer data for triangle.hlsl. Must match the cbuffer layout there.
        struct TriangleGpuConstants
        {
            DirectX::XMFLOAT4X4 worldViewProjection;
            float tint[4];
        };

        // Root constants for triangle.hlsl (b0). Must match DrawConstants there; at most kNumDrawConstants uints.
        struct TriangleDrawConstants
        {
            uint32_t vertexBufferIndex; // descriptor heap index of the vertex buffer's SRV
            uint32_t textureIndex;      // descriptor heap index of the checkerboard texture's SRV
        };
        static_assert(sizeof(TriangleDrawConstants) <= kNumDrawConstants * sizeof(uint32_t));
    }

    TrianglePass::TrianglePass(Device& device, UploadContext& upload, DescriptorHeap& srvHeap, const ShaderCompiler& compiler,
                               std::filesystem::path shaderPath, ID3D12RootSignature* rootSignature,
                               DXGI_FORMAT rtvFormat, DXGI_FORMAT dsvFormat)
        : m_device(device.Get()), m_srvHeap(srvHeap), m_compiler(compiler), m_shaderPath(std::move(shaderPath)),
          m_rootSignature(rootSignature), m_rtvFormat(rtvFormat), m_dsvFormat(dsvFormat)
    {
        CreateVertexBuffer(upload);
        CreateCheckerTexture(upload);
        if (!ReloadShaders())
            throw std::runtime_error("Failed to build triangle pipeline:\n" + m_lastError);
    }

    TrianglePass::~TrianglePass()
    {
        m_srvHeap.Free(m_vertexBufferSrv);
        m_srvHeap.Free(m_checkerSrv);
    }

    void TrianglePass::CreateCheckerTexture(UploadContext& upload)
    {
        // Generate the pixels on the CPU: 4 bytes per texel (R, G, B, A), rows tightly packed.
        std::vector<uint8_t> pixels(kCheckerSize * kCheckerSize * 4);
        for (uint32_t y = 0; y < kCheckerSize; ++y)
        {
            for (uint32_t x = 0; x < kCheckerSize; ++x)
            {
                const bool light = ((x / kCheckerSquare) + (y / kCheckerSquare)) % 2 == 0;
                uint8_t* texel = &pixels[(y * kCheckerSize + x) * 4];
                texel[0] = texel[1] = texel[2] = light ? 230 : 60;
                texel[3] = 255;
            }
        }

        // _SRGB: these byte values were picked by how they *look* (sRGB, like any image file). Sampling through an
        // _SRGB format converts them back to linear light, which is what our lighting math expects.
        m_checkerTexture = Texture(m_device, TextureDesc{
            .width = kCheckerSize,
            .height = kCheckerSize,
            .mipLevels = 1,
            .format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
            .initialLayout = D3D12_BARRIER_LAYOUT_COPY_DEST,
            .name = L"CheckerTexture",
        });
        const TextureSubresourceData mip0{ .data = pixels.data(), .rowPitch = kCheckerSize * 4 };
        upload.UploadTexture(m_checkerTexture, std::span(&mip0, 1));

        m_checkerSrv = m_srvHeap.Allocate();
        m_checkerTexture.CreateSrv(m_device, m_checkerSrv.cpu);
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

        // Describe the buffer as a StructuredBuffer<Vertex> and write that description (the SRV) into a slot of
        // the shared, shader-visible heap. The slot's index is all a shader needs to find the buffer.
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = DXGI_FORMAT_UNKNOWN; // structured buffers have no format; elements are 'StructureByteStride' bytes
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Buffer.FirstElement = 0;
        srvDesc.Buffer.NumElements = static_cast<UINT>(std::size(kTriangleVertices));
        srvDesc.Buffer.StructureByteStride = sizeof(Vertex);
        srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;

        m_vertexBufferSrv = m_srvHeap.Allocate();
        m_device->CreateShaderResourceView(m_vertexBuffer.Get(), &srvDesc, m_vertexBufferSrv.cpu);
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

        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = m_rootSignature;
        desc.VS = vs.GetBytecode();
        desc.PS = ps.GetBytecode();
        desc.InputLayout = { nullptr, 0 }; // no input layout: the vertex shader fetches its own vertices
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

        // b0: tell the shader where its vertices are. No IASetVertexBuffers: the shader reads them itself.
        const TriangleDrawConstants drawConstants{
            .vertexBufferIndex = m_vertexBufferSrv.index,
            .textureIndex = m_checkerSrv.index,
        };
        commandList->SetGraphicsRoot32BitConstants(RootParam::DrawConstants, sizeof(drawConstants) / sizeof(uint32_t),
                                                   &drawConstants, 0);

        // The topology is still needed: it tells the rasterizer how to group the 3 vertices into a triangle.
        commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        commandList->DrawInstanced(3, 1, 0, 0); // runs the vertex shader with SV_VertexID = 0, 1, 2
    }
}
