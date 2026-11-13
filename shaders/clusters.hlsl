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

// Lights in view space, shared by the 64 threads of a group (L5). "groupshared" memory is a small, fast scratchpad
// that every thread of one group can read and write (not visible to other groups). The group loads the lights in
// batches of 64: each thread fetches and transforms ONE light into it, then every thread tests its own cluster
// against all 64. Each light is transformed once per group instead of once per thread: 64x less work and reading.
static const uint kThreadsPerGroup = 64;
groupshared float4 s_lights[kThreadsPerGroup]; // xyz = view-space center, w = range (negative: no light)

// Tests this thread's cluster against every light, one shared batch at a time. Pass 1 only counts; pass 2 writes the
// indices into the cluster's range [first, first + capacity).
uint TestLights(uint groupIndex, bool valid, float3 boxMin, float3 boxMax, bool write, uint first, uint capacity)
{
    StructuredBuffer<PointLight> lights = ResourceDescriptorHeap[g_frame.pointLightBufferIndex];
    RWStructuredBuffer<uint> indices = ResourceDescriptorHeap[g_draw.indicesUavIndex];
    uint count = 0;
    for (uint base = 0; base < g_frame.pointLightCount; base += kThreadsPerGroup)
    {
        // Every thread of the group loads one light of the batch...
        const uint lightIndex = base + groupIndex;
        float4 viewLight = float4(0.0f, 0.0f, 0.0f, -1.0f);
        if (lightIndex < g_frame.pointLightCount)
        {
            const PointLight light = lights[lightIndex];
            viewLight = float4(mul(float4(light.position, 1.0f), g_frame.view).xyz, light.range);
        }
        s_lights[groupIndex] = viewLight;
        // ... and waits until all 64 are there. (Every thread of the group must reach this line, so threads without a
        // cluster don't return early; they just skip the tests.)
        GroupMemoryBarrierWithGroupSync();

        if (valid)
        {
            const uint batchSize = min(kThreadsPerGroup, g_frame.pointLightCount - base);
            for (uint k = 0; k < batchSize; ++k)
            {
                const float4 light = s_lights[k];
                if (SphereTouchesBox(light.xyz, light.w, boxMin, boxMax))
                {
                    if (write && count < capacity)
                        indices[first + count] = base + k;
                    ++count;
                }
            }
        }
        // Wait until everyone is done with this batch before the next one overwrites it.
        GroupMemoryBarrierWithGroupSync();
    }
    return count;
}

[numthreads(kThreadsPerGroup, 1, 1)]
void CSBuildClusters(uint3 dispatchThreadId : SV_DispatchThreadID, uint groupIndex : SV_GroupIndex)
{
    const uint cluster = dispatchThreadId.x;
    const bool valid = cluster < kClusterCount; // the last group may have a few threads too many
    float3 boxMin = 0.0f, boxMax = 0.0f;
    if (valid)
    {
        const uint3 cell = uint3(cluster % kClusterCountX, (cluster / kClusterCountX) % kClusterCountY,
                                 cluster / (kClusterCountX * kClusterCountY));
        ClusterBounds(cell, boxMin, boxMax);
    }

    // Pass 1: how many lights touch this cluster?
    uint count = TestLights(groupIndex, valid, boxMin, boxMax, false, 0, 0);

    // Reserve exactly that many slots in the shared index list. InterlockedAdd adds atomically and returns the old
    // value: every cluster gets its own range, however the 3456 threads interleave.
    RWStructuredBuffer<uint> counter = ResourceDescriptorHeap[g_draw.counterUavIndex];
    uint first = 0;
    if (valid && count > 0)
        InterlockedAdd(counter[0], count, first);
    count = first < g_draw.indexCapacity ? min(count, g_draw.indexCapacity - first) : 0; // never write past the end

    // Pass 2: the same tests again, now writing the indices into the reserved range.
    TestLights(groupIndex, valid, boxMin, boxMax, true, first, count);
    if (valid)
    {
        RWStructuredBuffer<uint2> grid = ResourceDescriptorHeap[g_draw.gridUavIndex];
        grid[cluster] = uint2(first, count);
    }
}
