// The scene's GPU data layouts, shared by every shader that reads the scene buffers (vertex pulling, materials,
// ray hits). Must match Scene.h and ScenePass.cpp.
#ifndef WALNUT_SCENE_DATA_HLSLI
#define WALNUT_SCENE_DATA_HLSLI

#include "shared/Constants.h" // kNoTexture, kAlpha* (AlphaMode)


// Must match SceneVertex in Scene.h (48 bytes).
struct Vertex
{
    float3 position;
    float3 normal;
    float4 tangent;
    float2 uv;
};

// Must match InstanceGpuData in ScenePass.cpp (144 bytes).
struct Instance
{
    row_major float4x4 world;
    row_major float4x4 normalMatrix;
    uint materialIndex;
    uint firstIndex;  // where its triangles start in the index buffer
    uint2 padding;
};

// Must match MaterialGpuData in ScenePass.cpp (80 bytes).
struct Material
{
    float4 baseColorFactor; // linear
    float3 emissiveFactor;
    float metallicFactor;
    float roughnessFactor;
    float alphaCutoff;
    uint alphaMode;
    uint doubleSided;
    // Descriptor heap slots of Texture2Ds, or kNoTexture.
    uint baseColorTexture;         // sRGB: base color + alpha
    uint metallicRoughnessTexture; // linear: G = roughness, B = metallic
    uint normalTexture;            // linear: tangent-space normal
    uint occlusionTexture;         // linear: R = ambient occlusion
    uint emissiveTexture;          // sRGB: emitted light color
    float normalScale;
    float occlusionStrength;
    uint padding;
};

#endif
