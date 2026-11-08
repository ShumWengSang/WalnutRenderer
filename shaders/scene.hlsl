// Scene shaders: draws every instance of the loaded scene. Compiled at runtime by DXC (see ScenePass.cpp).

#include "common/Bindless.hlsli"

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

static const uint kViewNormals         = 0;
static const uint kViewTexcoords       = 1;
static const uint kViewInstances       = 2;
static const uint kViewMaterials       = 3;
static const uint kViewBaseColorFactor = 4;
static const uint kViewAlphaMode       = 5;

static const uint kAlphaOpaque = 0; // AlphaMode in Scene.h
static const uint kAlphaMask   = 1;
static const uint kAlphaBlend  = 2;

// Set once per pass at b1. Must match ScenePassConstants in ScenePass.cpp.
cbuffer PassConstants : register(b1)
{
    row_major float4x4 g_viewProjection;
};

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

// Must match MaterialGpuData in ScenePass.cpp (48 bytes).
struct Material
{
    float4 baseColorFactor; // linear
    float3 emissiveFactor;
    float metallicFactor;
    float roughnessFactor;
    float alphaCutoff;
    uint alphaMode;
    uint doubleSided;
};

struct PSInput
{
    float4 position : SV_Position;
    float3 normal   : NORMAL;
    float2 uv       : TEXCOORD0;
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
    output.position = mul(worldPosition, g_viewProjection);
    output.normal = mul(float4(vertex.normal, 0.0f), instance.normalMatrix).xyz; // w = 0: directions don't move
    output.uv = vertex.uv;
    return output;
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
    float3 normal = normalize(input.normal); // interpolation shortens normals, so normalize again
    if (!isFrontFace)
        normal = -normal;

    float3 color;
    if (g_draw.debugView == kViewNormals)
        color = normal * 0.5f + 0.5f;
    else if (g_draw.debugView == kViewTexcoords)
        color = float3(frac(input.uv), 0.0f); // frac: glTF UVs may go beyond 0..1 (repeating textures)
    else if (g_draw.debugView == kViewInstances)
        color = HashColor(g_draw.instanceIndex);
    else if (g_draw.debugView == kViewMaterials)
        color = HashColor(materialIndex);
    else if (g_draw.debugView == kViewBaseColorFactor)
        color = material.baseColorFactor.rgb;
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
