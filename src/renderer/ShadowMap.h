#pragma once

#include "gfx/D3D12Common.h"
#include "gfx/DescriptorHeap.h"

#include <cstdint>
#include <memory>

namespace walnut
{
    class Device;

    // A depth texture rendered from the sun's point of view: each texel stores how far the light reaches in that
    // direction before hitting something. Anything farther away than that, along the same light ray, is in shadow.
    //
    // One resource, three views:
    //   DSV  (D32_FLOAT)  the shadow pass writes depth through it
    //   SRV  (R32_FLOAT)  shaders read the depth values (bindless, for the lighting in the next step)
    //   SRV  (R32_FLOAT, red copied into green and blue)  only for showing it in the UI as a gray image
    // A depth format can't be read through an SRV directly, so the resource is created TYPELESS and each view
    // picks the concrete format it needs.
    class ShadowMap
    {
    public:
        static constexpr DXGI_FORMAT kResourceFormat = DXGI_FORMAT_R32_TYPELESS;
        static constexpr DXGI_FORMAT kDepthFormat = DXGI_FORMAT_D32_FLOAT;
        static constexpr DXGI_FORMAT kReadFormat = DXGI_FORMAT_R32_FLOAT;

        ShadowMap(Device& device, DescriptorHeap& srvHeap, uint32_t size);
        ~ShadowMap();

        ShadowMap(const ShadowMap&) = delete;
        ShadowMap& operator=(const ShadowMap&) = delete;

        // Makes the shadow map the (only) depth target, cleared, with a matching viewport. Draw the shadow casters
        // next, then call End.
        void Begin(ID3D12GraphicsCommandList7* commandList) const;
        // Turns the written depth into something pixel shaders (and the UI) can read this frame.
        void End(ID3D12GraphicsCommandList7* commandList) const;

        uint32_t GetSize() const { return m_size; }
        uint32_t GetSrvIndex() const { return m_srv.index; }
        D3D12_GPU_DESCRIPTOR_HANDLE GetDebugSrvGpuHandle() const { return m_debugSrv.gpu; }

    private:
        std::unique_ptr<DescriptorHeap> m_dsvHeap;
        ComPtr<ID3D12Resource> m_texture;
        OwnedDescriptor m_dsv;
        OwnedDescriptor m_srv;
        OwnedDescriptor m_debugSrv;
        uint32_t m_size = 0;
    };
}
