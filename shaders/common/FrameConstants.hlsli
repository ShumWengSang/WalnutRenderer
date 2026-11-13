// Per-frame constants at b2 (root parameter FrameData). Must match FrameConstants in src/renderer/FrameConstants.h.
#ifndef WALNUT_FRAME_CONSTANTS_HLSLI
#define WALNUT_FRAME_CONSTANTS_HLSLI

struct FrameConstants
{
    row_major float4x4 viewProjection;
    row_major float4x4 sunViewProjection; // world -> the sun's shadow map space
    float3 cameraPosition;  // world space
    uint   shadowMapIndex;  // bindless SRV of the sun's shadow map (Texture2D<float>)
    float3 sunDirection;    // world space, unit length, pointing towards the sun
    uint   shadowsEnabled;
    float3 sunIlluminance;  // linear
    uint   shadingModel;    // 0 = Lambert, 1 = PBR (ShadingModel in Lighting.h)
    float3 skyRadiance;     // linear: ambient light from above the horizon
    float  roughnessOverride; // >= 0: replaces every material's roughness
    float3 groundRadiance;  // linear: ambient light from below it
    float  metallicOverride;  // >= 0: replaces every material's metallic

    float  shadowTexelWorldSize; // meters per shadow-map texel
    float  shadowDepthPerMeter;  // depth change per meter towards the sun (linear: orthographic)
    float  shadowConstantBias;   // texels
    float  shadowSlopeBias;      // texels, times tan(angle to the sun)
    float  shadowNormalOffset;   // texels, along the surface normal
    uint   shadowFilterRadius;   // PCF kernel radius in texels; 0xFFFFFFFF = hard (point comparison)
    uint   tlasIndex;            // bindless SRV of the TLAS (RaytracingAccelerationStructure)
    uint   rayAlphaTest;         // 0: shadow rays treat alpha-tested geometry as solid

    row_major float4x4 inverseViewProjection; // clip space -> world (deferred: position from depth)
    uint   vertexBufferIndex;    // the scene's buffers, for ray hits outside scene draws
    uint   instanceBufferIndex;
    uint   materialBufferIndex;
    uint   indexBufferIndex;

    uint   pointLightBufferIndex; // bindless StructuredBuffer<PointLight> (Lighting.hlsli)
    uint   pointLightCount;
    uint   pointShadowMapIndex;  // bindless TextureCubeArray<float>: the point lights' cube shadow maps (M2)
    float  pointShadowNearScale; // their face cameras' near plane, as a fraction of the light's range

    row_major float4x4 view;     // world -> view space (clustered lighting)
    float  tanHalfFovX;
    float  tanHalfFovY;
    float  clusterNear;          // view-space depth range of the cluster slices
    float  clusterFar;
    uint   clusterGridIndex;         // StructuredBuffer<uint2> (first, count) per cluster (Clusters.hlsli)
    uint   clusterLightIndicesIndex; // StructuredBuffer<uint>
    uint   clusteredLighting;    // 1: point lights from the pixel's cluster list; 0: every light (brute force)
    uint   padding6;
};
ConstantBuffer<FrameConstants> g_frame : register(b2);

#endif
