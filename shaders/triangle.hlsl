// Hello-triangle shaders. Compiled at runtime by DXC (see TrianglePass.cpp).

#include "common/Bindless.hlsli"

// Root constants at b0 (root parameter DrawConstants). Must match TriangleDrawConstants in TrianglePass.cpp.
struct DrawConstants
{
    uint vertexBufferIndex; // descriptor heap slot of the vertex buffer's SRV
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

// One element of the vertex buffer. Must match Vertex in TrianglePass.cpp (24 bytes).
struct Vertex
{
    float3 position;
    float3 color;
};

struct PSInput
{
    float4 position : SV_Position;
    float3 color    : COLOR;
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
    return output;
}

float4 PSMain(PSInput input) : SV_Target
{
    return float4(input.color, 1.0f) * g_tint;
}
