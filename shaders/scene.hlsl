// Scene shaders: draws every instance of the loaded scene. Compiled at runtime by DXC (see ScenePass.cpp).

#include "common/Bindless.hlsli"

// Root constants at b0. Must match SceneDrawConstants in ScenePass.cpp.
struct DrawConstants
{
    uint vertexBufferIndex;   // descriptor heap slot of StructuredBuffer<Vertex>
    uint instanceBufferIndex; // descriptor heap slot of StructuredBuffer<Instance>
    uint debugView;           // SceneDebugView in ScenePass.h
    uint instanceIndex;       // which instance this draw is
};
ConstantBuffer<DrawConstants> g_draw : register(b0);

static const uint kViewNormals   = 0;
static const uint kViewTexcoords = 1;
static const uint kViewInstances = 2;

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

// Must match InstanceGpuData in ScenePass.cpp (128 bytes).
struct Instance
{
    row_major float4x4 world;
    row_major float4x4 normalMatrix;
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

// Cheap integer hash -> a stable, distinct color per instance.
float3 InstanceColor(uint index)
{
    uint h = index * 747796405u + 2891336453u;
    h = ((h >> ((h >> 28u) + 4u)) ^ h) * 277803737u;
    h = (h >> 22u) ^ h;
    return float3(h & 0xFF, (h >> 8) & 0xFF, (h >> 16) & 0xFF) / 255.0f;
}

float4 PSMain(PSInput input) : SV_Target
{
    float3 color;
    if (g_draw.debugView == kViewNormals)
        color = normalize(input.normal) * 0.5f + 0.5f; // interpolation shortens normals, so normalize again
    else if (g_draw.debugView == kViewTexcoords)
        color = float3(frac(input.uv), 0.0f); // frac: glTF UVs may go beyond 0..1 (repeating textures)
    else
        color = InstanceColor(g_draw.instanceIndex);
    return float4(color, 1.0f);
}
