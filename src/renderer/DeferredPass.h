#pragma once

#include "gfx/D3D12Common.h"

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

    // Deferred shading's fullscreen pass (shaders/deferred.hlsl): reads the G-buffer once per screen pixel and
    // writes the HDR scene color. K2: shows one G-buffer target.
    class DeferredPass
    {
    public:
        DeferredPass(Device& device, const ShaderCompiler& compiler, std::filesystem::path shaderPath,
                     ID3D12RootSignature* rootSignature, DXGI_FORMAT outputFormat);

        // Recompiles the shaders and rebuilds the PSO. On failure the previous PSO is kept.
        // The old PSO is released on success, so the GPU must be idle before calling this.
        bool ReloadShaders();
        const std::string& GetLastError() const { return m_lastError; }

        // Expects the bindless state and frame constants to be bound, and Renderer::BeginDeferredPass.
        void Render(ID3D12GraphicsCommandList* commandList, const DeferredInputs& inputs, SceneDebugView view) const;

    private:
        ID3D12Device10* m_device = nullptr;
        const ShaderCompiler& m_compiler;
        std::filesystem::path m_shaderPath;
        ID3D12RootSignature* m_rootSignature = nullptr; // owned by the Renderer
        DXGI_FORMAT m_outputFormat;

        ComPtr<ID3D12PipelineState> m_pipeline;
        std::string m_lastError;
    };
}
