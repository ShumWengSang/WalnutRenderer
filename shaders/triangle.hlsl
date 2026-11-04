// Hello-triangle shaders. Compiled at runtime by DXC (see TrianglePass.cpp).

// Root constants (root parameter 0). Layout must match TriangleConstants in TrianglePass.h.
cbuffer TriangleConstants : register(b0)
{
    float4 g_tint;
    float2 g_offset;  // NDC translation
    float  g_angle;   // radians
    float  g_scale;
    float  g_aspect;  // width / height, keeps the triangle from stretching
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
    float s, c;
    sincos(g_angle, s, c);
    float2 p = input.position.xy * g_scale;
    p = float2(p.x * c - p.y * s, p.x * s + p.y * c);
    p.x /= g_aspect;
    p += g_offset;

    PSInput output;
    output.position = float4(p, input.position.z, 1.0f);
    output.color = input.color;
    return output;
}

float4 PSMain(PSInput input) : SV_Target
{
    return float4(input.color, 1.0f) * g_tint;
}
