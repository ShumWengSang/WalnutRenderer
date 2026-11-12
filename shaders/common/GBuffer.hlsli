// G-buffer layout (deferred shading, K2). Must match GBuffer.h.
//
// The G-buffer ("geometry buffer") stores, for every pixel, what the lighting needs to know about the visible
// surface, so lighting can run later in a single fullscreen pass instead of once per drawn fragment:
//
//   RT0  baseColor  R8G8B8A8_UNORM_SRGB  rgb = base color (written linear; the _SRGB format encodes it)
//   RT1  normals    R16G16B16A16_SNORM   xy = shading normal, zw = geometric normal (both octahedral, see below)
//   RT2  material   R8G8B8A8_UNORM       r = roughness, g = metallic, b = occlusion
//   RT3  emissive   R11G11B10_FLOAT      rgb = emitted light (HDR: can be far above 1)
//   depth           R32_FLOAT (the depth buffer itself): the position is rebuilt from it, not stored
#ifndef WALNUT_GBUFFER_HLSLI
#define WALNUT_GBUFFER_HLSLI

struct GBufferOutput
{
    float4 baseColor : SV_Target0;
    float4 normals   : SV_Target1;
    float4 material  : SV_Target2;
    float4 emissive  : SV_Target3;
};

// Octahedral encoding: a unit vector in 2 numbers instead of 3. Picture the sphere of directions squashed into an
// octahedron (|x| + |y| + |z| = 1), the top half seen from above as a diamond, and the bottom half's four
// triangles folded out over the diamond's corners: together they fill the square -1..1 x -1..1. Every direction
// gets a spot, with nearly even precision all over the sphere, and decoding is cheap.
float2 OctahedralWrap(float2 v)
{
    return (1.0f - abs(v.yx)) * float2(v.x >= 0.0f ? 1.0f : -1.0f, v.y >= 0.0f ? 1.0f : -1.0f);
}

float2 OctahedralEncode(float3 n)
{
    n /= abs(n.x) + abs(n.y) + abs(n.z); // onto the octahedron
    return n.z >= 0.0f ? n.xy : OctahedralWrap(n.xy); // bottom half: fold out
}

float3 OctahedralDecode(float2 e)
{
    float3 n = float3(e, 1.0f - abs(e.x) - abs(e.y));
    const float t = saturate(-n.z); // > 0 only for the folded-out bottom half: fold it back
    n.xy += float2(n.x >= 0.0f ? -t : t, n.y >= 0.0f ? -t : t);
    return normalize(n);
}

#endif
