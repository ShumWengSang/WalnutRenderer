// Fullscreen pass: reads the HDR scene color texture and writes the back buffer.
// For now it copies the color as-is; the tonemapping step adds exposure and a tonemap curve here.

#include "common/Bindless.hlsli"

// Root constants at b0. Must match TonemapDrawConstants in TonemapPass.cpp.
struct DrawConstants
{
    uint sceneColorIndex; // descriptor heap slot of the scene color SRV
};
ConstantBuffer<DrawConstants> g_draw : register(b0);

struct VSOutput
{
    float4 position : SV_Position;
};

// The "fullscreen triangle": 3 vertices made up from SV_VertexID, no vertex buffer.
//
//   id 0 -> (-1,  1)   top-left corner of the screen
//   id 1 -> ( 3,  1)   far off to the right
//   id 2 -> (-1, -3)   far off to the bottom
//
// This one oversized triangle covers the whole [-1, 1] screen square; the parts outside are clipped away.
// It's slightly cheaper than two triangles (a quad), which would shade the pixels along the diagonal twice.
VSOutput VSMain(uint vertexId : SV_VertexID)
{
    const float2 corner = float2((vertexId << 1) & 2, vertexId & 2); // (0,0), (2,0), (0,2)
    VSOutput output;
    output.position = float4(corner * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    return output;
}

float4 PSMain(VSOutput input) : SV_Target
{
    Texture2D<float4> sceneColor = ResourceDescriptorHeap[g_draw.sceneColorIndex];

    // In the pixel shader, SV_Position.xy is this pixel's center in pixels (e.g. (10.5, 3.5)). The scene color
    // texture is exactly the back buffer's size, so Load() reads the matching texel directly, with no sampler
    // and no filtering.
    const float3 color = sceneColor.Load(int3(input.position.xy, 0)).rgb;

    // The back buffer can only hold 0..1, so anything brighter is clipped for now.
    return float4(saturate(color), 1.0f);
}
