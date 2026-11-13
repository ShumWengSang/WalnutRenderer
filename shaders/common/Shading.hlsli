// Lighting one surface point: sun, sky/ground ambient and point lights through the BRDF (ShadeSurface). Shared by
// forward shading (scene.hlsl) and the deferred lighting pass (deferred.hlsl).
//
// Expects RAYTRACED_SHADOWS to be defined (0 or 1) before it's included.
#ifndef WALNUT_SHADING_HLSLI
#define WALNUT_SHADING_HLSLI

#include "common/Bindless.hlsli"
#include "common/Brdf.hlsli"
#include "common/Clusters.hlsli"
#include "common/DebugViews.hlsli"
#include "common/FrameConstants.hlsli"
#include "common/Material.hlsli"
#include "common/PointLight.hlsli"
#include "common/PointShadows.hlsli"
#include "common/ShadowRays.hlsli"
#include "common/SunShadow.hlsli"
#include "shared/Constants.h" // kShading*, kPointShadow* (the point shadow technique)

// The ambient "hemisphere light": the sky's radiance from above the horizon, the ground's from below.
//
// Looking along a direction d with a blur of 'width', what arrives is a mix of the two, depending on how far above
// the horizon d points (d.y = sine of its elevation):
//   width = 1:     the whole hemisphere around d, cosine-weighted. A surface whose normal is tilted by an angle t
//                  from straight up sees the sky over a (1 + cos t) / 2 share of it, which is exactly
//                  lerp(ground, sky, 0.5 + 0.5 * d.y): the diffuse ambient (irradiance / pi).
//   width -> 0:    a mirror: a sharp horizon, sky or ground.
//   width = alpha: a GGX lobe is roughly alpha radians wide, so the reflected horizon blurs over about that much.
float3 HemisphereLight(float3 direction, float width)
{
    const float skyShare = saturate(0.5f + 0.5f * direction.y / max(width, 1e-3f));
    return lerp(g_frame.groundRadiance, g_frame.skyRadiance, skyShare);
}


// Adds one point light's reflected light (radiance) and arriving light (irradiance, for the Lambert views). The
// visibility of shadowed lamps that reach the point is summed into shadowSum / shadowCount (the shadow debug view).
// comparePointShadows (the point shadow difference view, ray-traced variant): for lamps with a cube map, also trace
// a ray and sum both answers into compareMap / compareRay.
void AddPointLight(PointLight light, float3 worldPosition, MaterialSample mat, float3 toCamera, SurfaceBrdf surface,
                   bool comparePointShadows, inout float3 radiance, inout float3 irradianceSum, inout float shadowSum,
                   inout uint shadowCount, inout float3 compare)
{
    const float3 toLight = light.position - worldPosition;
    const float distanceSquared = dot(toLight, toLight);
    if (distanceSquared >= light.range * light.range)
        return;
    const float3 L = toLight * rsqrt(distanceSquared);
    const float NdotL = saturate(dot(mat.normal, L));
    if (NdotL <= 0.0f)
        return;
    // The point shadow technique (Lighting > Point lights): cube maps for the lamps with a slot, or a ray for every
    // lamp (only in the ray-traced shader variant, which the UI selects when this is on).
    float shadow;
    bool shadowed;
#if RAYTRACED_SHADOWS
    if (g_frame.pointShadowTechnique == kPointShadowRayTraced)
    {
        shadow = RayTracedPointShadow(worldPosition, mat.geometricNormal, light.position);
        shadowed = true;
    }
    else
#endif
    {
        shadow = PointShadow(light, worldPosition, mat.geometricNormal);
        shadowed = light.shadowSlot != kNoShadowSlot;
    }
#if RAYTRACED_SHADOWS
    if (comparePointShadows && light.shadowSlot != kNoShadowSlot)
        compare += float3(shadow, RayTracedPointShadow(worldPosition, mat.geometricNormal, light.position), 1.0f);
#endif
    if (shadowed)
    {
        shadowSum += shadow;
        ++shadowCount;
    }
    if (shadow <= 0.0f)
        return;
    const float3 irradiance = light.intensity * (PointLightFalloff(distanceSquared, light.range) * NdotL * shadow);
    const BrdfValue brdf = EvaluateBrdf(surface, mat.normal, toCamera, L);
    radiance += (brdf.diffuse + brdf.specular) * irradiance;
    irradianceSum += irradiance;
}

// The light leaving a surface towards the camera, split into the parts the debug views show.
struct ShadingResult
{
    float3 color;       // the Lit view: Lambert or PBR (FrameConstants.shadingModel)
    float3 lighting;    // what a white matte surface would show: the sun's share plus the ambient around the normal
    float3 sunSpecular; // the sun's GGX highlight alone
    float3 ambient;     // sky + ground light, diffuse + reflection, darkened by occlusion
    float3 pointLights; // every point light's diffuse + specular (Lambert mode: diffuse only)
    float  pointShadow; // average visibility of the shadowed lamps reaching the point; -1: none reaches it
    float3 pointShadowCompare; // comparePointShadows: (sum of cube-map visibility, sum of ray visibility, lamp count)
    float  shadow;      // the sun's visibility: 1 = lit, 0 = blocked (shadow map, or ray in the ray-traced variant)
    float  mapShadow;   // ray-traced variant with compareShadowMap only: the shadow map's answer for the same point
};

// Lights one surface point. Forward shading calls it per fragment with the interpolated position; deferred shading
// calls it once per screen pixel with the position rebuilt from the depth buffer. Same code, so same result.
ShadingResult ShadeSurface(float3 worldPosition, MaterialSample mat, bool compareShadowMap, bool comparePointShadows)
{
    ShadingResult result;

    // The sun's light arrives on a surface facing it with sunIlluminance; tilted away by an angle, the same light
    // spreads over a bigger area, so each point gets cos(angle) = N.L of it (clamped: facing away gets none).
    // It only arrives if nothing blocks it: 'shadow' is 1 (lit) or 0 (in shadow).
    //
    // The surface sends some of that light towards the camera; how much is the BRDF (see Brdf.hlsli):
    //   Lambert (J1): baseColor / pi, the same in every direction (a matte surface)
    //   PBR (J3):     Lambert diffuse for what enters the surface + GGX specular for what bounces off it
    //
    //   sun:      BRDF * sunIlluminance * N.L * shadow
    //   ambient:  light from the sky and the ground (J4), see below
    //   emissive: light the surface makes itself (J4)
    const float NdotL = saturate(dot(mat.normal, g_frame.sunDirection));
    // Shadow lookups use the real surface (geometricNormal), not the normal-mapped one.
    const float geometricNdotL = saturate(dot(mat.geometricNormal, g_frame.sunDirection));
#if RAYTRACED_SHADOWS
    // This variant also runs when only the point lights are ray traced: then the sun still uses its map.
    if (g_frame.sunShadowRaytraced != 0)
    {
        result.shadow = NdotL > 0.0f ? RayTracedSunShadow(worldPosition, mat.geometricNormal) : 0.0f;
        result.mapShadow = compareShadowMap && NdotL > 0.0f ? SunShadow(worldPosition, mat.geometricNormal, geometricNdotL) : 0.0f;
    }
    else
    {
        result.shadow = NdotL > 0.0f ? SunShadow(worldPosition, mat.geometricNormal, geometricNdotL) : 0.0f;
        result.mapShadow = result.shadow;
    }
#else
    result.shadow = NdotL > 0.0f ? SunShadow(worldPosition, mat.geometricNormal, geometricNdotL) : 0.0f; // facing away: dark anyway, skip the lookup
    result.mapShadow = result.shadow;
#endif
    const float3 sunLight = g_frame.sunIlluminance * NdotL * result.shadow;
    result.lighting = sunLight / kPi + HemisphereLight(mat.normal, 1.0f);

    const float3 toCamera = normalize(g_frame.cameraPosition - worldPosition);
    const SurfaceBrdf surface = MakeSurfaceBrdf(mat.baseColor, mat.metallic, mat.roughness);
    const BrdfValue brdf = EvaluateBrdf(surface, mat.normal, toCamera, g_frame.sunDirection);
    result.sunSpecular = brdf.specular * sunLight;

    // Ambient light, split like the sun's into what enters the surface and what bounces off it:
    //   diffuse:  the hemisphere around the normal, for the light that isn't reflected
    //   specular: what a mirror would show along the reflected view ray (sky above the horizon, ground below),
    //             blurred by roughness, times how much the surface reflects in total (EnvironmentBrdf)
    // Occlusion (the glTF occlusion map: 1 = open to the sky, 0 = deep in a crack) only darkens this ambient
    // part; the sun's shadow is already computed exactly.
    const float NdotV = max(dot(mat.normal, toCamera), 1e-4f);
    const float3 environmentBrdf = EnvironmentBrdf(surface, NdotV);
    const float3 reflected = reflect(-toCamera, mat.normal);
    result.ambient = (surface.diffuseColor * (1.0f - environmentBrdf) * HemisphereLight(mat.normal, 1.0f)
                    + environmentBrdf * HemisphereLight(reflected, surface.alpha)) * mat.occlusion;

    // Point lights: the same BRDF as the sun, but each light has its own direction and gets weaker with distance.
    // Their shadows come from cube maps or rays (AddPointLight, Step 6). Lights out of range or behind the surface are
    // skipped.
    //   Brute force (L1): every pixel visits every light. The loop is the same for the whole screen, so a wave of
    //                     pixels reads each light together (one fetch for all of them).
    //   Clustered (L4):   the pixel looks up its cluster (ClusterOf: screen tile + depth slice) and visits only the
    //                     lights the compute pass listed there. Fewer lights, but neighbouring pixels in different
    //                     clusters now loop different counts over different lights.
    //
    // Two separate loops (the branch is the same for the whole frame), so each compiles to its own tight loop.
    StructuredBuffer<PointLight> pointLights = ResourceDescriptorHeap[g_frame.pointLightBufferIndex];
    float3 pointLightIrradiance = 0.0f; // for the Lambert / lighting-only views
    result.pointLights = 0.0f;
    float shadowSum = 0.0f;
    uint shadowCount = 0;
    result.pointShadowCompare = 0.0f;
    if (g_frame.clusteredLighting != 0)
    {
        StructuredBuffer<uint2> grid = ResourceDescriptorHeap[g_frame.clusterGridIndex];
        StructuredBuffer<uint> clusterLightIndices = ResourceDescriptorHeap[g_frame.clusterLightIndicesIndex];
        const uint2 entry = grid[ClusterOf(worldPosition)]; // (first, count)
        for (uint i = 0; i < entry.y; ++i)
            AddPointLight(pointLights[clusterLightIndices[entry.x + i]], worldPosition, mat, toCamera, surface,
                          comparePointShadows, result.pointLights, pointLightIrradiance, shadowSum, shadowCount,
                          result.pointShadowCompare);
    }
    else
    {
        for (uint i = 0; i < g_frame.pointLightCount; ++i)
            AddPointLight(pointLights[i], worldPosition, mat, toCamera, surface, comparePointShadows, result.pointLights,
                          pointLightIrradiance, shadowSum, shadowCount, result.pointShadowCompare);
    }
    result.lighting += pointLightIrradiance / kPi;
    result.pointShadow = shadowCount > 0 ? shadowSum / float(shadowCount) : -1.0f;

    if (g_frame.shadingModel == kShadingPbr)
        result.color = (brdf.diffuse + brdf.specular) * sunLight + result.pointLights + result.ambient + mat.emissive;
    else
    {
        result.pointLights = mat.baseColor * pointLightIrradiance / kPi;
        result.color = mat.baseColor * result.lighting; // Lambert: matte only, no occlusion map, no emission
    }
    return result;
}

#endif
