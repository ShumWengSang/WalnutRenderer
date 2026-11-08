// Hello-triangle shaders. Compiled at runtime by DXC (see TrianglePass.cpp).

#include "common/Bindless.hlsli"

// Root constants at b0 (root parameter DrawConstants). Must match TriangleDrawConstants in TrianglePass.cpp.
struct DrawConstants
{
    uint  vertexBufferIndex; // descriptor heap slot of the vertex buffer's SRV
    uint  textureIndex;      // descriptor heap slot of the checkerboard texture's SRV
    uint  filterMode;        // TextureFilterMode in TrianglePass.h
    float uvScale;           // how many times the texture repeats across the triangle
};
ConstantBuffer<DrawConstants> g_draw : register(b0);

static const uint kFilterPointNoMips    = 0;
static const uint kFilterBilinearNoMips = 1;
static const uint kFilterTrilinear      = 2;
static const uint kFilterAnisotropic    = 3;
static const uint kFilterShowMipLevel   = 4;

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
    Texture2D<float4> checker = ResourceDescriptorHeap[g_draw.textureIndex];
    const float2 uv = input.uv * g_draw.uvScale; // beyond 0..1, the WRAP samplers repeat the texture

    // Sample() picks the mip level automatically: the GPU shades pixels in 2x2 groups, compares the UVs of
    // neighbouring pixels, and from how fast the UV changes per pixel works out how many texels one pixel covers.
    // Covering 4x4 texels -> mip 2, and so on. SampleLevel(..., 0) skips that and always reads mip 0.
    float3 albedo;
    if (g_draw.filterMode == kFilterPointNoMips)
        albedo = checker.SampleLevel(g_samplerPointWrap, uv, 0).rgb;
    else if (g_draw.filterMode == kFilterBilinearNoMips)
        albedo = checker.SampleLevel(g_samplerLinearWrap, uv, 0).rgb;
    else if (g_draw.filterMode == kFilterTrilinear)
        albedo = checker.Sample(g_samplerLinearWrap, uv).rgb;
    else if (g_draw.filterMode == kFilterAnisotropic)
        albedo = checker.Sample(g_samplerAnisotropicWrap, uv).rgb;
    else
    {
        // Debug view: which mip the hardware would use here. 0 = red, 1 = yellow, 2 = green, 3 = cyan, 4+ = blue.
        const float lod = checker.CalculateLevelOfDetail(g_samplerLinearWrap, uv);
        const float3 mipColors[5] = { float3(1, 0, 0), float3(1, 1, 0), float3(0, 1, 0), float3(0, 1, 1), float3(0, 0, 1) };
        const uint level = min((uint)lod, 3u);
        return float4(lerp(mipColors[level], mipColors[level + 1], saturate(lod - level)), 1.0f);
    }

    return float4(input.color * albedo, 1.0f) * g_tint;
}
