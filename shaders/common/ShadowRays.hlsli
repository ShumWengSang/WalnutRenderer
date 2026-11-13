// Shadow rays with inline ray tracing (Step 2: I2-I3, Step 6: M3). Only compiled into the RAYTRACED_SHADOWS=1
// variants, which must define it before including this.
#ifndef WALNUT_SHADOW_RAYS_HLSLI
#define WALNUT_SHADOW_RAYS_HLSLI

#include "common/Bindless.hlsli"
#include "common/FrameConstants.hlsli"
#include "common/SceneData.hlsli"

#if RAYTRACED_SHADOWS
// Does this candidate hit on an alpha-tested triangle land on an opaque part of its texture?
//
// A ray hit only reports WHICH triangle (instance + primitive index) and WHERE on it (barycentrics: how much of each
// corner). Everything else, the shader looks up itself, the same bindless way the vertex shader does:
//   instance -> material and firstIndex -> the triangle's 3 indices -> 3 vertices' UVs -> blend by barycentrics
//   -> sample the base color texture's alpha -> compare with the cutoff.
bool IsOpaqueHit(uint instanceIndex, uint triangleIndex, float2 barycentrics)
{
    StructuredBuffer<Instance> instances = ResourceDescriptorHeap[g_frame.scene.instanceBufferIndex];
    StructuredBuffer<Material> materials = ResourceDescriptorHeap[g_frame.scene.materialBufferIndex];
    StructuredBuffer<uint> indices = ResourceDescriptorHeap[g_frame.scene.indexBufferIndex];
    StructuredBuffer<Vertex> vertices = ResourceDescriptorHeap[g_frame.scene.vertexBufferIndex];

    const Instance instance = instances[instanceIndex];
    const Material material = materials[instance.materialIndex];
    const uint first = instance.firstIndex + triangleIndex * 3;
    const float2 uv0 = vertices[indices[first + 0]].uv;
    const float2 uv1 = vertices[indices[first + 1]].uv;
    const float2 uv2 = vertices[indices[first + 2]].uv;
    // Barycentrics (b1, b2) are the weights of corners 1 and 2; corner 0 gets the rest.
    const float2 uv = uv0 * (1.0f - barycentrics.x - barycentrics.y) + uv1 * barycentrics.x + uv2 * barycentrics.y;

    float alpha = material.baseColorFactor.a;
    if (material.baseColorTexture != kNoTexture)
    {
        // Unlike in the pixel shader's own material lookup, this texture index differs from pixel to pixel (each
        // pixel's ray hits a different leaf, maybe with a different material). The GPU runs pixels in groups that
        // share one descriptor lookup unless told otherwise: NonUniformResourceIndex says "may differ per pixel".
        Texture2D<float4> texture = ResourceDescriptorHeap[NonUniformResourceIndex(material.baseColorTexture)];
        // SampleLevel 0: a ray has no screen-space neighbours to compute a mip level from, so use the full-size mip
        // (correct, if more memory traffic than needed; "ray cones" estimate a proper level).
        alpha *= texture.SampleLevel(g_samplerLinearWrap, uv, 0).a;
    }
    return alpha >= material.alphaCutoff;
}

// The same question as SunShadow, answered by tracing a ray: is anything between this point and the sun?
//
// RayQuery is "inline ray tracing" (DXR 1.1): any shader can trace a ray and get the answer back right away, without
// the separate ray-generation/hit shaders and shader tables of full DXR. The hardware walks the TLAS and BLAS boxes.
//
// For a shadow ray we don't need the closest hit, only whether there is ANY hit, so the flags let the hardware stop
// at the first one found and skip work it would need for shading a hit.
//
// Opaque triangles are accepted by the hardware on its own. Alpha-tested ones (built without the OPAQUE flag in I1)
// come back to the shader as "candidates": Proceed() pauses on each one, the shader checks the texture, and commits
// the hit only if the ray really hits leaf. Otherwise the traversal continues as if that triangle weren't there.
//
// Shared by the sun (to infinity) and the point lights (up to the lamp, M3): 1 = nothing in the way, 0 = blocked.
float TraceShadowRay(float3 origin, float3 direction, float maxDistance)
{
    RaytracingAccelerationStructure scene = ResourceDescriptorHeap[g_frame.scene.tlasIndex];

    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = 0.0f;
    ray.TMax = maxDistance;

    // ACCEPT_FIRST_HIT_AND_END_SEARCH: any hit means shadow, no need to find the closest one.
    // SKIP_PROCEDURAL_PRIMITIVES: the scene only has triangles; lets the compiler drop that path.
    // FORCE_OPAQUE (only with alpha testing turned off): treat every triangle as solid, no candidates at all.
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> query;
    query.TraceRayInline(scene, g_frame.scene.rayAlphaTest ? RAY_FLAG_NONE : RAY_FLAG_FORCE_OPAQUE, 0xFF /*instance mask: all*/, ray);
    while (query.Proceed())
    {
        // Only non-opaque triangles stop here (no procedural geometry in the scene).
        if (IsOpaqueHit(query.CandidateInstanceID(), query.CandidatePrimitiveIndex(), query.CandidateTriangleBarycentrics()))
            query.CommitNonOpaqueTriangleHit(); // with ACCEPT_FIRST_HIT this also ends the search
    }
    return query.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? 0.0f : 1.0f;
}

float RayTracedSunShadow(float3 worldPosition, float3 normal)
{
    if (g_frame.sunShadow.enabled == 0)
        return 1.0f;
    // Start a little above the surface: from exactly on it, floating-point rounding could make the ray hit the very
    // triangle it starts on ("self-intersection", the ray tracing cousin of shadow acne). 1 cm here.
    // TMax "far enough": the sun is infinitely far away; anything in the scene counts as a blocker.
    return TraceShadowRay(worldPosition + normal * 0.01f, g_frame.sun.direction, 1.0e4f);
}

// The same question for a point light (M3): is anything between this point and the lamp? The ray stops at the lamp
// (TMax = its distance): geometry behind the lamp can't block it. Lamps are points, not geometry in the TLAS, so
// nothing at the end gets in the way.
float RayTracedPointShadow(float3 worldPosition, float3 geometricNormal, float3 lightPosition)
{
    const float3 toLight = lightPosition - worldPosition;
    // Lift the origin 1 cm off the surface on the lamp's side, like the sun's rays.
    const float3 n = dot(geometricNormal, toLight) < 0.0f ? -geometricNormal : geometricNormal;
    const float3 origin = worldPosition + n * 0.01f;
    const float3 toLightFromOrigin = lightPosition - origin;
    const float distance = length(toLightFromOrigin);
    return TraceShadowRay(origin, toLightFromOrigin / distance, distance);
}
#endif

#endif
