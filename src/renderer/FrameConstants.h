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
        // Ambient light (J4): a "hemisphere light", standing in for the sky and the light bounced off the ground.
        DirectX::XMFLOAT3 skyRadiance;     // linear color * intensity: light arriving from above the horizon
        float roughnessOverride = -1.0f;   // >= 0: every material uses this roughness (J5 comparisons)
        DirectX::XMFLOAT3 groundRadiance;  // linear color * intensity: light arriving from below it
        float metallicOverride = -1.0f;    // >= 0: every material uses this metallic

        // Shadow bias (H3). Sizes are in shadow-map texels: one texel covers shadowTexelWorldSize meters.
        float shadowTexelWorldSize = 0.0f; // meters covered by one shadow-map texel
        float shadowDepthPerMeter = 0.0f;  // the sun's depth is linear: moving 1 m towards the sun adds this
        float shadowConstantBias = 0.0f;   // texels: push every comparison towards the sun by this much
        float shadowSlopeBias = 0.0f;      // texels: ... plus this much times tan(angle to the sun)
        float shadowNormalOffset = 0.0f;   // texels: look up the shadow map this far out along the surface normal
        uint32_t shadowFilterRadius = 0;   // PCF kernel: (2r+1)^2 filtered taps; 0 = one tap; UINT32_MAX = hard (point)
        uint32_t tlasIndex = UINT32_MAX;   // bindless SRV of the ray tracing TLAS (ray-traced shadow pipelines only)
        uint32_t rayAlphaTest = 1;         // 0: shadow rays treat alpha-tested geometry as solid

        // Deferred shading (K3): turns a pixel's screen position + depth back into a world position.
        DirectX::XMFLOAT4X4 inverseViewProjection;
        // The scene's buffers (bindless SRVs), for shaders that aren't scene draws: the deferred pass's shadow
        // rays look up alpha-tested hits in them.
        uint32_t vertexBufferIndex = UINT32_MAX;
        uint32_t instanceBufferIndex = UINT32_MAX;
        uint32_t materialBufferIndex = UINT32_MAX;
        uint32_t indexBufferIndex = UINT32_MAX;

        // Point lights (L1): StructuredBuffer<PointLight> in the bindless heap, and how many of them to use.
        uint32_t pointLightBufferIndex = UINT32_MAX;
        uint32_t pointLightCount = 0;
        uint32_t padding5[2] = {};

        // Clustered lighting (L3): the camera's view matrix and field of view (cluster boxes are built in view space),
        // the depth range the slices cover, and the grid itself.
        DirectX::XMFLOAT4X4 view;
        float tanHalfFovX = 1.0f;
        float tanHalfFovY = 1.0f;
        float clusterNear = 0.1f;  // view-space depth (meters) where slice 0 starts: the camera's near plane
        float clusterFar = 100.0f; // ... and where the last slice ends: the farthest corner of the scene
        uint32_t clusterGridIndex = UINT32_MAX;         // StructuredBuffer<uint2>: (first index, count) per cluster
        uint32_t clusterLightIndicesIndex = UINT32_MAX; // StructuredBuffer<uint>: the clusters' light indices
        uint32_t clusteredLighting = 1; // 1: shade point lights from the pixel's cluster list; 0: brute force
        uint32_t padding6 = 0;
    };
    static_assert(sizeof(FrameConstants) == 432);
}
