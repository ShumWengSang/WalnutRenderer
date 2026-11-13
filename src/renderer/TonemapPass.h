#pragma once

#include "gfx/D3D12Common.h"
#include "shared/Constants.h"

#include <cstdint>
#include <filesystem>
#include <string>

namespace walnut
{
    class Device;
    class ShaderCompiler;

    // Which curve maps HDR scene light (0 .. infinity) to displayable 0..1. Values shared with tonemap.hlsl
    // (shared/Constants.h).
    enum class TonemapOperator : uint32_t
    {
        Clamp = kTonemapClamp,           // no curve: everything above 1 is clipped (what D1 did)
        Reinhard = kTonemapReinhard,     // x / (1 + x): simple, never clips, but flattens and desaturates bright areas
        AcesFitted = kTonemapAcesFitted, // fit of the ACES film curve: contrasty "filmic" look, gentle highlight roll-off
    };

    struct TonemapSettings
    {
        // Exposure in photographic stops: each +1 doubles the scene's brightness before the curve.
        float exposureStops = 0.0f;
        TonemapOperator op = TonemapOperator::AcesFitted;
    };

    // Fullscreen pass that reads the HDR scene color texture, applies exposure and a tonemap curve, and writes
    // the back buffer through its _SRGB view (so the hardware does the linear -> sRGB encoding).
    class TonemapPass
    {
    public:
        TonemapPass(Device& device, const ShaderCompiler& compiler, std::filesystem::path shaderPath,
                    ID3D12RootSignature* rootSignature, DXGI_FORMAT outputFormat);

        // Recompiles the shaders and rebuilds the PSO. On failure the previous PSO is kept.
        // The old PSO is released on success, so the GPU must be idle before calling this.
        bool ReloadShaders();
        const std::string& GetLastError() const { return m_lastError; }

        // Expects the bindless state to be bound and the back buffer to be the render target
        // (Renderer::BeginBackBufferPass).
        void Render(ID3D12GraphicsCommandList* commandList, uint32_t sceneColorSrvIndex, const TonemapSettings& settings) const;

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
