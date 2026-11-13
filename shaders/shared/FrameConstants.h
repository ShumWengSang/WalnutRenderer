// Everything shaders need that is the same for the whole frame: g_frame at b2 (root parameter FrameData), set once per
// frame by Renderer::SetFrameConstants and readable by any pass. Shared by C++ (walnut::FrameConstants, filled by
// BuildFrameConstants in renderer/FrameConstants.cpp) and HLSL (common/FrameConstants.hlsli), so the layout can't drift.
//
// Made of blocks whose sizes are multiples of 16 bytes: HLSL starts every struct on a new 16-byte register, so with
// whole-register blocks the C++ and HLSL layouts are the same by construction (checked by the static_asserts below).
#ifndef WALNUT_SHARED_FRAME_CONSTANTS_H
#define WALNUT_SHARED_FRAME_CONSTANTS_H

#include "shared/Shared.h"

WALNUT_GPU_NAMESPACE_BEGIN

struct CameraConstants
{
    WALNUT_ROW_MAJOR float4x4 viewProjection;
    WALNUT_ROW_MAJOR float4x4 inverseViewProjection; // clip space -> world (deferred: position from depth)
    WALNUT_ROW_MAJOR float4x4 view;                  // world -> view space (clustered lighting builds boxes there)
    float3 position;                                 // world space
    float  tanHalfFovX;
    float  tanHalfFovY;
    uint   padding0, padding1, padding2;
};

// The sun: a directional light.
struct SunConstants
{
    float3 direction;   // world space, unit length, pointing TOWARDS the sun
    uint   padding0;
    float3 illuminance; // linear color * intensity: light arriving on a surface facing the sun
    uint   padding1;
};

// The sun's shadow (H1-H5, I1-I4). Bias sizes are in shadow-map texels: one texel covers texelWorldSize meters.
struct SunShadowConstants
{
    WALNUT_ROW_MAJOR float4x4 viewProjection; // world -> the shadow map's space (SceneLighting::GetSunViewProjection)
    uint   mapIndex;       // bindless SRV of the shadow map (Texture2D<float>)
    uint   enabled;        // 0: the sun reaches everything
    uint   raytraced;      // 1: from a ray (ray-traced shader variant only); 0: from the shadow map
    uint   filterRadius;   // PCF kernel: (2r+1)^2 filtered taps; 0 = one tap; 0xFFFFFFFF = hard (point comparison)
    float  texelWorldSize; // meters covered by one shadow-map texel
    float  depthPerMeter;  // the sun's depth is linear (orthographic): moving 1 m towards the sun adds this
    float  constantBias;   // texels: push every comparison towards the sun by this much
    float  slopeBias;      // texels: ... plus this much times tan(angle to the sun)
    float  normalOffset;   // texels: look up the shadow map this far out along the surface normal
    uint   padding0, padding1, padding2;
};

// Ambient light (J4): a "hemisphere light", standing in for the sky and the light bounced off the ground.
struct AmbientConstants
{
    float3 skyRadiance;    // linear color * intensity: light arriving from above the horizon
    uint   padding0;
    float3 groundRadiance; // ... and from below it
    uint   padding1;
};

struct ShadingConstants
{
    uint   model;             // ShadingModel (renderer/RenderSettings.h): kShadingLambert or kShadingPbr
    float  roughnessOverride; // >= 0: every material uses this roughness (J5 comparisons)
    float  metallicOverride;  // >= 0: every material uses this metallic
    uint   debugView;         // what the shaders output: kView* (shared/DebugViewList.h)
    uint   normalMapping;     // NormalMapping (renderer/RenderSettings.h): kNormalMapping*
    uint   padding0, padding1, padding2;
};

// Point lights (L1, M1-M3).
struct PointLightConstants
{
    uint   bufferIndex;     // bindless StructuredBuffer<PointLight> (common/PointLight.hlsli)
    uint   count;           // how many of its lights to use
    uint   shadowTechnique; // PointShadowTechnique (renderer/RenderSettings.h): kPointShadow*
    uint   shadowMapIndex;  // bindless TextureCubeArray<float>: the cube shadow maps
    float  shadowNearScale; // the cube face cameras' near plane, as a fraction of the light's range
    uint   padding0, padding1, padding2;
};

// Clustered lighting (L3/L4): the depth range the slices cover, and the grid itself (common/Clusters.hlsli).
struct ClusterConstants
{
    float  nearDepth;         // view-space depth (meters) where slice 0 starts: the camera's near plane
    float  farDepth;          // ... and where the last slice ends: the farthest corner of the scene
    uint   gridIndex;         // StructuredBuffer<uint2>: (first index, count) per cluster
    uint   lightIndicesIndex; // StructuredBuffer<uint>: the clusters' light indices
    uint   enabled;           // 1: point lights from the pixel's cluster list; 0: every light (brute force)
    uint   padding0, padding1, padding2;
};

// The scene's buffers and acceleration structure (bindless SRVs): the scene draws pull their vertices, instances and
// materials from them, and shadow rays look up alpha-tested hits in them.
struct SceneBindings
{
    uint   vertexBufferIndex;
    uint   instanceBufferIndex;
    uint   materialBufferIndex;
    uint   indexBufferIndex;
    uint   tlasIndex;    // RaytracingAccelerationStructure (ray-traced shader variants only)
    uint   rayAlphaTest; // 0: shadow rays treat alpha-tested geometry as solid
    uint   padding0, padding1;
};

struct FrameConstants
{
    CameraConstants     camera;
    SunConstants        sun;
    SunShadowConstants  sunShadow;
    AmbientConstants    ambient;
    ShadingConstants    shading;
    PointLightConstants points;
    ClusterConstants    clusters;
    SceneBindings       scene;
};

WALNUT_GPU_NAMESPACE_END

#ifndef __HLSL_VERSION
#include <cstddef>

namespace walnut
{
    using gpu::FrameConstants;

    static_assert(sizeof(gpu::CameraConstants) == 224);
    static_assert(sizeof(gpu::SunConstants) == 32);
    static_assert(sizeof(gpu::SunShadowConstants) == 112);
    static_assert(sizeof(gpu::AmbientConstants) == 32);
    static_assert(sizeof(gpu::ShadingConstants) == 32);
    static_assert(sizeof(gpu::PointLightConstants) == 32);
    static_assert(sizeof(gpu::ClusterConstants) == 32);
    static_assert(sizeof(gpu::SceneBindings) == 32);
    // Every block a whole number of registers, and no padding between blocks: then HLSL places them the same way.
    static_assert(offsetof(FrameConstants, sun) == 224);
    static_assert(offsetof(FrameConstants, sunShadow) == 256);
    static_assert(offsetof(FrameConstants, ambient) == 368);
    static_assert(offsetof(FrameConstants, shading) == 400);
    static_assert(offsetof(FrameConstants, points) == 432);
    static_assert(offsetof(FrameConstants, clusters) == 464);
    static_assert(offsetof(FrameConstants, scene) == 496);
    static_assert(sizeof(FrameConstants) == 528);
}
#endif

#endif
