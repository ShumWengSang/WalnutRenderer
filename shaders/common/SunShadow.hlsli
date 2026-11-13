// The sun's shadow from its shadow map (Step 2: H1-H5).
#ifndef WALNUT_SUN_SHADOW_HLSLI
#define WALNUT_SUN_SHADOW_HLSLI

#include "common/Bindless.hlsli"
#include "common/FrameConstants.hlsli"

// Is this world-space point lit by the sun? 1 = yes, 0 = something closer to the sun covers it.
//
// 1. Transform the point the same way the shadow pass transformed the geometry: into the sun's clip space.
//    The projection is orthographic, so w is 1 and there's no divide: x, y in -1..1 across the map, z = depth.
// 2. Clip space y points up, texture v points down: uv = (x * 0.5 + 0.5, -y * 0.5 + 0.5).
// 3. SampleCmpLevelZero reads the depth the sun saw at uv and compares our depth with it (comparison sampler s5:
//    lit if ours >= stored, reversed-Z). "LevelZero": mip 0, the only one; derivatives aren't needed.
//
// Bias: each shadow-map texel stores ONE depth for a whole patch of surface. On a surface tilted away from the sun,
// half of that patch is slightly farther from the sun than the stored value and would shadow itself ("acne"). Two
// fixes, both sized in texels so they scale with the shadow map's resolution:
//  - Normal offset: look the point up a little way out along its normal, i.e. just above the surface.
//  - Depth bias: pretend the point is a little closer to the sun. The depth error inside one texel grows with the
//    surface's tilt, as tan(angle between normal and sun), so there's a constant part and a slope-scaled part.
// Too much of either detaches shadows from the objects casting them ("peter-panning").
//
// Soft edges (PCF, percentage-closer filtering): one comparison gives a hard 0/1 and stair-stepped edges, because
// each texel is all-or-nothing. Instead, do the comparison at several neighbouring texels and average the RESULTS:
// a pixel on a shadow edge sees some taps lit and some shadowed, and gets a value in between. Each tap here already
// uses the hardware's bilinear 2x2 comparison (sampler s6), so a radius r gives (2r+1)^2 taps covering
// (2r+2)^2 texels, with weights that fade smoothly at the kernel's border.
float SunShadow(float3 worldPosition, float3 normal, float NdotL)
{
    if (g_frame.shadowsEnabled == 0)
        return 1.0f;

    const float texel = g_frame.shadowTexelWorldSize;
    const float3 offsetPosition = worldPosition + normal * (g_frame.shadowNormalOffset * texel);
    const float4 lightClip = mul(float4(offsetPosition, 1.0f), g_frame.sunViewProjection);
    const float2 uv = float2(lightClip.x * 0.5f + 0.5f, lightClip.y * -0.5f + 0.5f);

    const float tanAngle = min(sqrt(saturate(1.0f - NdotL * NdotL)) / max(NdotL, 1e-4f), 10.0f);
    // Wider filters compare texels farther from the point, where a tilted surface's own depth has drifted further:
    // the slope part of the bias grows with the kernel's reach, or the outer taps start shadowing the surface itself.
    const float kernelReach = g_frame.shadowFilterRadius == 0xFFFFFFFF ? 0.0f : float(g_frame.shadowFilterRadius) + 1.0f;
    const float biasMeters = (g_frame.shadowConstantBias + g_frame.shadowSlopeBias * tanAngle * (1.0f + kernelReach)) * texel;
    const float depth = lightClip.z + biasMeters * g_frame.shadowDepthPerMeter; // + = closer to the sun (reversed-Z)

    Texture2D<float> shadowMap = ResourceDescriptorHeap[g_frame.shadowMapIndex];
    const uint radius = g_frame.shadowFilterRadius;
    if (radius == 0xFFFFFFFF)
        return shadowMap.SampleCmpLevelZero(g_samplerShadow, uv, depth); // hard: nearest texel only

    // Taps are one texel apart: the uv offset of one texel is 1 / map size.
    float width, height;
    shadowMap.GetDimensions(width, height);
    const float2 texelUv = 1.0f / float2(width, height);
    const int r = (int)radius;
    float lit = 0.0f;
    for (int y = -r; y <= r; ++y)
        for (int x = -r; x <= r; ++x)
            lit += shadowMap.SampleCmpLevelZero(g_samplerShadowLinear, uv + float2(x, y) * texelUv, depth);
    return lit / float((2 * r + 1) * (2 * r + 1));
}

#endif
