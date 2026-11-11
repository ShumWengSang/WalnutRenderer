// Scene shaders: draws every instance of the loaded scene. Compiled at runtime by DXC (see ScenePass.cpp).

#include "common/Bindless.hlsli"
#include "common/FrameConstants.hlsli"

#ifndef ALPHA_TEST
#define ALPHA_TEST 0 // ScenePass compiles the pixel shader twice: with ALPHA_TEST=1 for alpha-mask materials
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
    //   sun:     albedo / pi * sunIlluminance * N.L
    //   ambient: albedo * ambientRadiance    (the same light from every direction, integrated over the hemisphere:
    //                                         pi * radiance, times albedo / pi)
    //
    // No shadows yet: a surface facing the sun is lit even if a roof is in the way.
    const float NdotL = saturate(dot(normal, g_frame.sunDirection));
    const float3 lighting = g_frame.sunIlluminance * NdotL / kPi + g_frame.ambientRadiance;

    float3 color;
    if (g_draw.debugView == kViewLit)
        color = baseColor.rgb * lighting;
    else if (g_draw.debugView == kViewLightingOnly)
        color = lighting;
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
