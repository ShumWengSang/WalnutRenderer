// Per-frame constants at b2 (root parameter FrameData). Must match FrameConstants in src/renderer/FrameConstants.h.
#ifndef WALNUT_FRAME_CONSTANTS_HLSLI
#define WALNUT_FRAME_CONSTANTS_HLSLI

struct FrameConstants
{
    row_major float4x4 viewProjection;
    row_major float4x4 sunViewProjection; // world -> the sun's shadow map space
    float3 cameraPosition;  // world space
    float  padding0;
    float3 sunDirection;    // world space, unit length, pointing towards the sun
    float  padding1;
    float3 sunIlluminance;  // linear
    float  padding2;
    float3 ambientRadiance; // linear
    float  padding3;
};
ConstantBuffer<FrameConstants> g_frame : register(b2);

#endif
