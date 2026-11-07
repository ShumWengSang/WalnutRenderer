// Hello-triangle shaders. Compiled at runtime by DXC (see TrianglePass.cpp).

#include "common/Bindless.hlsli"

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

struct VSInput
{
    float3 position : POSITION;
    float3 color    : COLOR;
};

struct PSInput
{
    float4 position : SV_Position;
    float3 color    : COLOR;
};

PSInput VSMain(VSInput input)
{
    PSInput output;
    // Object space -> clip space in one multiply. The rasterizer then divides by w (perspective).
    output.position = mul(float4(input.position, 1.0f), g_worldViewProjection);
    output.color = input.color;
    return output;
}

float4 PSMain(PSInput input) : SV_Target
{
    return float4(input.color, 1.0f) * g_tint;
}
