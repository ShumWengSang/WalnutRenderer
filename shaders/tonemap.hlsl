// Fullscreen pass: reads the HDR scene color texture, applies exposure and a tonemap curve, and writes the
// back buffer. The render target is the back buffer's _SRGB view, so this shader outputs LINEAR colors and the
// hardware encodes them to sRGB as it writes.

#include "common/Bindless.hlsli"
#include "common/Fullscreen.hlsli"

// Root constants at b0. Must match TonemapDrawConstants in TonemapPass.cpp.
struct DrawConstants
{
    uint  sceneColorIndex; // descriptor heap slot of the scene color SRV
    float exposure;        // linear multiplier (2^stops)
    uint  tonemapOperator; // TonemapOperator in TonemapPass.h
};
ConstantBuffer<DrawConstants> g_draw : register(b0);

static const uint kTonemapClamp = 0;
static const uint kTonemapReinhard = 1;
static const uint kTonemapAcesFitted = 2;

// x / (1 + x): maps 0..infinity smoothly onto 0..1 and never clips. Simple, but it compresses everything
// (even mid-tones lose contrast) and bright saturated colors fade towards a flat look.
float3 TonemapReinhard(float3 x)
{
    return x / (1.0f + x);
}

// Krzysztof Narkowicz's curve fit of the ACES filmic tonemapper:
// https://knarkowicz.wordpress.com/2016/01/06/aces-filmic-tone-mapping-curve/
// An S-shaped curve like photographic film: a slightly crushed toe (deeper blacks), more mid-tone contrast,
// and a long, soft shoulder so highlights roll off into white instead of clipping. Applied per channel.
float3 TonemapAcesFitted(float3 x)
{
    const float a = 2.51f;
    const float b = 0.03f;
    const float c = 2.43f;
    const float d = 0.59f;
    const float e = 0.14f;
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

struct VSOutput
{
    float4 position : SV_Position;
};

// The fullscreen triangle (common/Fullscreen.hlsli).
VSOutput VSMain(uint vertexId : SV_VertexID)
{
    VSOutput output;
    output.position = FullscreenTrianglePosition(vertexId);
    return output;
}

float4 PSMain(VSOutput input) : SV_Target
{
    Texture2D<float4> sceneColor = ResourceDescriptorHeap[g_draw.sceneColorIndex];

    // In the pixel shader, SV_Position.xy is this pixel's center in pixels (e.g. (10.5, 3.5)). The scene color
    // texture is exactly the back buffer's size, so Load() reads the matching texel directly, with no sampler
    // and no filtering.
    float3 color = sceneColor.Load(int3(input.position.xy, 0)).rgb;

    // 1. Exposure: like a camera's exposure setting, scale the light before it hits the "film".
    color *= g_draw.exposure;

    // 2. Tonemap: squeeze HDR light (0 .. infinity) into the displayable 0..1 range.
    if (g_draw.tonemapOperator == kTonemapReinhard)
        color = TonemapReinhard(color);
    else if (g_draw.tonemapOperator == kTonemapAcesFitted)
        color = TonemapAcesFitted(color);
    else
        color = saturate(color);

    // 3. sRGB encoding happens in hardware on write (the render target view is R8G8B8A8_UNORM_SRGB).
    return float4(color, 1.0f);
}
