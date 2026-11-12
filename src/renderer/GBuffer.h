#pragma once

#include "gfx/D3D12Common.h"
#include "gfx/DescriptorHeap.h"

#include <array>
#include <cstdint>
#include <memory>

namespace walnut
{
    class Device;

    // The G-buffer of deferred shading: window-sized render targets that the scene pass fills with each pixel's
    // material (instead of a lit color), for a later fullscreen pass to light. Layout: shaders/common/GBuffer.hlsli.
    class GBuffer
    {
    public:
        enum Target : uint32_t
        {
            BaseColor = 0, // rgb base color
            Normals,       // shading + geometric normal, octahedral
            Material,      // roughness, metallic, occlusion
            Emissive,      // emitted light, HDR
            kCount
        };
        static constexpr std::array<DXGI_FORMAT, kCount> kFormats = {
            DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,  // 4 bytes: 8 bits per channel is plenty for an sRGB color
            DXGI_FORMAT_R16G16B16A16_SNORM,   // 8 bytes: 2 x 16-bit per normal (8 bits would show banding)
            DXGI_FORMAT_R8G8B8A8_UNORM,       // 4 bytes
            DXGI_FORMAT_R11G11B10_FLOAT,      // 4 bytes: unsigned floats, no alpha, values above 1
        };

        GBuffer(Device& device, DescriptorHeap& srvHeap, uint32_t width, uint32_t height);
        ~GBuffer();

        GBuffer(const GBuffer&) = delete;
        GBuffer& operator=(const GBuffer&) = delete;

        // Recreates the targets at the new size (the GPU must be idle). The descriptor slots stay the same.
        void Resize(uint32_t width, uint32_t height);

        // UNDEFINED -> RENDER_TARGET for every target, discarding the old contents (no clear needed: the lighting
        // only reads pixels the depth buffer says were drawn, and those were all written this frame).
        void BeginWrite(ID3D12GraphicsCommandList7* commandList) const;
        // RENDER_TARGET -> shader-readable, after the G-buffer pass.
        void EndWrite(ID3D12GraphicsCommandList7* commandList) const;

        const std::array<D3D12_CPU_DESCRIPTOR_HANDLE, kCount>& GetRtvs() const { return m_rtvHandles; }
        uint32_t GetSrvIndex(Target target) const { return m_srvs[target].index; }
        uint64_t GetBytes() const { return m_bytes; } // GPU memory of all targets

    private:
        void CreateTargets(uint32_t width, uint32_t height);

        ID3D12Device10* m_device = nullptr;
        DescriptorHeap& m_srvHeap;
        std::unique_ptr<DescriptorHeap> m_rtvHeap;
        std::array<ComPtr<ID3D12Resource>, kCount> m_targets;
        std::array<DescriptorHandle, kCount> m_rtvs;
        std::array<D3D12_CPU_DESCRIPTOR_HANDLE, kCount> m_rtvHandles{};
        std::array<DescriptorHandle, kCount> m_srvs;
        uint64_t m_bytes = 0;
    };
}
