// Deferred shading's second half: one fullscreen pass that lights every screen pixel once, from what the G-buffer
// pass (PSGBuffer in scene.hlsl) stored. The lighting is the forward path's own code (ShadeSurface in Shading.hlsli);
// only where its inputs come from differs.

#ifndef RAYTRACED_SHADOWS
#define RAYTRACED_SHADOWS 0 // DeferredPass compiles a second variant with 1 (shadow rays, sun and/or points), like ScenePass
#endif

#include "common/Bindless.hlsli"
#include "common/Fullscreen.hlsli"
#include "common/GBuffer.hlsli"
#include "common/Lighting.hlsli"

// Root constants at b0. Must match DeferredInputs in GBuffer.h. (The debug view is in the frame constants.)
struct DrawConstants
{
    uint baseColorIndex; // bindless SRVs of the G-buffer targets and the depth buffer
    uint normalsIndex;
    uint materialIndex;
    uint emissiveIndex;
    uint depthIndex;
};
WALNUT_CONSTANT_BUFFER(DrawConstants, g_draw, b0);

// One triangle that covers the whole screen (common/Fullscreen.hlsli).
float4 VSMain(uint vertexId : SV_VertexID) : SV_Position
{
    return FullscreenTrianglePosition(vertexId);
}

// The G-buffer doesn't store positions: the depth buffer already says how far away each pixel's surface is, and the
// pixel's place on screen says in which direction. Undoing the camera's projection turns the two back into a point:
//   1. pixel -> normalized device coordinates: x from -1 (left) to 1 (right), y from 1 (top) to -1 (bottom)
//   2. (x, y, depth, 1) times the inverse view-projection matrix -> a homogeneous world position
//   3. divide by w, undoing the perspective divide the rasterizer did
float3 WorldPositionFromDepth(float2 pixelCenter, float depth, float2 screenSize)
{
    const float2 ndc = float2(pixelCenter.x / screenSize.x * 2.0f - 1.0f, 1.0f - pixelCenter.y / screenSize.y * 2.0f);
    const float4 world = mul(float4(ndc, depth, 1.0f), g_frame.camera.inverseViewProjection);
    return world.xyz / world.w;
}

float4 PSMain(float4 position : SV_Position) : SV_Target
{
    // Load, not Sample: one G-buffer texel per screen pixel, read exactly (no filtering, no sampler).
    // SV_Position.xy is the pixel's center (x + 0.5, y + 0.5), the same point the G-buffer pass rasterized.
    const int3 pixel = int3(position.xy, 0);

    // Where nothing was drawn, the depth buffer still holds its clear value (0 = infinitely far, reversed-Z) and the
    // G-buffer holds garbage from earlier frames (it isn't cleared). Leave the scene color's clear color there.
    Texture2D<float> depthBuffer = ResourceDescriptorHeap[g_draw.depthIndex];
    const float depth = depthBuffer.Load(pixel);
    if (depth == 0.0f)
        discard;

    Texture2D<float4> baseColorTarget = ResourceDescriptorHeap[g_draw.baseColorIndex];
    Texture2D<float4> normalsTarget = ResourceDescriptorHeap[g_draw.normalsIndex];
    Texture2D<float4> materialTarget = ResourceDescriptorHeap[g_draw.materialIndex];
    Texture2D<float4> emissiveTarget = ResourceDescriptorHeap[g_draw.emissiveIndex];

    // Unpack the G-buffer into the same MaterialSample the forward path builds from the material's textures.
    const float4 normals = normalsTarget.Load(pixel);
    const float4 material = materialTarget.Load(pixel);
    MaterialSample mat;
    mat.baseColor = baseColorTarget.Load(pixel).rgb; // the _SRGB view decodes to linear
    mat.normal = OctahedralDecode(normals.xy);
    mat.geometricNormal = OctahedralDecode(normals.zw);
    mat.roughness = material.r;
    mat.metallic = material.g;
    mat.occlusion = material.b;
    mat.emissive = emissiveTarget.Load(pixel).rgb;

    float width, height;
    depthBuffer.GetDimensions(width, height);
    const float3 worldPosition = WorldPositionFromDepth(position.xy, depth, float2(width, height));

    const ShadingResult shading = ShadeSurface(worldPosition, mat, g_frame.shading.debugView == kViewShadowDifference,
                                               g_frame.shading.debugView == kViewPointShadowDifference);
    float3 color;
    if (!SharedViewColor(g_frame.shading.debugView, worldPosition, mat, shading, color))
        color = float3(1.0f, 0.0f, 1.0f); // forward-only view (the UI doesn't offer it in deferred mode)
    return float4(color, 1.0f);
}
