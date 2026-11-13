#include "renderer/ScenePass.h"

#include "gfx/Device.h"
#include "gfx/Pix.h"
#include "gfx/RootSignature.h"
#include "renderer/GBuffer.h"
#include "renderer/PointShadowMaps.h"
#include "renderer/SceneGpuData.h"
#include "renderer/ShadowMap.h"

#include <format>
#include <string>

namespace walnut
{
    namespace
    {
        // Root constants (b0). Must match DrawConstants in scene.hlsl.
        struct SceneDrawConstants
        {
            uint32_t vertexBufferIndex;
            uint32_t instanceBufferIndex;
            uint32_t materialBufferIndex;
            uint32_t debugView;
            uint32_t instanceIndex; // the only value that changes between draws
            uint32_t indexBufferIndex; // the index buffer as StructuredBuffer<uint>, for ray hits
            uint32_t normalMapping;    // NormalMapping
        };
        static_assert(sizeof(SceneDrawConstants) <= kNumDrawConstants * sizeof(uint32_t));
        constexpr uint32_t kInstanceIndexConstant = offsetof(SceneDrawConstants, instanceIndex) / sizeof(uint32_t);

        enum class PixelShader
        {
            DepthOnly, // none for opaque draws (the rasterizer writes depth by itself); PSDepthAlphaTest for masked ones
            Lit,       // PSMain: the material, lit (variants: ALPHA_TEST, RAYTRACED_SHADOWS)
            GBuffer,   // PSGBuffer: the material into the G-buffer
        };
        enum class RenderTargets { None, SceneColor, GBuffer };
        enum class DepthTarget { Scene, SunShadowMap, PointShadowMap };

        // How to build the PSOs of one way of drawing the scene. Every recipe gets one PSO per cull mode (back /
        // none), times the variants it has: alpha test (the masked materials' shaders) and ray-traced shadows.
        struct PassRecipe
        {
            ScenePass::Kind kind;
            const wchar_t* name; // debug name prefix
            const wchar_t* vs;
            const wchar_t* vsAlphaTest;
            PixelShader ps;
            D3D12_COMPARISON_FUNC depthFunc;
            bool depthWrite;
            RenderTargets targets;
            DepthTarget depth;
            bool alphaVariant;
            bool raytracedVariant;
        };

        // In ScenePass::Kind order. Reversed-Z: GREATER = closer.
        constexpr PassRecipe kRecipes[] = {
            // The sun's shadow map: depth only. Alpha-tested casters need a small pixel shader that discards the
            // see-through texels of the leaf texture.
            { ScenePass::Kind::SunShadow, L"ShadowPSO", L"VSShadow", L"VSShadowAlphaTest", PixelShader::DepthOnly,
              D3D12_COMPARISON_FUNC_GREATER, true, RenderTargets::None, DepthTarget::SunShadowMap, true, false },
            // A face of a point light's cube shadow map (M1): the same, with the face's camera from b1.
            { ScenePass::Kind::PointShadow, L"PointShadowPSO", L"VSPointShadow", L"VSPointShadowAlphaTest", PixelShader::DepthOnly,
              D3D12_COMPARISON_FUNC_GREATER, true, RenderTargets::None, DepthTarget::PointShadowMap, true, false },
            // The depth pre-pass: the same, from the camera into the scene's depth buffer.
            { ScenePass::Kind::DepthPrepass, L"DepthPrepassPSO", L"VSDepthPrepass", L"VSDepthPrepassAlphaTest", PixelShader::DepthOnly,
              D3D12_COMPARISON_FUNC_GREATER, true, RenderTargets::None, DepthTarget::Scene, true, false },
            // Forward shading without a pre-pass: the usual depth test and write.
            { ScenePass::Kind::Forward, L"ScenePSO", L"VSMain", L"VSMain", PixelShader::Lit,
              D3D12_COMPARISON_FUNC_GREATER, true, RenderTargets::SceneColor, DepthTarget::Scene, true, true },
            // Forward shading after the pre-pass: the depth buffer already holds the closest surface of every pixel,
            // so only fragments with EQUAL depth are shaded (exactly one per pixel), and depth isn't written.
            // Alpha-tested materials don't need 'discard' any more: the pre-pass already cut their holes into the
            // depth buffer, and EQUAL rejects the see-through fragments. So the opaque pixel shader serves both, and
            // early-Z (testing depth before the pixel shader runs) works for everything.
            { ScenePass::Kind::ForwardAfterPrepass, L"ScenePSO_DepthEqual", L"VSMain", L"VSMain", PixelShader::Lit,
              D3D12_COMPARISON_FUNC_EQUAL, false, RenderTargets::SceneColor, DepthTarget::Scene, false, true },
            // The G-buffer (deferred shading): the same EQUAL test after the pre-pass, but the pixel shader writes
            // four render targets instead of one lit color.
            { ScenePass::Kind::GBuffer, L"GBufferPSO", L"VSMain", L"VSMain", PixelShader::GBuffer,
              D3D12_COMPARISON_FUNC_EQUAL, false, RenderTargets::GBuffer, DepthTarget::Scene, false, false },
        };
        static_assert(std::size(kRecipes) == static_cast<size_t>(ScenePass::Kind::Count));
        static_assert([] {
            for (size_t i = 0; i < std::size(kRecipes); ++i)
                if (static_cast<size_t>(kRecipes[i].kind) != i)
                    return false;
            return true;
        }(), "kRecipes must be in ScenePass::Kind order");

        const PassRecipe& RecipeOf(ScenePass::Kind kind) { return kRecipes[static_cast<size_t>(kind)]; }
    }

    ScenePass::ScenePass(Device& device, const ShaderCompiler& compiler, std::filesystem::path shaderPath,
                         ID3D12RootSignature* rootSignature, DXGI_FORMAT rtvFormat, DXGI_FORMAT dsvFormat)
        : ShaderReloadable("scene", device.Get(), compiler, rootSignature, std::move(shaderPath)),
          m_rtvFormat(rtvFormat), m_dsvFormat(dsvFormat), m_raytracingSupported(device.SupportsInlineRaytracing())
    {
        BuildPipelinesOrThrow();
    }

    ScenePass::PipelineKey ScenePass::Normalize(PipelineKey key) const
    {
        // Variants a recipe doesn't have fall back to its plain PSO.
        const PassRecipe& recipe = RecipeOf(key.kind);
        key.alphaTest = key.alphaTest && recipe.alphaVariant;
        key.raytraced = key.raytraced && recipe.raytracedVariant && m_raytracingSupported;
        return key;
    }

    size_t ScenePass::SlotOf(PipelineKey key)
    {
        return ((static_cast<size_t>(key.kind) * 2 + key.raytraced) * 2 + key.alphaTest) * 2 + key.cullNone;
    }

    void ScenePass::BuildPipelines(PipelineBuilder& builder)
    {
        const auto cullName = [](bool cullNone) { return cullNone ? L"CullNone" : L"CullBack"; };
        const auto alphaName = [](bool alphaTest) { return alphaTest ? L"AlphaTest" : L"Opaque"; };
        const auto shadowName = [](bool raytraced) { return raytraced ? L"RtShadow" : L"ShadowMap"; };

        const auto pixelShader = [&](PixelShader ps, bool alphaTest, bool raytraced) -> D3D12_SHADER_BYTECODE {
            switch (ps)
            {
            case PixelShader::DepthOnly:
                return alphaTest ? builder.Shader(L"PSDepthAlphaTest", ShaderStage::Pixel) : D3D12_SHADER_BYTECODE{};
            case PixelShader::GBuffer:
                return builder.Shader(L"PSGBuffer", ShaderStage::Pixel);
            case PixelShader::Lit:
                // One source, compiled per variant: ALPHA_TEST=1 adds the 'discard'; RAYTRACED_SHADOWS=1 adds the
                // RayQuery shadow rays (which needs ray tracing support).
                if (raytraced)
                    return builder.Shader(L"PSMain", ShaderStage::Pixel,
                                          { { L"ALPHA_TEST", alphaTest ? L"1" : L"0" }, { L"RAYTRACED_SHADOWS", L"1" } });
                if (alphaTest)
                    return builder.Shader(L"PSMain", ShaderStage::Pixel, { { L"ALPHA_TEST", L"1" } });
                return builder.Shader(L"PSMain", ShaderStage::Pixel);
            }
            return {};
        };

        PipelineArray pipelines;
        for (const PassRecipe& recipe : kRecipes)
        {
            for (int raytraced = 0; raytraced < (recipe.raytracedVariant && m_raytracingSupported ? 2 : 1); ++raytraced)
            {
                for (int alphaTest = 0; alphaTest < (recipe.alphaVariant ? 2 : 1); ++alphaTest)
                {
                    for (int cullNone = 0; cullNone < 2; ++cullNone)
                    {
                        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
                        desc.VS = builder.Shader(alphaTest ? recipe.vsAlphaTest : recipe.vs, ShaderStage::Vertex);
                        desc.PS = pixelShader(recipe.ps, alphaTest, raytraced);
                        desc.InputLayout = { nullptr, 0 }; // vertex pulling: no input layout
                        desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
                        // glTF's front faces are counter-clockwise, and they still are on screen: the loader mirrored
                        // the data (z negated), but the left-handed projection mirrors it back, so the image isn't
                        // mirrored and keeps its winding. D3D12's default treats clockwise as front, so flip that.
                        // Double-sided materials (and "Back-face culling" off) use the CullNone PSOs.
                        desc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
                        desc.RasterizerState.FrontCounterClockwise = TRUE;
                        desc.RasterizerState.CullMode = cullNone ? D3D12_CULL_MODE_NONE : D3D12_CULL_MODE_BACK;
                        desc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
                        desc.SampleMask = UINT_MAX;
                        desc.SampleDesc.Count = 1;
                        desc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
                        desc.DepthStencilState.DepthFunc = recipe.depthFunc;
                        desc.DepthStencilState.DepthWriteMask = recipe.depthWrite ? D3D12_DEPTH_WRITE_MASK_ALL
                                                                                  : D3D12_DEPTH_WRITE_MASK_ZERO;
                        switch (recipe.targets)
                        {
                        case RenderTargets::None:
                            break;
                        case RenderTargets::SceneColor:
                            desc.NumRenderTargets = 1;
                            desc.RTVFormats[0] = m_rtvFormat;
                            break;
                        case RenderTargets::GBuffer:
                            // The G-buffer's formats, in SV_Target order.
                            desc.NumRenderTargets = GBuffer::kCount;
                            for (uint32_t i = 0; i < GBuffer::kCount; ++i)
                                desc.RTVFormats[i] = GBuffer::kFormats[i];
                            break;
                        }
                        switch (recipe.depth)
                        {
                        case DepthTarget::Scene: desc.DSVFormat = m_dsvFormat; break;
                        case DepthTarget::SunShadowMap: desc.DSVFormat = ShadowMap::kDepthFormat; break;
                        case DepthTarget::PointShadowMap: desc.DSVFormat = PointShadowMaps::kDepthFormat; break;
                        }

                        std::wstring name = recipe.name;
                        if (recipe.raytracedVariant)
                            name += std::format(L"_{}", shadowName(raytraced));
                        if (recipe.alphaVariant)
                            name += std::format(L"_{}", alphaName(alphaTest));
                        name += std::format(L"_{}", cullName(cullNone));
                        const PipelineKey key{ recipe.kind, raytraced != 0, alphaTest != 0, cullNone != 0 };
                        pipelines[SlotOf(key)] = builder.Graphics(desc, name);
                    }
                }
            }
        }
        if (!builder.Failed())
            m_pipelines = std::move(pipelines);
    }

    void ScenePass::Render(ID3D12GraphicsCommandList* commandList, const SceneGpuData& scene,
                           const FrameSettings& frame) const
    {
        PIXScopedEvent(commandList, pix::kColorGpu, L"ScenePass");
        RecordDraws(commandList, scene, frame, frame.depthPrepass ? Kind::ForwardAfterPrepass : Kind::Forward);
    }

    void ScenePass::RenderShadowDepth(ID3D12GraphicsCommandList* commandList, const SceneGpuData& scene,
                                      const FrameSettings& frame) const
    {
        PIXScopedEvent(commandList, pix::kColorGpu, L"Shadow casters");
        RecordDraws(commandList, scene, frame, Kind::SunShadow);
    }

    void ScenePass::RenderDepthPrepass(ID3D12GraphicsCommandList* commandList, const SceneGpuData& scene,
                                       const FrameSettings& frame) const
    {
        PIXScopedEvent(commandList, pix::kColorGpu, L"Depth pre-pass");
        RecordDraws(commandList, scene, frame, Kind::DepthPrepass);
    }

    void ScenePass::RenderGBuffer(ID3D12GraphicsCommandList* commandList, const SceneGpuData& scene,
                                  const FrameSettings& frame) const
    {
        PIXScopedEvent(commandList, pix::kColorGpu, L"G-buffer");
        RecordDraws(commandList, scene, frame, Kind::GBuffer);
    }

    uint32_t ScenePass::RenderPointShadowDepth(ID3D12GraphicsCommandList* commandList, const SceneGpuData& scene,
                                               const FrameSettings& frame,
                                               D3D12_GPU_VIRTUAL_ADDRESS faceViewProjection,
                                               const DirectX::XMFLOAT3& lightPosition, float lightRange) const
    {
        commandList->SetGraphicsRootConstantBufferView(RootParam::DrawData, faceViewProjection);
        const CullSphere cull{ lightPosition, lightRange };
        return RecordDraws(commandList, scene, frame, Kind::PointShadow, &cull);
    }

    uint32_t ScenePass::RecordDraws(ID3D12GraphicsCommandList* commandList, const SceneGpuData& scene,
                                    const FrameSettings& frame, Kind kind, const CullSphere* cull) const
    {
        // The camera and lights come from the frame constants (b2), which the renderer has already bound.
        // Everything else shared by all draws is set once here.
        const SceneGpuData::Bindings bindings = scene.GetBindings();
        const SceneDrawConstants drawConstants{
            .vertexBufferIndex = bindings.vertices,
            .instanceBufferIndex = bindings.instances,
            .materialBufferIndex = bindings.materials,
            .debugView = static_cast<uint32_t>(frame.view),
            .instanceIndex = 0,
            .indexBufferIndex = bindings.indices,
            .normalMapping = static_cast<uint32_t>(frame.normalMapping),
        };
        commandList->SetGraphicsRoot32BitConstants(RootParam::DrawConstants, sizeof(drawConstants) / sizeof(uint32_t),
                                                   &drawConstants, 0);

        const D3D12_INDEX_BUFFER_VIEW indexBufferView = scene.GetIndexBufferView();
        commandList->IASetIndexBuffer(&indexBufferView);
        commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        // Per draw, only one 32-bit root constant changes. SV_InstanceID can't carry the instance index instead:
        // in D3D12 it always counts from 0 within a draw, ignoring StartInstanceLocation.
        ID3D12PipelineState* currentPipeline = nullptr;
        uint32_t drawCount = 0;
        for (const SceneGpuData::DrawItem& draw : scene.GetDraws())
        {
            if (cull && !draw.worldBounds.TouchesSphere(cull->center, cull->radius))
                continue;
            // Double-sided materials are never culled; masked ones use the discarding pixel shader. The draws are
            // sorted by these two flags, so each PSO is set once. The ray-traced variant traces whichever shadows
            // are set to rays (the frame constants pick sun / points).
            const PipelineKey key = Normalize({
                .kind = kind,
                .raytraced = frame.raytracedVariant,
                .alphaTest = frame.alphaTest && draw.alphaTest,
                .cullNone = !frame.backfaceCulling || draw.doubleSided,
            });
            ID3D12PipelineState* pipeline = m_pipelines[SlotOf(key)].Get();
            if (pipeline != currentPipeline)
            {
                commandList->SetPipelineState(pipeline);
                currentPipeline = pipeline;
            }
            commandList->SetGraphicsRoot32BitConstant(RootParam::DrawConstants, draw.instanceIndex, kInstanceIndexConstant);
            // BaseVertexLocation = 0: the loader already added each primitive's first vertex to its indices.
            commandList->DrawIndexedInstanced(draw.indexCount, 1, draw.firstIndex, 0, 0);
            ++drawCount;
        }
        return drawCount;
    }
}