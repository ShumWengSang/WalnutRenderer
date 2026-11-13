// Lighting shared by forward shading (scene.hlsl) and the deferred lighting pass (deferred.hlsl): the sun's shadow
// (shadow map or ray), the sky/ground ambient, and ShadeSurface, which combines them with the BRDF.
//
// Expects RAYTRACED_SHADOWS to be defined (0 or 1) before it's included.
#ifndef WALNUT_LIGHTING_HLSLI
#define WALNUT_LIGHTING_HLSLI

#include "common/Bindless.hlsli"
#include "common/Brdf.hlsli"
#include "common/Clusters.hlsli"
#include "common/DebugViews.hlsli"
#include "common/FrameConstants.hlsli"
#include "common/SceneData.hlsli"

// Everything the lighting needs from the material at one pixel: its textures and factors, evaluated. Forward
// shading gets it from SampleMaterial (scene.hlsl); deferred shading reads it back from the G-buffer.
struct MaterialSample
{
    float3 baseColor;
    float3 normal;          // shading normal: normal-mapped, flipped for back faces
    float3 geometricNormal; // the surface itself, without the normal map (shadow bias, ray offsets)
    float  roughness;
    float  metallic;
    float  occlusion;       // 1 = open to the sky, 0 = fully hidden (ambient only)
    float3 emissive;
};

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

#if RAYTRACED_SHADOWS
// Does this candidate hit on an alpha-tested triangle land on an opaque part of its texture?
//
// A ray hit only reports WHICH triangle (instance + primitive index) and WHERE on it (barycentrics: how much of each
// corner). Everything else, the shader looks up itself, the same bindless way the vertex shader does:
//   instance -> material and firstIndex -> the triangle's 3 indices -> 3 vertices' UVs -> blend by barycentrics
//   -> sample the base color texture's alpha -> compare with the cutoff.
bool IsOpaqueHit(uint instanceIndex, uint triangleIndex, float2 barycentrics)
{
    StructuredBuffer<Instance> instances = ResourceDescriptorHeap[g_frame.instanceBufferIndex];
    StructuredBuffer<Material> materials = ResourceDescriptorHeap[g_frame.materialBufferIndex];
    StructuredBuffer<uint> indices = ResourceDescriptorHeap[g_frame.indexBufferIndex];
    StructuredBuffer<Vertex> vertices = ResourceDescriptorHeap[g_frame.vertexBufferIndex];

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
    RaytracingAccelerationStructure scene = ResourceDescriptorHeap[g_frame.tlasIndex];

    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = 0.0f;
    ray.TMax = maxDistance;

    // ACCEPT_FIRST_HIT_AND_END_SEARCH: any hit means shadow, no need to find the closest one.
    // SKIP_PROCEDURAL_PRIMITIVES: the scene only has triangles; lets the compiler drop that path.
    // FORCE_OPAQUE (only with alpha testing turned off): treat every triangle as solid, no candidates at all.
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> query;
    query.TraceRayInline(scene, g_frame.rayAlphaTest ? RAY_FLAG_NONE : RAY_FLAG_FORCE_OPAQUE, 0xFF /*instance mask: all*/, ray);
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
    if (g_frame.shadowsEnabled == 0)
        return 1.0f;
    // Start a little above the surface: from exactly on it, floating-point rounding could make the ray hit the very
    // triangle it starts on ("self-intersection", the ray tracing cousin of shadow acne). 1 cm here.
    // TMax "far enough": the sun is infinitely far away; anything in the scene counts as a blocker.
    return TraceShadowRay(worldPosition + normal * 0.01f, g_frame.sunDirection, 1.0e4f);
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

// One point light (L1). Must match PointLightGpu in PointLights.h (32 bytes).
struct PointLight
{
    float3 position;  // world space
    float  range;     // meters: the light fades to exactly zero here
    float3 intensity; // linear color * intensity
    uint   shadowSlot; // cube shadow map slot (PointShadowMaps), 0xFFFFFFFF: no shadow
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

// Point light shadows from cube shadow maps (M2). The lamp's cube was rendered by six 90-degree cameras at the lamp
// (PointShadowMaps.cpp). Sampling a cube with a direction makes the hardware pick the face and the texel the ray from
// the lamp's center passes through; SampleCmp then compares our depth with the depth stored there, like SunShadow.
static const uint  kNoShadowSlot = 0xFFFFFFFF;
static const uint  kPointShadowOff       = 0; // PointShadowTechnique in Lighting.h
static const uint  kPointShadowCubeMaps  = 1;
static const uint  kPointShadowRayTraced = 2;
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
    // No shadows: their light goes through walls. Lights out of range or behind the surface are skipped.
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
    // Shadowed lamps' visibility: white = lit, black = blocked, dark blue = no shadowed lamp reaches this point.
    else if (view == kViewPointShadowDifference)
    {
        // Cube maps vs rays, for the lamps that have a cube: the same colors as the sun's difference view (red: only
        // the cube map shadows, blue: only the ray does), dark blue where no such lamp reaches the point.
        const float3 c = shading.pointShadowCompare;
        color = c.z > 0.0f ? ShadowDifferenceColor(c.y / c.z, c.x / c.z) : float3(0.03f, 0.05f, 0.15f);
    }
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
