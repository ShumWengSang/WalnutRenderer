// The debug views (View > Show) both rendering paths can show, from a MaterialSample and a ShadingResult.
#ifndef WALNUT_DEBUG_VIEW_COLORS_HLSLI
#define WALNUT_DEBUG_VIEW_COLORS_HLSLI

#include "common/Clusters.hlsli"
#include "common/DebugViews.hlsli"
#include "common/Material.hlsli"
#include "common/Shading.hlsli"

// Ray traced vs shadow map, for the same point. Gray where they agree; red where only the shadow map says "shadow"
// (acne, bias, blur spilling into lit areas); blue where only the ray says "shadow" (light leaking: peter-panning,
// thin casters the map's texels missed).
float3 ShadowDifferenceColor(float rayShadow, float mapShadow)
{
    const float difference = mapShadow - rayShadow; // + : map lit, ray shadow (blue)   - : map shadow, ray lit (red)
    float3 color = lerp(0.35f, 0.9f, rayShadow).xxx * 0.5f;
    color = lerp(color, float3(1.0f, 0.1f, 0.05f), saturate(-difference));
    return lerp(color, float3(0.1f, 0.35f, 1.0f), saturate(difference));
}

// A heat-map color for a count: dark blue at 0, through cyan, green and yellow, to red at 'maxCount' and above.
float3 HeatColor(uint count, uint maxCount)
{
    if (count == 0)
        return float3(0.02f, 0.02f, 0.08f);
    const float t = saturate(float(count) / float(maxCount)) * 4.0f;
    const float3 ramp[5] = { float3(0.1f, 0.1f, 0.9f), float3(0.0f, 0.8f, 0.9f), float3(0.1f, 0.85f, 0.1f),
                             float3(0.95f, 0.9f, 0.1f), float3(0.95f, 0.1f, 0.05f) };
    const uint i = min(uint(t), 3u);
    return lerp(ramp[i], ramp[i + 1], t - float(i));
}

// The debug views both rendering paths can show (they only need the material sample and the shading). Returns
// false for views that need what only the forward pass has: texture coordinates, instance and material ids, the raw
// normal map, alpha mode and face orientation.
bool SharedViewColor(uint view, float3 worldPosition, MaterialSample mat, ShadingResult shading, out float3 color)
{
    color = 0.0f;
    if (view == kViewClusterLights)
    {
        // How many lights the pixel's cluster lists (red = 64 or more), over a dim copy of the shaded image.
        StructuredBuffer<uint2> grid = ResourceDescriptorHeap[g_frame.clusterGridIndex];
        const uint count = grid[ClusterOf(worldPosition)].y;
        const float brightness = dot(shading.color, float3(0.2126f, 0.7152f, 0.0722f));
        color = HeatColor(count, 64) * (0.6f + 0.4f * saturate(brightness));
        return true;
    }
    if (view == kViewLit)                   color = shading.color;
    else if (view == kViewLightingOnly)     color = shading.lighting;
    else if (view == kViewSunSpecular)      color = shading.sunSpecular;
    else if (view == kViewAmbient)          color = shading.ambient;
    else if (view == kViewPointLights)      color = shading.pointLights;
    else if (view == kViewPointShadowDifference)
    {
        // Cube maps vs rays, for the lamps that have a cube: the same colors as the sun's difference view (red: only
        // the cube map shadows, blue: only the ray does), dark blue where no such lamp reaches the point.
        const float3 c = shading.pointShadowCompare;
        color = c.z > 0.0f ? ShadowDifferenceColor(c.y / c.z, c.x / c.z) : float3(0.03f, 0.05f, 0.15f);
    }
    // Shadowed lamps' visibility: white = lit, black = blocked, dark blue = no shadowed lamp reaches this point.
    else if (view == kViewPointShadows)     color = shading.pointShadow < 0.0f ? float3(0.03f, 0.05f, 0.15f) : shading.pointShadow.xxx;
    else if (view == kViewSunShadow)        color = shading.shadow.xxx;
    else if (view == kViewShadowDifference)
    {
#if RAYTRACED_SHADOWS
        color = ShadowDifferenceColor(shading.shadow, shading.mapShadow);
#else
        color = float3(1.0f, 0.0f, 1.0f); // needs the ray-traced variant (the UI only offers it there)
#endif
    }
    else if (view == kViewNormals)          color = mat.normal * 0.5f + 0.5f;
    else if (view == kViewGeometricNormals) color = mat.geometricNormal * 0.5f + 0.5f;
    else if (view == kViewBaseColor)        color = mat.baseColor;
    // Roughness/metallic as read (before the BRDF's minimum roughness).
    else if (view == kViewRoughness)        color = mat.roughness.xxx;
    else if (view == kViewMetallic)         color = mat.metallic.xxx;
    else if (view == kViewOcclusion)        color = mat.occlusion.xxx;
    else if (view == kViewEmissive)         color = mat.emissive;
    else
        return false;
    return true;
}

#endif
