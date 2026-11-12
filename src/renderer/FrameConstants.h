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
        float padding2 = 0.0f;
        DirectX::XMFLOAT3 ambientRadiance; // linear color * intensity: light arriving from every direction equally
        float padding3 = 0.0f;
    };
    static_assert(sizeof(FrameConstants) == 192);
}
