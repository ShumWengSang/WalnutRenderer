// Scene shaders: draws every instance of the loaded scene. Compiled at runtime by DXC (see ScenePass.cpp).


#ifndef ALPHA_TEST
#define ALPHA_TEST 0 // ScenePass compiles the pixel shader twice: with ALPHA_TEST=1 for alpha-mask materials
#endif
#ifndef RAYTRACED_SHADOWS
#define RAYTRACED_SHADOWS 0 // 1: sun shadows from a ray query against the TLAS instead of the shadow map
#endif

#include "common/Bindless.hlsli"
#include "common/Brdf.hlsli"
#include "common/FrameConstants.hlsli"
#include "common/GBuffer.hlsli"
#include "common/Lighting.hlsli"

// Root constants at b0. Must match SceneDrawConstants in ScenePass.cpp.
struct DrawConstants
{
    uint vertexBufferIndex;   // descriptor heap slot of StructuredBuffer<Vertex>
    uint instanceBufferIndex; // descriptor heap slot of StructuredBuffer<Instance>
    uint materialBufferIndex; // descriptor heap slot of StructuredBuffer<Material>
    uint debugView;           // SceneDebugView in ScenePass.h
    uint instanceIndex;       // which instance this draw is
    uint indexBufferIndex;    // descriptor heap slot of the index buffer as StructuredBuffer<uint> (ray hits)
    uint normalMapping;       // NormalMapping in ScenePass.h
};
ConstantBuffer<DrawConstants> g_draw : register(b0);

static const uint kNormalMappingOff     = 0; // NormalMapping in ScenePass.h
static const uint kNormalMappingMesh    = 1;
static const uint kNormalMappingDerived = 2;

struct PSInput
{
    float4 position      : SV_Position;
    float3 worldPosition : POSITION;
    float3 normal        : NORMAL;
    float4 tangent       : TANGENT;   // xyz: world-space direction of increasing u; w: bitangent sign (+1/-1)
    float2 uv            : TEXCOORD0;
};

// Where a vertex lands in clip space for a camera. The depth pre-pass writes depth with one shader and the scene pass
// tests it with EQUAL using another, so both must compute bit-identical positions. They share this function, and
// 'precise' forbids the compiler from rearranging the math differently in each shader (fusing a multiply and an add
// into one instruction in one shader but not the other would change the last bit, and EQUAL would fail).
float4 ClipPosition(float3 position, float4x4 world, float4x4 viewProjection)
{
    precise const float4 worldPosition = mul(float4(position, 1.0f), world);
    precise const float4 clipPosition = mul(worldPosition, viewProjection);
    return clipPosition;
}

// For an indexed draw, SV_VertexID is the value read from the index buffer, which the loader made point straight
// into the merged vertex buffer.
PSInput VSMain(uint vertexId : SV_VertexID)
{
    StructuredBuffer<Vertex> vertices = ResourceDescriptorHeap[g_draw.vertexBufferIndex];
    StructuredBuffer<Instance> instances = ResourceDescriptorHeap[g_draw.instanceBufferIndex];
    const Vertex vertex = vertices[vertexId];
    const Instance instance = instances[g_draw.instanceIndex];

    const float4 worldPosition = mul(float4(vertex.position, 1.0f), instance.world);

    PSInput output;
    output.position = ClipPosition(vertex.position, instance.world, g_frame.viewProjection);
    output.worldPosition = worldPosition.xyz;
    output.normal = mul(float4(vertex.normal, 0.0f), instance.normalMatrix).xyz; // w = 0: directions don't move
    // A tangent lies IN the surface (unlike the normal, which sticks out of it), so it moves with the surface:
    // the world matrix itself, not the normal matrix.
    output.tangent = float4(mul(float4(vertex.tangent.xyz, 0.0f), instance.world).xyz, vertex.tangent.w);
    output.uv = vertex.uv;
    return output;
}

// Depth-only passes: the shadow map (seen from the sun) and the depth pre-pass (seen from the camera). Only the
// position is needed: opaque geometry is drawn without a pixel shader, so the rasterizer just writes each covered
// pixel's depth.
float4 DepthOnlyPosition(uint vertexId, float4x4 viewProjection)
{
    StructuredBuffer<Vertex> vertices = ResourceDescriptorHeap[g_draw.vertexBufferIndex];
    StructuredBuffer<Instance> instances = ResourceDescriptorHeap[g_draw.instanceBufferIndex];
    return ClipPosition(vertices[vertexId].position, instances[g_draw.instanceIndex].world, viewProjection);
}

float4 VSShadow(uint vertexId : SV_VertexID) : SV_Position
{
    return DepthOnlyPosition(vertexId, g_frame.sunViewProjection);
}

float4 VSDepthPrepass(uint vertexId : SV_VertexID) : SV_Position
{
    return DepthOnlyPosition(vertexId, g_frame.viewProjection);
}

// Alpha-tested geometry (leaves): the rectangle's depth must only be written where the leaf texture is opaque, so
// these draws need a pixel shader after all. It writes no color (there's no render target), it only decides whether
// the rasterizer may write this pixel's depth: 'discard' skips it.
struct DepthAlphaInput
{
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
};

DepthAlphaInput DepthAlphaTestVertex(uint vertexId, float4x4 viewProjection)
{
    StructuredBuffer<Vertex> vertices = ResourceDescriptorHeap[g_draw.vertexBufferIndex];
    StructuredBuffer<Instance> instances = ResourceDescriptorHeap[g_draw.instanceBufferIndex];
    const Vertex vertex = vertices[vertexId];

    DepthAlphaInput output;
    output.position = ClipPosition(vertex.position, instances[g_draw.instanceIndex].world, viewProjection);
    output.uv = vertex.uv;
    return output;
}

DepthAlphaInput VSShadowAlphaTest(uint vertexId : SV_VertexID)
{
    return DepthAlphaTestVertex(vertexId, g_frame.sunViewProjection);
}

DepthAlphaInput VSDepthPrepassAlphaTest(uint vertexId : SV_VertexID)
{
    return DepthAlphaTestVertex(vertexId, g_frame.viewProjection);
}

void PSDepthAlphaTest(DepthAlphaInput input)
{
    StructuredBuffer<Instance> instances = ResourceDescriptorHeap[g_draw.instanceBufferIndex];
    StructuredBuffer<Material> materials = ResourceDescriptorHeap[g_draw.materialBufferIndex];
    const Material material = materials[instances[g_draw.instanceIndex].materialIndex];

    float alpha = material.baseColorFactor.a;
    if (material.baseColorTexture != kNoTexture)
    {
        Texture2D<float4> baseColorTexture = ResourceDescriptorHeap[material.baseColorTexture];
        alpha *= baseColorTexture.Sample(g_samplerAnisotropicWrap, input.uv).a;
    }
    if (alpha < material.alphaCutoff)
        discard;
}

// Samples one of the material's textures, or returns 'fallback' if it has none (glTF: a missing texture counts as 1,
// so the factor alone applies). Every pixel of a draw uses the same material, so the index is uniform.
float4 SampleMaterialTexture(uint textureIndex, float2 uv, float4 fallback)
{
    if (textureIndex == kNoTexture)
        return fallback;
    Texture2D<float4> texture = ResourceDescriptorHeap[textureIndex];
    return texture.Sample(g_samplerAnisotropicWrap, uv);
}

// Tangent frame from screen-space derivatives, for meshes without a glTF TANGENT (or to compare with one).
// Between this pixel and its right/lower neighbour the surface moves by dp1/dp2 and the texture coordinates by
// duv1/duv2. Solving
//   dp1 = T * duv1.x + V * duv1.y
//   dp2 = T * duv2.x + V * duv2.y
// gives T = dP/du (the tangent) and V = dP/dv. glTF normal maps have green = "up in the image", and v grows
// downwards, so the bitangent points along -V: w is whichever sign makes cross(N, T) * w do that. (In the engine's
// mirrored space that's -1 for an ordinary mesh, matching the loader's flipped TANGENT.w.)
float4 DerivedTangent(float3 worldPosition, float2 uv, float3 normal)
{
    const float3 dp1 = ddx(worldPosition);
    const float3 dp2 = ddy(worldPosition);
    const float2 duv1 = ddx(uv);
    const float2 duv2 = ddy(uv);
    const float det = duv1.x * duv2.y - duv1.y * duv2.x;
    if (det == 0.0f)
        return 0.0f; // no texture stretch to measure (constant uv): no frame
    const float3 T = (dp1 * duv2.y - dp2 * duv1.y) / det;
    const float3 V = (dp2 * duv1.x - dp1 * duv2.x) / det;
    const float3 t = T - normal * dot(normal, T);
    const float w = dot(cross(normal, t), -V) >= 0.0f ? 1.0f : -1.0f;
    return float4(t, w);
}

// Cheap integer hash -> a stable, distinct color per index.
float3 HashColor(uint index)
{
    uint h = index * 747796405u + 2891336453u;
    h = ((h >> ((h >> 28u) + 4u)) ^ h) * 277803737u;
    h = (h >> 22u) ^ h;
    return float3(h & 0xFF, (h >> 8) & 0xFF, (h >> 16) & 0xFF) / 255.0f;
}

MaterialSample SampleMaterial(PSInput input, bool isFrontFace, Material material)
{
    // The normal points out of the front face. Seen from behind (a leaf's underside), the surface faces the other
    // way, so glTF says to flip it for double-sided materials.
    float3 normal = input.normal;
    // Files without normals leave them zero (normalize would give NaN). Fall back to the triangle's flat normal:
    // ddx/ddy give how the world position changes to the neighbouring pixel, and their cross product is
    // perpendicular to the surface. Computed outside the 'if': derivatives need every pixel of the 2x2 group.
    float3 flatNormal = normalize(cross(ddy(input.worldPosition), ddx(input.worldPosition)));
    if (dot(flatNormal, g_frame.cameraPosition - input.worldPosition) < 0.0f)
        flatNormal = -flatNormal; // we see this side, so it faces us
    const bool hasVertexNormal = dot(normal, normal) > 1e-12f;
    normal = hasVertexNormal ? normalize(normal) : flatNormal; // interpolation shortens normals, so normalize again

    // The surface itself, before any normal map: used for shadow lookups (bias and ray offsets need the real
    // surface; a bumped normal can tilt into it and bring back acne / self-intersection).
    float3 geometricNormal = normal;
    if (hasVertexNormal && !isFrontFace)
        geometricNormal = -geometricNormal;

    // Normal mapping: the normal map stores small tilts of the surface, relative to the surface itself ("tangent
    // space": x along the texture's u, y along v, z straight out). To use one, build that frame at this pixel and
    // turn the stored direction into a world-space one:
    //   T (tangent)   = the direction u increases in, from the mesh (glTF TANGENT) ...
    //   B (bitangent) = cross(N, T) * w  (w picks which way: textures can be mirrored)
    //   N (normal)    = the surface normal
    //   world normal  = T * n.x + B * n.y + N * n.z
    // ... or, for meshes without tangents, worked out from how position and uv change between neighbouring pixels
    // (screen-space derivatives), which is what DerivedTangent does. Computed outside the 'if' below because
    // derivatives need the whole 2x2 pixel group.
    const float4 derivedTangent = DerivedTangent(input.worldPosition, input.uv, normal);
    if (g_draw.normalMapping != kNormalMappingOff && material.normalTexture != kNoTexture)
    {
        float4 tangent = input.tangent;
        tangent.xyz -= normal * dot(normal, tangent.xyz); // interpolation bends it: make it perpendicular again
        const bool hasTangent = dot(tangent.xyz, tangent.xyz) > 1e-12f && tangent.w != 0.0f;
        if (g_draw.normalMapping == kNormalMappingDerived || !hasTangent)
            tangent = derivedTangent;

        if (dot(tangent.xyz, tangent.xyz) > 1e-12f)
        {
            const float3 T = normalize(tangent.xyz);
            const float3 B = cross(normal, T) * tangent.w;
            Texture2D<float4> normalMap = ResourceDescriptorHeap[material.normalTexture];
            float3 n = normalMap.Sample(g_samplerAnisotropicWrap, input.uv).xyz * 2.0f - 1.0f; // 0..1 -> -1..1
            n.xy *= material.normalScale;
            normal = normalize(T * n.x + B * n.y + normal * n.z);
        }
    }
    if (hasVertexNormal && !isFrontFace)
        normal = -normal;

    // Base color = constant factor * texture. The texture is _SRGB, so Sample() returns linear values.
    // The texture index comes from the material, which comes from a root constant, so it's the same for every
    // pixel of the draw ("uniform"). An index that could differ between pixels of one draw would have to be
    // wrapped in NonUniformResourceIndex(), or the GPU may read the wrong texture.
    float4 baseColor = material.baseColorFactor;
    if (material.baseColorTexture != kNoTexture)
    {
        Texture2D<float4> baseColorTexture = ResourceDescriptorHeap[material.baseColorTexture];
        baseColor *= baseColorTexture.Sample(g_samplerAnisotropicWrap, input.uv);
    }

#if ALPHA_TEST
    // Alpha test (glTF alphaMode MASK): pixels whose alpha is below the cutoff are thrown away. 'discard' writes
    // neither color nor depth, so the leaves' outlines come from the texture, not from the (rectangular) triangles.
    //
    // It's compiled only into the pixel shader that masked materials use. A shader that may discard can't have its
    // depth written before it runs ("early-Z"), since the GPU doesn't know yet whether the pixel survives; keeping
    // discard out of the opaque shader keeps that optimization for everything else. With the depth pre-pass (K1),
    // the scene pass doesn't use this variant at all: the pre-pass discards, and depth EQUAL does the rest.
    if (baseColor.a < material.alphaCutoff)
        discard;
#endif

    MaterialSample result;
    result.baseColor = baseColor.rgb;
    result.normal = normal;
    result.geometricNormal = geometricNormal;

    // glTF packs both into one texture: roughness in G, metallic in B (R is unused, or occlusion in some files).
    const float4 metallicRoughness = SampleMaterialTexture(material.metallicRoughnessTexture, input.uv, 1.0f);
    result.roughness = metallicRoughness.g * material.roughnessFactor;
    result.metallic = saturate(metallicRoughness.b * material.metallicFactor);
    if (g_frame.roughnessOverride >= 0.0f)
        result.roughness = g_frame.roughnessOverride; // View > Roughness override
    if (g_frame.metallicOverride >= 0.0f)
        result.metallic = g_frame.metallicOverride;

    result.occlusion = lerp(1.0f, SampleMaterialTexture(material.occlusionTexture, input.uv, 1.0f).r,
                            material.occlusionStrength);
    // Emitted light (screens, lamps, lava): added as is, it doesn't depend on any light arriving.
    result.emissive = SampleMaterialTexture(material.emissiveTexture, input.uv, 1.0f).rgb * material.emissiveFactor;
    return result;
}

// SV_IsFrontFace: whether the rasterizer saw this triangle's front (counter-clockwise on screen, per the PSO).
// Only visible for double-sided materials (or with culling turned off).
//
// Deferred shading (K2): instead of lighting the pixel, store what the lighting will need (the G-buffer, see
// GBuffer.hlsli). Drawn after the depth pre-pass with depth EQUAL, so each pixel writes the G-buffer once.
GBufferOutput PSGBuffer(PSInput input, bool isFrontFace : SV_IsFrontFace)
{
    StructuredBuffer<Instance> instances = ResourceDescriptorHeap[g_draw.instanceBufferIndex];
    StructuredBuffer<Material> materials = ResourceDescriptorHeap[g_draw.materialBufferIndex];
    const Material material = materials[instances[g_draw.instanceIndex].materialIndex];
    const MaterialSample mat = SampleMaterial(input, isFrontFace, material);

    GBufferOutput output;
    output.baseColor = float4(mat.baseColor, 1.0f);
    output.normals = float4(OctahedralEncode(mat.normal), OctahedralEncode(mat.geometricNormal));
    output.material = float4(mat.roughness, mat.metallic, mat.occlusion, 0.0f);
    output.emissive = float4(mat.emissive, 0.0f);
    return output;
}

// Forward shading: material and lighting in one go, written straight to the HDR scene color.
float4 PSMain(PSInput input, bool isFrontFace : SV_IsFrontFace) : SV_Target
{
    // The material is the same for the whole draw, so every pixel reads the same two elements (cheap: cached).
    StructuredBuffer<Instance> instances = ResourceDescriptorHeap[g_draw.instanceBufferIndex];
    StructuredBuffer<Material> materials = ResourceDescriptorHeap[g_draw.materialBufferIndex];
    const uint materialIndex = instances[g_draw.instanceIndex].materialIndex;
    const Material material = materials[materialIndex];

    const MaterialSample mat = SampleMaterial(input, isFrontFace, material);
    const ShadingResult shading = ShadeSurface(input.worldPosition, mat, g_draw.debugView == kViewShadowDifference);

    float3 color;
    if (SharedViewColor(g_draw.debugView, input.worldPosition, mat, shading, color))
        return float4(color, 1.0f);

    // Views only the forward pass can show: they need the draw's own inputs.
    if (g_draw.debugView == kViewTexcoords)
        color = float3(frac(input.uv), 0.0f); // frac: glTF UVs may go beyond 0..1 (repeating textures)
    else if (g_draw.debugView == kViewInstances)
        color = HashColor(g_draw.instanceIndex);
    else if (g_draw.debugView == kViewMaterials)
        color = HashColor(materialIndex);
    else if (g_draw.debugView == kViewNormalMap)
        // Raw tangent-space normal map: (0.5, 0.5, 1) = "straight out of the surface", the typical light blue.
        color = SampleMaterialTexture(material.normalTexture, input.uv, float4(0.5f, 0.5f, 1.0f, 1.0f)).rgb;
    else
    {
        color = material.alphaMode == kAlphaMask  ? float3(1.0f, 0.85f, 0.1f)
              : material.alphaMode == kAlphaBlend ? float3(1.0f, 0.2f, 0.1f)
                                                  : float3(0.4f, 0.4f, 0.4f);
        if (!isFrontFace)
            color *= 0.4f; // back faces darker, to show which surfaces are drawn from both sides
    }
    return float4(color, 1.0f);
}
