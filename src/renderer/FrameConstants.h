#pragma once

#include <DirectXMath.h>

namespace walnut
{
    // Everything shaders need that is the same for the whole frame. Bound once per frame at b2 (root parameter
    // FrameData) by Renderer::SetFrameConstants; any pass can read it. Must match shaders/common/FrameConstants.hlsli.
    //
    // cbuffer packing: a float3 followed by a float fills one 16-byte register, so each float3 here is paired
    // with a float to keep the C++ and HLSL layouts identical.
    struct FrameConstants
    {
        DirectX::XMFLOAT4X4 viewProjection;
        DirectX::XMFLOAT4X4 sunViewProjection; // world -> the sun's shadow map space (see SceneLighting)
        DirectX::XMFLOAT3 cameraPosition;  // world space
        uint32_t shadowMapIndex = 0;       // bindless SRV of the sun's shadow map
        DirectX::XMFLOAT3 sunDirection;    // world space, unit length, pointing TOWARDS the sun
        uint32_t shadowsEnabled = 1;       // 0: the sun reaches everything (no shadow map lookup)
        DirectX::XMFLOAT3 sunIlluminance;  // linear color * intensity: light arriving on a surface facing the sun
        uint32_t shadingModel = 1;         // ShadingModel (Lighting.h): 0 = Lambert, 1 = PBR
        DirectX::XMFLOAT3 ambientRadiance; // linear color * intensity: light arriving from every direction equally
        float padding3 = 0.0f;

        // Shadow bias (H3). Sizes are in shadow-map texels: one texel covers shadowTexelWorldSize meters.
        float shadowTexelWorldSize = 0.0f; // meters covered by one shadow-map texel
        float shadowDepthPerMeter = 0.0f;  // the sun's depth is linear: moving 1 m towards the sun adds this
        float shadowConstantBias = 0.0f;   // texels: push every comparison towards the sun by this much
        float shadowSlopeBias = 0.0f;      // texels: ... plus this much times tan(angle to the sun)
        float shadowNormalOffset = 0.0f;   // texels: look up the shadow map this far out along the surface normal
        uint32_t shadowFilterRadius = 0;   // PCF kernel: (2r+1)^2 filtered taps; 0 = one tap; UINT32_MAX = hard (point)
        uint32_t tlasIndex = UINT32_MAX;   // bindless SRV of the ray tracing TLAS (ray-traced shadow pipelines only)
        uint32_t rayAlphaTest = 1;         // 0: shadow rays treat alpha-tested geometry as solid
    };
    static_assert(sizeof(FrameConstants) == 224);
}
