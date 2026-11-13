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
float RayTracedSunShadow(float3 worldPosition, float3 normal)
{
    if (g_frame.shadowsEnabled == 0)
        return 1.0f;

    RaytracingAccelerationStructure scene = ResourceDescriptorHeap[g_frame.tlasIndex];

    RayDesc ray;
    // Start a little above the surface: from exactly on it, floating-point rounding could make the ray hit the very
    // triangle it starts on ("self-intersection", the ray tracing cousin of shadow acne). 1 cm here.
    ray.Origin = worldPosition + normal * 0.01f;
    ray.Direction = g_frame.sunDirection;
    ray.TMin = 0.0f;
    ray.TMax = 1.0e4f; // "far enough": the sun is infinitely far away; anything in the scene counts as a blocker

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
    uint   padding;
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

// The light leaving a surface towards the camera, split into the parts the debug views show.
struct ShadingResult
{
    float3 color;       // the Lit view: Lambert or PBR (FrameConstants.shadingModel)
    float3 lighting;    // what a white matte surface would show: the sun's share plus the ambient around the normal
    float3 sunSpecular; // the sun's GGX highlight alone
    float3 ambient;     // sky + ground light, diffuse + reflection, darkened by occlusion
    float3 pointLights; // every point light's diffuse + specular (Lambert mode: diffuse only)
    float  shadow;      // the sun's visibility: 1 = lit, 0 = blocked (shadow map, or ray in the ray-traced variant)
    float  mapShadow;   // ray-traced variant with compareShadowMap only: the shadow map's answer for the same point
};

// Lights one surface point. Forward shading calls it per fragment with the interpolated position; deferred shading
// calls it once per screen pixel with the position rebuilt from the depth buffer. Same code, so same result.
ShadingResult ShadeSurface(float3 worldPosition, MaterialSample mat, bool compareShadowMap)
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
    result.shadow = NdotL > 0.0f ? RayTracedSunShadow(worldPosition, mat.geometricNormal) : 0.0f;
    result.mapShadow = compareShadowMap && NdotL > 0.0f ? SunShadow(worldPosition, mat.geometricNormal, geometricNdotL) : 0.0f;
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

    // Point lights (L1): the same BRDF as the sun, but each light has its own direction and gets weaker with
    // distance. No shadows: their light goes through walls. Brute force for now: every pixel visits every light, and
    // skips the ones out of range or behind the surface. The loop count is the same for the whole screen, so all
    // pixels read the same light at the same time (the GPU fetches it once for a whole wave of pixels).
    StructuredBuffer<PointLight> pointLights = ResourceDescriptorHeap[g_frame.pointLightBufferIndex];
    float3 pointLightIrradiance = 0.0f; // for the Lambert / lighting-only views
    result.pointLights = 0.0f;
    for (uint i = 0; i < g_frame.pointLightCount; ++i)
    {
        const PointLight light = pointLights[i];
        const float3 toLight = light.position - worldPosition;
        const float distanceSquared = dot(toLight, toLight);
        if (distanceSquared >= light.range * light.range)
            continue;
        const float3 L = toLight * rsqrt(distanceSquared);
        const float pointNdotL = saturate(dot(mat.normal, L));
        if (pointNdotL <= 0.0f)
            continue;
        const float3 irradiance = light.intensity * (PointLightFalloff(distanceSquared, light.range) * pointNdotL);
        const BrdfValue pointBrdf = EvaluateBrdf(surface, mat.normal, toCamera, L);
        result.pointLights += (pointBrdf.diffuse + pointBrdf.specular) * irradiance;
        pointLightIrradiance += irradiance;
    }
    result.lighting += pointLightIrradiance / kPi;

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
