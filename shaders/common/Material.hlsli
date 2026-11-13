// The material at one pixel, as the lighting sees it (Lighting.hlsli).
#ifndef WALNUT_MATERIAL_HLSLI
#define WALNUT_MATERIAL_HLSLI


// Everything the lighting needs from the material at one pixel: its textures and factors, evaluated. Forward
// shading gets it from SampleMaterial (scene.hlsl); deferred shading reads it back from the G-buffer.
struct MaterialSample
{
    float3 baseColor;
    float3 normal;          // shading normal: normal-mapped, flipped for back faces
    float3 geometricNormal; // the surface itself, without the normal map (shadow bias, ray offsets)
    float  roughness;
    float  metallic;
    float  occlusion;       // 1 = open to the sky, 0 = fully hidden (ambient only)
    float3 emissive;
};

#endif
