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
    float  padding2;
    float3 ambientRadiance; // linear
    float  padding3;

    float  shadowTexelWorldSize; // meters per shadow-map texel
    float  shadowDepthPerMeter;  // depth change per meter towards the sun (linear: orthographic)
    float  shadowConstantBias;   // texels
    float  shadowSlopeBias;      // texels, times tan(angle to the sun)
    float  shadowNormalOffset;   // texels, along the surface normal
    uint   shadowFilterRadius;   // PCF kernel radius in texels; 0xFFFFFFFF = hard (point comparison)
    uint   tlasIndex;            // bindless SRV of the TLAS (RaytracingAccelerationStructure)
    float  padding4;
};
ConstantBuffer<FrameConstants> g_frame : register(b2);

#endif
