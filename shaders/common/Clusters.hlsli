// Clustered lighting (L3/L4): the view frustum cut into a 3D grid of cells ("clusters"), each with a list of the point
// lights that can reach it. Built every frame by clusters.hlsl, read by the shading (Lighting.hlsli).
// Must match ClusterGrid.h.
//
//   x, y: 16 x 9 tiles across the screen
//   z:    24 slices in depth, spaced exponentially between clusterNear and clusterFar (thin near the camera, thick far
//         away: in perspective a distant pixel covers much more depth, so equal-looking slices need growing depths)
#ifndef WALNUT_CLUSTERS_HLSLI
#define WALNUT_CLUSTERS_HLSLI

#include "common/FrameConstants.hlsli"

static const uint kClusterCountX = 16;
static const uint kClusterCountY = 9;
static const uint kClusterCountZ = 24;
static const uint kClusterCount = kClusterCountX * kClusterCountY * kClusterCountZ;

uint ClusterIndex(uint3 cell)
{
    return cell.x + kClusterCountX * (cell.y + kClusterCountY * cell.z);
}

// View-space depth where slice 'slice' starts: near * (far / near)^(slice / count). Slice count ends at far.
float ClusterSliceDepth(uint slice)
{
    return g_frame.clusterNear * pow(g_frame.clusterFar / g_frame.clusterNear, float(slice) / float(kClusterCountZ));
}

// The inverse: which slice a view-space depth falls into, log(z / near) / log(far / near) * count.
uint ClusterSlice(float viewDepth)
{
    const float slice = log(max(viewDepth, g_frame.clusterNear) / g_frame.clusterNear) /
                        log(g_frame.clusterFar / g_frame.clusterNear) * float(kClusterCountZ);
    return min(uint(slice), kClusterCountZ - 1);
}

// The cluster a world-space point is in. The view-projection matrix gives both halves at once: clip.xy / clip.w is the
// point's place on screen (-1..1), and clip.w itself is its view-space depth (a perspective projection copies z to w).
uint ClusterOf(float3 worldPosition)
{
    const float4 clip = mul(float4(worldPosition, 1.0f), g_frame.viewProjection);
    const float2 ndc = clip.xy / clip.w;
    const float2 screenUv = float2(ndc.x * 0.5f + 0.5f, 0.5f - ndc.y * 0.5f); // y down, like pixels
    const uint2 tile = min(uint2(saturate(screenUv) * float2(kClusterCountX, kClusterCountY)),
                           uint2(kClusterCountX - 1, kClusterCountY - 1));
    return ClusterIndex(uint3(tile, ClusterSlice(clip.w)));
}

#endif
