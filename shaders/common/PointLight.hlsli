// The point light layout and falloff (Step 5), shared by the shading, the cluster build and the light markers.
#ifndef WALNUT_POINT_LIGHT_HLSLI
#define WALNUT_POINT_LIGHT_HLSLI

#include "shared/Constants.h" // kNoShadowSlot

// One point light (L1). Must match PointLightGpu in PointLights.h (32 bytes).
struct PointLight
{
    float3 position;  // world space
    float  range;     // meters: the light fades to exactly zero here
    float3 intensity; // linear color * intensity
    uint   shadowSlot; // cube shadow map slot (PointShadowMaps), kNoShadowSlot: none
};

// How much of a point light reaches a surface at distance^2 = distanceSquared, per unit of intensity.
//   1 / d^2:  the inverse-square law. The light spreads over a sphere whose area grows with d^2, so twice as far
//             gets a quarter of the light. (max with 1 cm^2 so a surface touching the light doesn't get infinity.)
//   window:   real light never reaches zero, but a light that affects the whole scene can't be skipped anywhere.
//             (1 - (d/range)^4)^2 is ~1 close up and bends smoothly down to exactly 0 at the range, so beyond it the
//             light can be ignored without a visible edge (the falloff used by Unreal and Frostbite).
float PointLightFalloff(float distanceSquared, float range)
{
    const float ratio2 = distanceSquared / (range * range);
    const float window = saturate(1.0f - ratio2 * ratio2);
    return window * window / max(distanceSquared, 1e-4f);
}

#endif
