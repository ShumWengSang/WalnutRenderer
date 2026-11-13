// Point light shadows from cube shadow maps (Step 6: M2).
#ifndef WALNUT_POINT_SHADOWS_HLSLI
#define WALNUT_POINT_SHADOWS_HLSLI

#include "common/Bindless.hlsli"
#include "common/FrameConstants.hlsli"
#include "common/PointLight.hlsli"

// Point light shadows from cube shadow maps (M2). The lamp's cube was rendered by six 90-degree cameras at the lamp
// (PointShadowMaps.cpp). Sampling a cube with a direction makes the hardware pick the face and the texel the ray from
// the lamp's center passes through; SampleCmp then compares our depth with the depth stored there, like SunShadow.
static const float kPointShadowFaceSize = 256.0f;     // PointShadowMaps::kFaceSize
static const float kPointShadowNormalOffset = 1.5f;   // texels, along the surface normal (towards the lamp)
static const float kPointShadowDepthBias = 1.0f;      // texels, towards the lamp

float PointShadow(PointLight light, float3 worldPosition, float3 geometricNormal)
{
    if (light.shadowSlot == kNoShadowSlot)
        return 1.0f; // beyond the budget of shadowed lamps (or point shadows off): unblocked

    // One texel of a 90-degree face of N texels covers 2 * distance / N meters at that distance (the face spans from
    // -45 to +45 degrees: 2 * distance * tan(45) wide). Bias and filter sizes are in texels, so they scale with it.
    const float3 fromLight = worldPosition - light.position;
    const float texel = 2.0f * length(fromLight) / kPointShadowFaceSize;
    // Normal offset: look up a point slightly above the surface, on the lamp's side of it.
    const float3 n = dot(geometricNormal, fromLight) > 0.0f ? -geometricNormal : geometricNormal;
    const float3 offset = fromLight + n * (kPointShadowNormalOffset * texel);

    // The depth the face camera would have given this point. A face camera's depth axis is the face's axis, so its
    // view-space z is the largest component of the offset; the reversed-Z projection (near = nearScale * range,
    // far = range) turns it into near * (range - z) / ((range - near) * z): 1 at the near plane, 0 at the range.
    const float z = max(max(abs(offset.x), abs(offset.y)), abs(offset.z)) - kPointShadowDepthBias * texel;
    const float nearZ = g_frame.pointShadowNearScale * light.range;
    const float depth = nearZ * (light.range - z) / ((light.range - nearZ) * max(z, nearZ));

    // Soft edges: the hardware's 2x2 comparison filter (sampler s6) at four points around the direction, half a texel
    // out on two axes perpendicular to it.
    TextureCubeArray<float> cubes = ResourceDescriptorHeap[g_frame.pointShadowMapIndex];
    const float3 dir = offset / length(offset);
    const float3 axisA = normalize(cross(dir, abs(dir.y) < 0.99f ? float3(0.0f, 1.0f, 0.0f) : float3(1.0f, 0.0f, 0.0f)));
    const float3 axisB = cross(dir, axisA);
    const float spread = 0.75f * 2.0f / kPointShadowFaceSize; // in direction units: 0.75 texel at distance 1
    float lit = 0.0f;
    [unroll] for (int i = 0; i < 4; ++i)
    {
        const float2 o = float2((i & 1) ? 1.0f : -1.0f, (i & 2) ? 1.0f : -1.0f) * spread;
        lit += cubes.SampleCmpLevelZero(g_samplerShadowLinear, float4(dir + axisA * o.x + axisB * o.y, light.shadowSlot), depth);
    }
    return lit * 0.25f;
}

#endif
