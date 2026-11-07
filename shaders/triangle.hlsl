// Hello-triangle shaders. Compiled at runtime by DXC (see TrianglePass.cpp).

#include "common/Bindless.hlsli"

// Root constants at b0 (root parameter DrawConstants). Must match TriangleDrawConstants in TrianglePass.cpp.
struct DrawConstants
{
    uint vertexBufferIndex; // descriptor heap slot of the vertex buffer's SRV
    uint textureIndex;      // descriptor heap slot of the checkerboard texture's SRV
};
ConstantBuffer<DrawConstants> g_draw : register(b0);

// Per-draw constant buffer at b1 (root parameter DrawData of the bindless root signature). Layout must match
// TriangleGpuConstants in TrianglePass.cpp.
cbuffer TriangleConstants : register(b1)
{
    // row_major: DirectXMath stores matrices row by row and multiplies row vectors (p * M). HLSL's default
    // packing is column_major, which would effectively transpose the matrix. Declaring row_major makes the
    // memory layouts match, so the CPU can upload the matrix as-is and the shader multiplies the same way.
    row_major float4x4 g_worldViewProjection;
    float4 g_tint;
};

// One element of the vertex buffer. Must match Vertex in TrianglePass.cpp (32 bytes).
struct Vertex
{
    float3 position;
    float3 color;
    float2 uv;
};

struct PSInput
{
    float4 position : SV_Position;
    float3 color    : COLOR;
    float2 uv       : TEXCOORD0; // interpolated across the triangle, like the color
};

// Vertex pulling: instead of the input assembler handing us a vertex, we get only its index (SV_VertexID)
// and read the vertex ourselves from a buffer we look up in the descriptor heap.
PSInput VSMain(uint vertexId : SV_VertexID)
{
    StructuredBuffer<Vertex> vertices = ResourceDescriptorHeap[g_draw.vertexBufferIndex];
    const Vertex vertex = vertices[vertexId];

    PSInput output;
    // Object space -> clip space in one multiply. The rasterizer then divides by w (perspective).
    output.position = mul(float4(vertex.position, 1.0f), g_worldViewProjection);
    output.color = vertex.color;
    output.uv = vertex.uv;
    return output;
}

float4 PSMain(PSInput input) : SV_Target
{
    // Texture from the bindless heap + static sampler s0 (anisotropic, WRAP addressing). UVs go 0..1 across the
    // triangle; multiplying by 2 makes them go 0..2, and WRAP repeats the texture to fill that range.
    Texture2D<float4> checker = ResourceDescriptorHeap[g_draw.textureIndex];
    const float3 albedo = checker.Sample(g_samplerAnisotropicWrap, input.uv * 2.0f).rgb;

    return float4(input.color * albedo, 1.0f) * g_tint;
}
