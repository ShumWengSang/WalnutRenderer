#pragma once

#include "gfx/D3D12Common.h"
#include "gfx/PipelineBuilder.h"

#include <cstdint>
#include <filesystem>
#include <string>

namespace walnut
{
    class Device;
    class ShaderCompiler;
    enum class SceneDebugView : uint32_t;

    // Where the deferred pass finds its inputs: bindless SRV indices of the G-buffer targets and the depth buffer.
    struct DeferredInputs
    {
        uint32_t baseColor;
        uint32_t normals;
        uint32_t material;
        uint32_t emissive;
        uint32_t depth;
    };

    // Deferred shading's fullscreen pass (shaders/deferred.hlsl): reads the G-buffer once per screen pixel, rebuilds
    // the position from depth, lights it (the forward path's ShadeSurface) and writes the HDR scene color.
    class DeferredPass : public ShaderReloadable
    {
    public:
        // With raytracingSupported, a second pipeline can trace shadow rays (RAYTRACED_SHADOWS=1): the sun's, the
        // point lights', or both, as the frame constants choose.
        DeferredPass(Device& device, const ShaderCompiler& compiler, std::filesystem::path shaderPath,
                     ID3D12RootSignature* rootSignature, DXGI_FORMAT outputFormat, bool raytracingSupported);


        // Expects the bindless state and frame constants to be bound, and Renderer::BeginDeferredPass.
        void Render(ID3D12GraphicsCommandList* commandList, const DeferredInputs& inputs, SceneDebugView view,
                    bool raytracedShadows) const;

    private:
        void BuildPipelines(PipelineBuilder& builder) override;

        DXGI_FORMAT m_outputFormat;
        bool m_raytracingSupported = false;

        ComPtr<ID3D12PipelineState> m_pipeline;   // no shadow rays: sun shadow from the shadow map
        ComPtr<ID3D12PipelineState> m_rtPipeline; // can trace shadow rays (only with ray tracing support)
    };
}
