// Light markers (L2): a small glowing dot at every point light, so you can see where the lamps are.
//
// One instance per light, six vertices each (two triangles of a square). The vertex shader builds the square around
// the light's position, facing the camera ("billboard"): it reads the light from the same buffer the shading uses,
// so there is no vertex buffer at all. Drawn after the scene with additive blending (the glow adds to what's behind)
// and the depth test on but no depth writes: walls in front of a lamp hide its marker.

#include "common/Bindless.hlsli"
#include "common/FrameConstants.hlsli"
#include "common/PointLight.hlsli"

// Root constants at b0. Must match LightMarkerDrawConstants in LightMarkerPass.cpp.
struct DrawConstants
{
    float markerRadius; // meters
};
WALNUT_CONSTANT_BUFFER(DrawConstants, g_draw, b0);

struct VSOutput
{
    float4 position : SV_Position;
    float2 corner   : TEXCOORD0; // -1..1 across the square
    float3 color    : COLOR0;
};

VSOutput VSMain(uint vertexId : SV_VertexID, uint lightIndex : SV_InstanceID)
{
    StructuredBuffer<PointLight> lights = ResourceDescriptorHeap[g_frame.points.bufferIndex];
    const PointLight light = lights[lightIndex];

    // Two triangles: (0,1,2) and (2,1,3) of the corners (-1,-1), (1,-1), (-1,1), (1,1).
    static const uint kCornerOrder[6] = { 0, 1, 2, 2, 1, 3 };
    const uint c = kCornerOrder[vertexId];
    const float2 corner = float2((c & 1) ? 1.0f : -1.0f, (c & 2) ? 1.0f : -1.0f);

    // The square's two axes, both perpendicular to the line from the camera to the light.
    const float3 toLight = normalize(light.position - g_frame.camera.position);
    // (Looking straight up or down at a light, "up" and toLight are parallel; use x as the reference instead.)
    const float3 reference = abs(toLight.y) > 0.999f ? float3(1.0f, 0.0f, 0.0f) : float3(0.0f, 1.0f, 0.0f);
    const float3 right = normalize(cross(reference, toLight));
    const float3 up = cross(toLight, right);
    const float3 worldPosition = light.position + (right * corner.x + up * corner.y) * g_draw.markerRadius;

    VSOutput output;
    output.position = mul(float4(worldPosition, 1.0f), g_frame.camera.viewProjection);
    output.corner = corner;
    // The light's color at a fixed brightness (well above 1, so the tonemapper shows it as a bright spot).
    output.color = light.intensity / max(max(light.intensity.r, light.intensity.g), max(light.intensity.b, 1e-6f)) * 8.0f;
    return output;
}

float4 PSMain(VSOutput input) : SV_Target
{
    // A round, soft dot: full at the center, fading to 0 at the circle's edge (and 0 in the square's corners).
    const float falloff = saturate(1.0f - dot(input.corner, input.corner));
    return float4(input.color * falloff * falloff, 1.0f);
}
