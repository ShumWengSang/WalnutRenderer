#pragma once

#include "gfx/D3D12Common.h"
#include "gfx/PipelineBuilder.h"
#include "renderer/RenderSettings.h"

#include <DirectXMath.h>

#include <array>
#include <cstdint>
#include <filesystem>

namespace walnut
{
    class Device;
    class SceneGpuData;
    class ShaderCompiler;

    // Draws a scene's GPU data (SceneGpuData) in each way the frame needs: forward shading, the G-buffer, the depth
    // pre-pass and the shadow depth passes. It owns the PSOs for all of them; the vertex shader pulls vertices and
    // instance data from the bindless heap, and each instance is one DrawIndexedInstanced.
    class ScenePass : public ShaderReloadable
    {
    public:
        // The ways of drawing the scene. Each has its own PSOs (its recipe in ScenePass.cpp).
        enum class Kind : uint8_t
        {
            SunShadow,           // depth only, into the sun's shadow map
            PointShadow,         // depth only, into one face of a point light's cube
            DepthPrepass,        // depth only, from the camera
            Forward,             // shaded, depth test and write
            ForwardAfterPrepass, // shaded, depth EQUAL to the pre-pass's, no depth writes
            GBuffer,             // the material into the G-buffer, depth EQUAL
            Count,
        };

        ScenePass(Device& device, const ShaderCompiler& compiler, std::filesystem::path shaderPath,
                  ID3D12RootSignature* rootSignature, DXGI_FORMAT rtvFormat, DXGI_FORMAT dsvFormat);

        bool SupportsRaytracedShadows() const { return m_raytracingSupported; }

        // All of these expect the bindless state (descriptor heap + root signature) and the frame constants to be
        // bound already.
        void Render(ID3D12GraphicsCommandList* commandList, const SceneGpuData& scene, const FrameSettings& frame) const;
        // Every instance into the bound shadow map: depth only, from the sun (frame constants' sunViewProjection).
        void RenderShadowDepth(ID3D12GraphicsCommandList* commandList, const SceneGpuData& scene,
                               const FrameSettings& frame) const;
        // Every instance into the bound depth buffer from the camera, depth only. With frame.depthPrepass, Render()
        // must follow with the same settings: it then shades only the fragments matching this depth.
        void RenderDepthPrepass(ID3D12GraphicsCommandList* commandList, const SceneGpuData& scene,
                                const FrameSettings& frame) const;
        // Deferred shading: writes every visible pixel's material into the bound G-buffer. Needs the depth pre-pass
        // first (it tests depth EQUAL).
        void RenderGBuffer(ID3D12GraphicsCommandList* commandList, const SceneGpuData& scene,
                           const FrameSettings& frame) const;
        // One face of a point light's cube shadow map (M1): depth only, with the face camera's view-projection at b1.
        // Draws only the instances whose bounds reach the light's sphere. Returns how many draws it recorded.
        uint32_t RenderPointShadowDepth(ID3D12GraphicsCommandList* commandList, const SceneGpuData& scene,
                                        const FrameSettings& frame, D3D12_GPU_VIRTUAL_ADDRESS faceViewProjection,
                                        const DirectX::XMFLOAT3& lightPosition, float lightRange) const;

    private:
        // Which PSO: the kind of drawing and its variants. Variants a kind doesn't have are ignored (Normalize).
        struct PipelineKey
        {
            Kind kind;
            bool raytraced; // the pixel shader can trace shadow rays
            bool alphaTest; // alpha-mask materials: the shaders that discard
            bool cullNone;  // double-sided materials, or back-face culling off
        };
        static constexpr size_t kPipelineSlots = static_cast<size_t>(Kind::Count) * 8;
        using PipelineArray = std::array<ComPtr<ID3D12PipelineState>, kPipelineSlots>;

        void BuildPipelines(PipelineBuilder& builder) override;
        PipelineKey Normalize(PipelineKey key) const;
        static size_t SlotOf(PipelineKey key);

        // Optional culling: only draws whose bounds touch this sphere.
        struct CullSphere
        {
            DirectX::XMFLOAT3 center;
            float radius;
        };
        // Binds the geometry and issues every draw (or those touching 'cull'), with the PSOs of the given kind.
        // Returns how many draws it recorded.
        uint32_t RecordDraws(ID3D12GraphicsCommandList* commandList, const SceneGpuData& scene,
                             const FrameSettings& frame, Kind kind, const CullSphere* cull = nullptr) const;

        DXGI_FORMAT m_rtvFormat;
        DXGI_FORMAT m_dsvFormat;
        bool m_raytracingSupported = false;
        PipelineArray m_pipelines; // by SlotOf(key); variants that weren't built stay empty
    };
}