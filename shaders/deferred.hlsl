// Deferred shading's second half: a fullscreen pass that reads the G-buffer (written by PSGBuffer in scene.hlsl)
// once per screen pixel. K2: shows one G-buffer target, decoded. The lighting pass comes in K3.

#include "common/Bindless.hlsli"
#include "common/GBuffer.hlsli"

// Root constants at b0. Must match DeferredDrawConstants in DeferredPass.cpp.
struct DrawConstants
{
    uint baseColorIndex; // bindless SRVs of the G-buffer targets and the depth buffer
    uint normalsIndex;
    uint materialIndex;
    uint emissiveIndex;
    uint depthIndex;
    uint debugView;      // SceneDebugView in ScenePass.h
};
ConstantBuffer<DrawConstants> g_draw : register(b0);

static const uint kViewBaseColor        = 2; // SceneDebugView in ScenePass.h
static const uint kViewNormals          = 3;
static const uint kViewRoughness        = 10;
static const uint kViewMetallic         = 11;
static const uint kViewOcclusion        = 13;
static const uint kViewEmissive         = 14;
static const uint kViewGeometricNormals = 17;

// One triangle that covers the whole screen (see tonemap.hlsl).
float4 VSMain(uint vertexId : SV_VertexID) : SV_Position
{
    const float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
    return float4(uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
}

float4 PSMain(float4 position : SV_Position) : SV_Target
{
    // Load, not Sample: one G-buffer texel per screen pixel, read exactly (no filtering, no sampler).
    const int3 pixel = int3(position.xy, 0);

    // Where nothing was drawn, the depth buffer still holds its clear value (0 = infinitely far, reversed-Z) and the
    // G-buffer holds garbage from earlier frames (it isn't cleared). Leave the scene color's clear color there.
    Texture2D<float> depthBuffer = ResourceDescriptorHeap[g_draw.depthIndex];
    if (depthBuffer.Load(pixel) == 0.0f)
        discard;

    Texture2D<float4> baseColorTarget = ResourceDescriptorHeap[g_draw.baseColorIndex];
    Texture2D<float4> normalsTarget = ResourceDescriptorHeap[g_draw.normalsIndex];
    Texture2D<float4> materialTarget = ResourceDescriptorHeap[g_draw.materialIndex];
    Texture2D<float4> emissiveTarget = ResourceDescriptorHeap[g_draw.emissiveIndex];

    float3 color;
    if (g_draw.debugView == kViewNormals)
        color = OctahedralDecode(normalsTarget.Load(pixel).xy) * 0.5f + 0.5f;
    else if (g_draw.debugView == kViewGeometricNormals)
        color = OctahedralDecode(normalsTarget.Load(pixel).zw) * 0.5f + 0.5f;
    else if (g_draw.debugView == kViewRoughness)
        color = materialTarget.Load(pixel).rrr;
    else if (g_draw.debugView == kViewMetallic)
        color = materialTarget.Load(pixel).ggg;
    else if (g_draw.debugView == kViewOcclusion)
        color = materialTarget.Load(pixel).bbb;
    else if (g_draw.debugView == kViewEmissive)
        color = emissiveTarget.Load(pixel).rgb;
    else
        color = baseColorTarget.Load(pixel).rgb; // the _SRGB view decodes to linear, like the forward pass's input
    return float4(color, 1.0f);
}
