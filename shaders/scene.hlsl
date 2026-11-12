// Scene shaders: draws every instance of the loaded scene. Compiled at runtime by DXC (see ScenePass.cpp).

#include "common/Bindless.hlsli"
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

static const float kPi = 3.14159265f;

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
    uint3 padding;
};

// Must match MaterialGpuData in ScenePass.cpp (64 bytes).
struct Material
{
    float4 baseColorFactor; // linear
    float3 emissiveFactor;
    float metallicFactor;
    float roughnessFactor;
    float alphaCutoff;
    uint alphaMode;
    uint doubleSided;
    uint baseColorTexture; // descriptor heap slot of a Texture2D, or kNoTexture
    uint3 padding;
};

struct PSInput
{
    float4 position      : SV_Position;
    float3 worldPosition : POSITION;
    float3 normal        : NORMAL;
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
// The same question as SunShadow, answered by tracing a ray: is anything between this point and the sun?
//
// RayQuery is "inline ray tracing" (DXR 1.1): any shader can trace a ray and get the answer back right away, without
// the separate ray-generation/hit shaders and shader tables of full DXR. The hardware walks the TLAS and BLAS boxes.
//
// For a shadow ray we don't need the closest hit, only whether there is ANY hit, so the flags let the hardware stop
// at the first one found and skip work it would need for shading a hit.
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
    // FORCE_OPAQUE: treat the alpha-tested leaves as solid for now (step I3 lets them be see-through).
    // SKIP_PROCEDURAL_PRIMITIVES: the scene only has triangles; lets the compiler drop that path.
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> query;
    query.TraceRayInline(scene, RAY_FLAG_NONE, 0xFF /*instance mask: all*/, ray);
    query.Proceed(); // with FORCE_OPAQUE there are no candidates to decide on: one call does the whole traversal
    return query.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? 0.0f : 1.0f;
}
#endif

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
    if (dot(normal, normal) > 1e-12f)
    {
        normal = normalize(normal); // interpolation shortens normals, so normalize again
        if (!isFrontFace)
            normal = -normal;
    }
    else
        normal = flatNormal;

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

    // Diffuse lighting (Lambert). A matte surface scatters light equally in all directions; how much it receives
    // depends on how directly it faces the light: N.L, the cosine of the angle between the normal and the light
    // direction (1 facing the sun, 0 edge-on, negative = facing away, so clamped to 0). Its BRDF is albedo / pi
    // (the pi spreads the light over the hemisphere, so a white surface never reflects more than it receives).
    //
    // The sun's light only arrives if nothing blocks it: 'shadow' is 1 (lit) or 0 (in shadow).
    //
    //   sun:     albedo / pi * sunIlluminance * N.L * shadow
    //   ambient: albedo * ambientRadiance    (the same light from every direction, integrated over the hemisphere:
    //                                         pi * radiance, times albedo / pi)
    const float NdotL = saturate(dot(normal, g_frame.sunDirection));
#if RAYTRACED_SHADOWS
    const float shadow = NdotL > 0.0f ? RayTracedSunShadow(input.worldPosition, normal) : 0.0f;
#else
    const float shadow = NdotL > 0.0f ? SunShadow(input.worldPosition, normal, NdotL) : 0.0f; // facing away: dark anyway, skip the lookup
#endif
    const float3 lighting = g_frame.sunIlluminance * NdotL * shadow / kPi + g_frame.ambientRadiance;

    float3 color;
    if (g_draw.debugView == kViewLit)
        color = baseColor.rgb * lighting;
    else if (g_draw.debugView == kViewLightingOnly)
        color = lighting;
    else if (g_draw.debugView == kViewSunShadow)
        color = shadow.xxx;
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
