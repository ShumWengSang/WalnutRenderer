// Scene shaders: draws every instance of the loaded scene. Compiled at runtime by DXC (see ScenePass.cpp).

#include "common/Bindless.hlsli"
#include "common/Brdf.hlsli"
#include "common/FrameConstants.hlsli"

#ifndef ALPHA_TEST
#define ALPHA_TEST 0 // ScenePass compiles the pixel shader twice: with ALPHA_TEST=1 for alpha-mask materials
#endif
#ifndef RAYTRACED_SHADOWS
#define RAYTRACED_SHADOWS 0 // 1: sun shadows from a ray query against the TLAS instead of the shadow map
#endif

// Root constants at b0. Must match SceneDrawConstants in ScenePass.cpp.
struct DrawConstants
{
    uint vertexBufferIndex;   // descriptor heap slot of StructuredBuffer<Vertex>
    uint instanceBufferIndex; // descriptor heap slot of StructuredBuffer<Instance>
    uint materialBufferIndex; // descriptor heap slot of StructuredBuffer<Material>
    uint debugView;           // SceneDebugView in ScenePass.h
    uint instanceIndex;       // which instance this draw is
    uint indexBufferIndex;    // descriptor heap slot of the index buffer as StructuredBuffer<uint> (ray hits)
    uint normalMapping;       // NormalMapping in ScenePass.h
};
ConstantBuffer<DrawConstants> g_draw : register(b0);

static const uint kViewLit          = 0; // SceneDebugView in ScenePass.h
static const uint kViewLightingOnly = 1;
static const uint kViewBaseColor    = 2;
static const uint kViewNormals      = 3;
static const uint kViewTexcoords    = 4;
static const uint kViewInstances    = 5;
static const uint kViewMaterials    = 6;
static const uint kViewAlphaMode    = 7;
static const uint kViewSunShadow    = 8;
static const uint kViewShadowDifference = 9;
static const uint kViewRoughness    = 10;
static const uint kViewMetallic     = 11;
static const uint kViewNormalMap    = 12;
static const uint kViewOcclusion    = 13;
static const uint kViewEmissive     = 14;
static const uint kViewSunSpecular  = 15;
static const uint kViewAmbient      = 16;

static const uint kShadingLambert = 0; // ShadingModel in Lighting.h
static const uint kShadingPbr     = 1;

static const uint kNormalMappingOff     = 0; // NormalMapping in ScenePass.h
static const uint kNormalMappingMesh    = 1;
static const uint kNormalMappingDerived = 2;

static const uint kNoTexture = 0xFFFFFFFF;

static const uint kAlphaOpaque = 0; // AlphaMode in Scene.h
static const uint kAlphaMask   = 1;
static const uint kAlphaBlend  = 2;


// Must match SceneVertex in Scene.h (48 bytes).
struct Vertex
{
    float3 position;
    float3 normal;
    float4 tangent;
    float2 uv;
};

// Must match InstanceGpuData in ScenePass.cpp (144 bytes).
struct Instance
{
    row_major float4x4 world;
    row_major float4x4 normalMatrix;
    uint materialIndex;
    uint firstIndex;  // where its triangles start in the index buffer
    uint2 padding;
};

// Must match MaterialGpuData in ScenePass.cpp (80 bytes).
struct Material
{
    float4 baseColorFactor; // linear
    float3 emissiveFactor;
    float metallicFactor;
    float roughnessFactor;
    float alphaCutoff;
    uint alphaMode;
    uint doubleSided;
    // Descriptor heap slots of Texture2Ds, or kNoTexture.
    uint baseColorTexture;         // sRGB: base color + alpha
    uint metallicRoughnessTexture; // linear: G = roughness, B = metallic
    uint normalTexture;            // linear: tangent-space normal
    uint occlusionTexture;         // linear: R = ambient occlusion
    uint emissiveTexture;          // sRGB: emitted light color
    float normalScale;
    float occlusionStrength;
    uint padding;
};

struct PSInput
{
    float4 position      : SV_Position;
    float3 worldPosition : POSITION;
    float3 normal        : NORMAL;
    float4 tangent       : TANGENT;   // xyz: world-space direction of increasing u; w: bitangent sign (+1/-1)
    float2 uv            : TEXCOORD0;
};

// For an indexed draw, SV_VertexID is the value read from the index buffer, which the loader made point straight
// into the merged vertex buffer.
PSInput VSMain(uint vertexId : SV_VertexID)
{
    StructuredBuffer<Vertex> vertices = ResourceDescriptorHeap[g_draw.vertexBufferIndex];
    StructuredBuffer<Instance> instances = ResourceDescriptorHeap[g_draw.instanceBufferIndex];
    const Vertex vertex = vertices[vertexId];
    const Instance instance = instances[g_draw.instanceIndex];

    const float4 worldPosition = mul(float4(vertex.position, 1.0f), instance.world);

    PSInput output;
    output.position = mul(worldPosition, g_frame.viewProjection);
    output.worldPosition = worldPosition.xyz;
    output.normal = mul(float4(vertex.normal, 0.0f), instance.normalMatrix).xyz; // w = 0: directions don't move
    // A tangent lies IN the surface (unlike the normal, which sticks out of it), so it moves with the surface:
    // the world matrix itself, not the normal matrix.
    output.tangent = float4(mul(float4(vertex.tangent.xyz, 0.0f), instance.world).xyz, vertex.tangent.w);
    output.uv = vertex.uv;
    return output;
}

// Shadow map pass: the same vertices, seen from the sun. Only the position is needed: the pipeline has no pixel
// shader, so the rasterizer just writes each covered texel's depth.
float4 VSShadow(uint vertexId : SV_VertexID) : SV_Position
{
    StructuredBuffer<Vertex> vertices = ResourceDescriptorHeap[g_draw.vertexBufferIndex];
    StructuredBuffer<Instance> instances = ResourceDescriptorHeap[g_draw.instanceBufferIndex];
    const float4 worldPosition = mul(float4(vertices[vertexId].position, 1.0f), instances[g_draw.instanceIndex].world);
    return mul(worldPosition, g_frame.sunViewProjection);
}

// Alpha-tested shadow casters (leaves): the rectangle's depth must only be written where the leaf texture is opaque,
// so these draws need a pixel shader after all. It writes no color (there's no render target), it only decides
// whether the rasterizer may write this texel's depth: 'discard' skips it, exactly like in the scene pass.
struct ShadowAlphaInput
{
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
};

ShadowAlphaInput VSShadowAlphaTest(uint vertexId : SV_VertexID)
{
    StructuredBuffer<Vertex> vertices = ResourceDescriptorHeap[g_draw.vertexBufferIndex];
    StructuredBuffer<Instance> instances = ResourceDescriptorHeap[g_draw.instanceBufferIndex];
    const Vertex vertex = vertices[vertexId];
    const float4 worldPosition = mul(float4(vertex.position, 1.0f), instances[g_draw.instanceIndex].world);

    ShadowAlphaInput output;
    output.position = mul(worldPosition, g_frame.sunViewProjection);
    output.uv = vertex.uv;
    return output;
}

void PSShadowAlphaTest(ShadowAlphaInput input)
{
    StructuredBuffer<Instance> instances = ResourceDescriptorHeap[g_draw.instanceBufferIndex];
    StructuredBuffer<Material> materials = ResourceDescriptorHeap[g_draw.materialBufferIndex];
    const Material material = materials[instances[g_draw.instanceIndex].materialIndex];

    float alpha = material.baseColorFactor.a;
    if (material.baseColorTexture != kNoTexture)
    {
        Texture2D<float4> baseColorTexture = ResourceDescriptorHeap[material.baseColorTexture];
        alpha *= baseColorTexture.Sample(g_samplerAnisotropicWrap, input.uv).a;
    }
    if (alpha < material.alphaCutoff)
        discard;
}

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
    StructuredBuffer<Instance> instances = ResourceDescriptorHeap[g_draw.instanceBufferIndex];
    StructuredBuffer<Material> materials = ResourceDescriptorHeap[g_draw.materialBufferIndex];
    StructuredBuffer<uint> indices = ResourceDescriptorHeap[g_draw.indexBufferIndex];
    StructuredBuffer<Vertex> vertices = ResourceDescriptorHeap[g_draw.vertexBufferIndex];

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

// Samples one of the material's textures, or returns 'fallback' if it has none (glTF: a missing texture counts as 1,
// so the factor alone applies). Every pixel of a draw uses the same material, so the index is uniform.
float4 SampleMaterialTexture(uint textureIndex, float2 uv, float4 fallback)
{
    if (textureIndex == kNoTexture)
        return fallback;
    Texture2D<float4> texture = ResourceDescriptorHeap[textureIndex];
    return texture.Sample(g_samplerAnisotropicWrap, uv);
}

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

// Tangent frame from screen-space derivatives, for meshes without a glTF TANGENT (or to compare with one).
// Between this pixel and its right/lower neighbour the surface moves by dp1/dp2 and the texture coordinates by
// duv1/duv2. Solving
//   dp1 = T * duv1.x + V * duv1.y
//   dp2 = T * duv2.x + V * duv2.y
// gives T = dP/du (the tangent) and V = dP/dv. glTF normal maps have green = "up in the image", and v grows
// downwards, so the bitangent points along -V: w is whichever sign makes cross(N, T) * w do that. (In the engine's
// mirrored space that's -1 for an ordinary mesh, matching the loader's flipped TANGENT.w.)
float4 DerivedTangent(float3 worldPosition, float2 uv, float3 normal)
{
    const float3 dp1 = ddx(worldPosition);
    const float3 dp2 = ddy(worldPosition);
    const float2 duv1 = ddx(uv);
    const float2 duv2 = ddy(uv);
    const float det = duv1.x * duv2.y - duv1.y * duv2.x;
    if (det == 0.0f)
        return 0.0f; // no texture stretch to measure (constant uv): no frame
    const float3 T = (dp1 * duv2.y - dp2 * duv1.y) / det;
    const float3 V = (dp2 * duv1.x - dp1 * duv2.x) / det;
    const float3 t = T - normal * dot(normal, T);
    const float w = dot(cross(normal, t), -V) >= 0.0f ? 1.0f : -1.0f;
    return float4(t, w);
}

// Cheap integer hash -> a stable, distinct color per index.
float3 HashColor(uint index)
{
    uint h = index * 747796405u + 2891336453u;
    h = ((h >> ((h >> 28u) + 4u)) ^ h) * 277803737u;
    h = (h >> 22u) ^ h;
    return float3(h & 0xFF, (h >> 8) & 0xFF, (h >> 16) & 0xFF) / 255.0f;
}

// SV_IsFrontFace: whether the rasterizer saw this triangle's front (counter-clockwise on screen, per the PSO).
// Only visible for double-sided materials (or with culling turned off).
float4 PSMain(PSInput input, bool isFrontFace : SV_IsFrontFace) : SV_Target
{
    // The material is the same for the whole draw, so every pixel reads the same two elements (cheap: cached).
    StructuredBuffer<Instance> instances = ResourceDescriptorHeap[g_draw.instanceBufferIndex];
    StructuredBuffer<Material> materials = ResourceDescriptorHeap[g_draw.materialBufferIndex];
    const uint materialIndex = instances[g_draw.instanceIndex].materialIndex;
    const Material material = materials[materialIndex];

    // The normal points out of the front face. Seen from behind (a leaf's underside), the surface faces the other
    // way, so glTF says to flip it for double-sided materials.
    float3 normal = input.normal;
    // Files without normals leave them zero (normalize would give NaN). Fall back to the triangle's flat normal:
    // ddx/ddy give how the world position changes to the neighbouring pixel, and their cross product is
    // perpendicular to the surface. Computed outside the 'if': derivatives need every pixel of the 2x2 group.
    float3 flatNormal = normalize(cross(ddy(input.worldPosition), ddx(input.worldPosition)));
    if (dot(flatNormal, g_frame.cameraPosition - input.worldPosition) < 0.0f)
        flatNormal = -flatNormal; // we see this side, so it faces us
    const bool hasVertexNormal = dot(normal, normal) > 1e-12f;
    normal = hasVertexNormal ? normalize(normal) : flatNormal; // interpolation shortens normals, so normalize again

    // The surface itself, before any normal map: used for shadow lookups (bias and ray offsets need the real
    // surface; a bumped normal can tilt into it and bring back acne / self-intersection).
    float3 geometricNormal = normal;
    if (hasVertexNormal && !isFrontFace)
        geometricNormal = -geometricNormal;

    // Normal mapping: the normal map stores small tilts of the surface, relative to the surface itself ("tangent
    // space": x along the texture's u, y along v, z straight out). To use one, build that frame at this pixel and
    // turn the stored direction into a world-space one:
    //   T (tangent)   = the direction u increases in, from the mesh (glTF TANGENT) ...
    //   B (bitangent) = cross(N, T) * w  (w picks which way: textures can be mirrored)
    //   N (normal)    = the surface normal
    //   world normal  = T * n.x + B * n.y + N * n.z
    // ... or, for meshes without tangents, worked out from how position and uv change between neighbouring pixels
    // (screen-space derivatives), which is what DerivedTangent does. Computed outside the 'if' below because
    // derivatives need the whole 2x2 pixel group.
    const float4 derivedTangent = DerivedTangent(input.worldPosition, input.uv, normal);
    if (g_draw.normalMapping != kNormalMappingOff && material.normalTexture != kNoTexture)
    {
        float4 tangent = input.tangent;
        tangent.xyz -= normal * dot(normal, tangent.xyz); // interpolation bends it: make it perpendicular again
        const bool hasTangent = dot(tangent.xyz, tangent.xyz) > 1e-12f && tangent.w != 0.0f;
        if (g_draw.normalMapping == kNormalMappingDerived || !hasTangent)
            tangent = derivedTangent;

        if (dot(tangent.xyz, tangent.xyz) > 1e-12f)
        {
            const float3 T = normalize(tangent.xyz);
            const float3 B = cross(normal, T) * tangent.w;
            Texture2D<float4> normalMap = ResourceDescriptorHeap[material.normalTexture];
            float3 n = normalMap.Sample(g_samplerAnisotropicWrap, input.uv).xyz * 2.0f - 1.0f; // 0..1 -> -1..1
            n.xy *= material.normalScale;
            normal = normalize(T * n.x + B * n.y + normal * n.z);
        }
    }
    if (hasVertexNormal && !isFrontFace)
        normal = -normal;

    // Base color = constant factor * texture. The texture is _SRGB, so Sample() returns linear values.
    // The texture index comes from the material, which comes from a root constant, so it's the same for every
    // pixel of the draw ("uniform"). An index that could differ between pixels of one draw would have to be
    // wrapped in NonUniformResourceIndex(), or the GPU may read the wrong texture.
    float4 baseColor = material.baseColorFactor;
    if (material.baseColorTexture != kNoTexture)
    {
        Texture2D<float4> baseColorTexture = ResourceDescriptorHeap[material.baseColorTexture];
        baseColor *= baseColorTexture.Sample(g_samplerAnisotropicWrap, input.uv);
    }

#if ALPHA_TEST
    // Alpha test (glTF alphaMode MASK): pixels whose alpha is below the cutoff are thrown away. 'discard' writes
    // neither color nor depth, so the leaves' outlines come from the texture, not from the (rectangular) triangles.
    //
    // It's compiled only into the pixel shader that masked materials use. A shader that may discard can't have its
    // depth written before it runs ("early-Z"), since the GPU doesn't know yet whether the pixel survives; keeping
    // discard out of the opaque shader keeps that optimization for everything else.
    if (baseColor.a < material.alphaCutoff)
        discard;
#endif

    // The sun's light arrives on a surface facing it with sunIlluminance; tilted away by an angle, the same light
    // spreads over a bigger area, so each point gets cos(angle) = N.L of it (clamped: facing away gets none).
    // It only arrives if nothing blocks it: 'shadow' is 1 (lit) or 0 (in shadow).
    //
    // The surface sends some of that light towards the camera; how much is the BRDF (see Brdf.hlsli):
    //   Lambert (J1): baseColor / pi, the same in every direction (a matte surface)
    //   PBR (J3):     Lambert diffuse for what enters the surface + GGX specular for what bounces off it
    //
    //   sun:     BRDF * sunIlluminance * N.L * shadow
    //   ambient: light from the sky and the ground (J4), see below
    //   emissive: light the surface makes itself (J4)
    const float NdotL = saturate(dot(normal, g_frame.sunDirection));
    // Shadow lookups use the real surface (geometricNormal), not the normal-mapped one.
    const float geometricNdotL = saturate(dot(geometricNormal, g_frame.sunDirection));
#if RAYTRACED_SHADOWS
    const float shadow = NdotL > 0.0f ? RayTracedSunShadow(input.worldPosition, geometricNormal) : 0.0f;
#else
    const float shadow = NdotL > 0.0f ? SunShadow(input.worldPosition, geometricNormal, geometricNdotL) : 0.0f; // facing away: dark anyway, skip the lookup
#endif
    const float3 sunLight = g_frame.sunIlluminance * NdotL * shadow;
    // What a white matte surface would show: the sun's share plus the ambient light around the normal.
    const float3 lighting = sunLight / kPi + HemisphereLight(normal, 1.0f);

    // glTF packs both into one texture: roughness in G, metallic in B (R is unused, or occlusion in some files).
    const float4 metallicRoughness = SampleMaterialTexture(material.metallicRoughnessTexture, input.uv, 1.0f);
    const float roughness = metallicRoughness.g * material.roughnessFactor;
    const float metallic = saturate(metallicRoughness.b * material.metallicFactor);

    const float3 toCamera = normalize(g_frame.cameraPosition - input.worldPosition);
    const SurfaceBrdf surface = MakeSurfaceBrdf(baseColor.rgb, metallic, roughness);
    const BrdfValue brdf = EvaluateBrdf(surface, normal, toCamera, g_frame.sunDirection);
    const float3 sunSpecular = brdf.specular * sunLight;

    // Ambient light, split like the sun's into what enters the surface and what bounces off it:
    //   diffuse:  the hemisphere around the normal, for the light that isn't reflected
    //   specular: what a mirror would show along the reflected view ray (sky above the horizon, ground below),
    //             blurred by roughness, times how much the surface reflects in total (EnvironmentBrdf)
    // Occlusion (the glTF occlusion map: 1 = open to the sky, 0 = deep in a crack) only darkens this ambient
    // part; the sun's shadow is already computed exactly.
    const float NdotV = max(dot(normal, toCamera), 1e-4f);
    const float3 environmentBrdf = EnvironmentBrdf(surface, NdotV);
    const float3 reflected = reflect(-toCamera, normal);
    const float occlusion = lerp(1.0f, SampleMaterialTexture(material.occlusionTexture, input.uv, 1.0f).r,
                                 material.occlusionStrength);
    const float3 ambient = (surface.diffuseColor * (1.0f - environmentBrdf) * HemisphereLight(normal, 1.0f)
                          + environmentBrdf * HemisphereLight(reflected, surface.alpha)) * occlusion;

    // Emitted light (screens, lamps, lava) is added as is: it doesn't depend on any light arriving.
    const float3 emissive = SampleMaterialTexture(material.emissiveTexture, input.uv, 1.0f).rgb * material.emissiveFactor;

    float3 color;
    if (g_draw.debugView == kViewLit)
    {
        if (g_frame.shadingModel == kShadingPbr)
            color = (brdf.diffuse + brdf.specular) * sunLight + ambient + emissive;
        else
            color = baseColor.rgb * lighting; // Lambert: matte only, no occlusion map, no emission
    }
    else if (g_draw.debugView == kViewLightingOnly)
        color = lighting;
    else if (g_draw.debugView == kViewSunSpecular)
        color = sunSpecular;
    else if (g_draw.debugView == kViewAmbient)
        color = ambient;
    else if (g_draw.debugView == kViewSunShadow)
        color = shadow.xxx;
    else if (g_draw.debugView == kViewShadowDifference)
    {
#if RAYTRACED_SHADOWS
        // Both answers for the same pixel: the ray (exact) and the shadow map (what H1-H5 produce). Gray where they
        // agree; red where only the shadow map says "shadow" (acne, bias, blur spilling into lit areas); blue where only
        // the ray says "shadow" (light leaking: peter-panning, thin casters the map's texels missed).
        const float mapShadow = NdotL > 0.0f ? SunShadow(input.worldPosition, geometricNormal, geometricNdotL) : 0.0f;
        const float difference = mapShadow - shadow; // + : map lit, ray shadow (blue)   - : map shadow, ray lit (red)
        color = lerp(0.35f, 0.9f, shadow).xxx * 0.5f;
        color = lerp(color, float3(1.0f, 0.1f, 0.05f), saturate(-difference));
        color = lerp(color, float3(0.1f, 0.35f, 1.0f), saturate(difference));
#else
        color = float3(1.0f, 0.0f, 1.0f); // needs the ray-traced variant (the UI only offers it there)
#endif
    }
    else if (g_draw.debugView == kViewNormals)
        color = normal * 0.5f + 0.5f;
    else if (g_draw.debugView == kViewTexcoords)
        color = float3(frac(input.uv), 0.0f); // frac: glTF UVs may go beyond 0..1 (repeating textures)
    else if (g_draw.debugView == kViewInstances)
        color = HashColor(g_draw.instanceIndex);
    else if (g_draw.debugView == kViewMaterials)
        color = HashColor(materialIndex);
    else if (g_draw.debugView == kViewBaseColor)
        color = baseColor.rgb;
    // The other maps, as stored. Roughness/metallic as read above (before the BRDF's minimum roughness).
    else if (g_draw.debugView == kViewRoughness)
        color = roughness.xxx;
    else if (g_draw.debugView == kViewMetallic)
        color = metallic.xxx;
    else if (g_draw.debugView == kViewNormalMap)
        // Raw tangent-space normal map: (0.5, 0.5, 1) = "straight out of the surface", the typical light blue.
        color = SampleMaterialTexture(material.normalTexture, input.uv, float4(0.5f, 0.5f, 1.0f, 1.0f)).rgb;
    else if (g_draw.debugView == kViewOcclusion)
        color = occlusion.xxx;
    else if (g_draw.debugView == kViewEmissive)
        color = emissive;
    else
    {
        color = material.alphaMode == kAlphaMask  ? float3(1.0f, 0.85f, 0.1f)
              : material.alphaMode == kAlphaBlend ? float3(1.0f, 0.2f, 0.1f)
                                                  : float3(0.4f, 0.4f, 0.4f);
        if (!isFrontFace)
            color *= 0.4f; // back faces darker, to show which surfaces are drawn from both sides
    }
    return float4(color, 1.0f);
}
