#pragma once

#include "gfx/D3D12Common.h"

namespace walnut
{
    // The one root signature every pass shares ("bindless"). Instead of each pass describing which textures
    // and buffers it binds, shaders fetch resources directly from the shader-visible descriptor heap by index
    // (ResourceDescriptorHeap[i], Shader Model 6.6). The root signature then only carries a few small things:
    //
    //   param 0  b0  root constants   kNumDrawConstants x 32-bit values, per draw (e.g. descriptor indices)
    //   param 1  b1  root CBV         per-draw data in a FrameAllocator constant buffer (e.g. a world matrix)
    //   param 2  b2  root CBV         per-frame data (e.g. camera, lights), shared by all draws
    //   static samplers s0..s2        fixed sampler states baked into the root signature
    //
    // The HLSL side of this contract is shaders/common/Bindless.hlsli. Keep the two in sync.
    namespace RootParam
    {
        enum : UINT
        {
            DrawConstants = 0,
            DrawData = 1,
            FrameData = 2,
            Count,
        };
    }

    inline constexpr UINT kNumDrawConstants = 8;

    ComPtr<ID3D12RootSignature> CreateBindlessRootSignature(ID3D12Device* device);
}
