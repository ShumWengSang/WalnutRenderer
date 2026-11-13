// Builds the cluster grid (L3): for every cluster, the list of point lights whose sphere of influence touches it.
// The engine's first compute shader: no vertices, no pixels, just threads. Dispatch(N) runs N groups of [numthreads]
// threads; each thread finds its own job from SV_DispatchThreadID (here: which cluster it handles).

#define RAYTRACED_SHADOWS 0 // Lighting.hlsli is only included for the PointLight layout

#include "common/Bindless.hlsli"
#include "common/Clusters.hlsli"
#include "common/FrameConstants.hlsli"
#include "common/Lighting.hlsli"

// Root constants at b0. Must match ClusterBuildConstants in ClusterGrid.cpp.
struct DrawConstants
{
    uint gridUavIndex;     // RWStructuredBuffer<uint2>: (first index, light count) per cluster
    uint indicesUavIndex;  // RWStructuredBuffer<uint>: every cluster's light indices, packed one after another
    uint counterUavIndex;  // RWStructuredBuffer<uint>: how many indices are used so far
    uint indexCapacity;    // size of the index list
};
ConstantBuffer<DrawConstants> g_draw : register(b0);

// One thread: the counter starts every frame at 0. (A separate dispatch, so it's done before any thread of the next
// one adds to it; threads of one dispatch can't wait for each other across groups.)
[numthreads(1, 1, 1)]
void CSResetCounter()
{
    RWStructuredBuffer<uint> counter = ResourceDescriptorHeap[g_draw.counterUavIndex];
    counter[0] = 0;
}

// The cluster's box in view space (x right, y up, z forward). Its four screen-tile corners are rays from the camera:
// a point on screen at (x, y) in -1..1 seen at depth z sits at (x * tan(fovX / 2) * z, y * tan(fovY / 2) * z, z).
// Taking those rays at the slice's near and far depth gives 8 corner points; the box around them holds the cluster.
void ClusterBounds(uint3 cell, out float3 boxMin, out float3 boxMax)
{
    const float2 ndcMin = float2(float(cell.x) / kClusterCountX * 2.0f - 1.0f, 1.0f - float(cell.y + 1) / kClusterCountY * 2.0f);
    const float2 ndcMax = float2(float(cell.x + 1) / kClusterCountX * 2.0f - 1.0f, 1.0f - float(cell.y) / kClusterCountY * 2.0f);
    const float2 tanHalfFov = float2(g_frame.tanHalfFovX, g_frame.tanHalfFovY);
    const float zNear = ClusterSliceDepth(cell.z);
    const float zFar = ClusterSliceDepth(cell.z + 1);

    // x and y grow with depth, so the extremes are at either the near or the far end.
    const float2 a = ndcMin * tanHalfFov, b = ndcMax * tanHalfFov;
    boxMin = float3(min(min(a * zNear, a * zFar), min(b * zNear, b * zFar)), zNear);
    boxMax = float3(max(max(a * zNear, a * zFar), max(b * zNear, b * zFar)), zFar);
}

// Does a sphere touch a box? Find the box's point closest to the sphere's center (clamp the center into the box) and
// check whether it's within the radius.
bool SphereTouchesBox(float3 center, float radius, float3 boxMin, float3 boxMax)
{
    const float3 closest = clamp(center, boxMin, boxMax);
    const float3 d = closest - center;
    return dot(d, d) <= radius * radius;
}

[numthreads(64, 1, 1)]
void CSBuildClusters(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint cluster = dispatchThreadId.x;
    if (cluster >= kClusterCount)
        return; // the last group has a few threads too many
    const uint3 cell = uint3(cluster % kClusterCountX, (cluster / kClusterCountX) % kClusterCountY,
                             cluster / (kClusterCountX * kClusterCountY));
    float3 boxMin, boxMax;
    ClusterBounds(cell, boxMin, boxMax);

    StructuredBuffer<PointLight> lights = ResourceDescriptorHeap[g_frame.pointLightBufferIndex];
    RWStructuredBuffer<uint2> grid = ResourceDescriptorHeap[g_draw.gridUavIndex];
    RWStructuredBuffer<uint> indices = ResourceDescriptorHeap[g_draw.indicesUavIndex];
    RWStructuredBuffer<uint> counter = ResourceDescriptorHeap[g_draw.counterUavIndex];

    // Pass 1: how many lights touch this cluster? (Light positions go to view space, where the box is.)
    uint count = 0;
    for (uint i = 0; i < g_frame.pointLightCount; ++i)
    {
        const PointLight light = lights[i];
        const float3 center = mul(float4(light.position, 1.0f), g_frame.view).xyz;
        if (SphereTouchesBox(center, light.range, boxMin, boxMax))
            ++count;
    }

    // Reserve exactly that many slots in the shared index list. InterlockedAdd adds atomically and returns the old
    // value: every cluster gets its own range, however the 3456 threads interleave.
    uint first = 0;
    if (count > 0)
        InterlockedAdd(counter[0], count, first);
    count = first < g_draw.indexCapacity ? min(count, g_draw.indexCapacity - first) : 0; // never write past the end
    grid[cluster] = uint2(first, count);

    // Pass 2: the same tests again, now writing the indices into the reserved range.
    uint written = 0;
    for (uint j = 0; j < g_frame.pointLightCount && written < count; ++j)
    {
        const PointLight light = lights[j];
        const float3 center = mul(float4(light.position, 1.0f), g_frame.view).xyz;
        if (SphereTouchesBox(center, light.range, boxMin, boxMax))
            indices[first + written++] = j;
    }
}
