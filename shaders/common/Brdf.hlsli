// Physically based shading: the glTF metallic-roughness BRDF (a "bidirectional reflectance distribution function":
// how much of the light arriving from L leaves towards V). Two parts:
//
//   diffuse  = (1 - F) * diffuseColor / pi                    light that goes into the surface, scatters, comes out
//   specular = D * G * F / (4 * N.L * N.V)                     light that bounces off the surface (Cook-Torrance)
//
// The specular part treats the surface as millions of tiny mirrors ("microfacets"). Only the mirrors facing H,
// halfway between L and V, bounce light from L into V:
//   D (GGX)    how many microfacets face H              roughness: smooth = few directions, tall narrow spike
//   G (Smith)  how many of them aren't hidden by others  matters at grazing angles, more for rough surfaces
//   F (Schlick Fresnel) how much each one reflects       F0 looking straight on, rising to 1 at grazing angles
#ifndef WALNUT_BRDF_HLSLI
#define WALNUT_BRDF_HLSLI

static const float kPi = 3.14159265f;

// How much a non-metal (water, plastic, stone, wood, ...) reflects looking straight at it: about 4%, untinted.
static const float kDielectricF0 = 0.04f;

// Roughness 0 would make the sun's highlight an infinitely small, infinitely bright point, which a pixel can only
// hit or miss (flickering). Keeping it above this leaves a highlight of at least a few pixels.
static const float kMinRoughness = 0.045f;

// What the BRDF needs from a material, after the metallic workflow has turned baseColor + metallic into the
// diffuse color and the specular reflectance.
struct SurfaceBrdf
{
    float3 diffuseColor; // light that enters and comes back out: baseColor for non-metals, black for metals
    float3 f0;           // reflectance at normal incidence: 4% for non-metals, baseColor for metals
    float  roughness;    // perceptual roughness (what the texture stores), after the minimum
    float  alpha;        // GGX width: roughness squared (makes the roughness slider feel even)
};

// glTF metallic workflow. Metals let no light in (no diffuse) and tint their reflection; non-metals reflect 4%
// untinted. Values between 0 and 1 are for blends (rust on iron, texture filtering at a boundary).
SurfaceBrdf MakeSurfaceBrdf(float3 baseColor, float metallic, float roughness)
{
    SurfaceBrdf surface;
    surface.diffuseColor = baseColor * (1.0f - metallic);
    surface.f0 = lerp(kDielectricF0.xxx, baseColor, metallic);
    surface.roughness = clamp(roughness, kMinRoughness, 1.0f);
    surface.alpha = surface.roughness * surface.roughness;
    return surface;
}

// GGX / Trowbridge-Reitz normal distribution: the density of microfacets whose normal is H.
float DistributionGgx(float NdotH, float alpha)
{
    const float a2 = alpha * alpha;
    const float d = NdotH * NdotH * (a2 - 1.0f) + 1.0f;
    return a2 / (kPi * d * d);
}

// Smith height-correlated masking-shadowing, already divided by the 4 * N.L * N.V of the specular formula
// ("visibility"): G / (4 N.L N.V). Folding them together cancels terms and avoids dividing by ~0 at grazing angles.
float VisibilitySmithGgxCorrelated(float NdotV, float NdotL, float alpha)
{
    const float a2 = alpha * alpha;
    const float ggxV = NdotL * sqrt(NdotV * NdotV * (1.0f - a2) + a2);
    const float ggxL = NdotV * sqrt(NdotL * NdotL * (1.0f - a2) + a2);
    return 0.5f / (ggxV + ggxL);
}

// Schlick's approximation of the Fresnel equations: f0 at normal incidence, rising to 1 as the angle grazes.
float3 FresnelSchlick(float3 f0, float VdotH)
{
    const float f = pow(1.0f - VdotH, 5.0f);
    return f0 + (1.0f - f0) * f;
}

struct BrdfValue
{
    float3 diffuse;
    float3 specular;
};

// The BRDF for light arriving from L (unit, towards the light) seen from V (unit, towards the camera). Multiply
// by the light's illuminance and N.L to get the reflected light.
BrdfValue EvaluateBrdf(SurfaceBrdf surface, float3 N, float3 V, float3 L)
{
    const float3 H = normalize(V + L);
    const float NdotL = saturate(dot(N, L));
    // A normal map can tilt N away from the camera; a tiny positive N.V keeps the formulas finite.
    const float NdotV = max(dot(N, V), 1e-4f);
    const float NdotH = saturate(dot(N, H));
    const float VdotH = saturate(dot(V, H));

    const float3 F = FresnelSchlick(surface.f0, VdotH);
    BrdfValue brdf;
    brdf.specular = DistributionGgx(NdotH, surface.alpha) * VisibilitySmithGgxCorrelated(NdotV, NdotL, surface.alpha) * F;
    // What the surface reflected (F) can't also enter it.
    brdf.diffuse = (1.0f - F) * surface.diffuseColor / kPi;
    return brdf;
}

// Light from a whole environment instead of one direction ("split sum"): the reflected light is roughly
//   (environment seen along the reflection, blurred by roughness) * (how much the specular lobe reflects in total)
// The second factor depends only on f0, roughness and N.V: integrated offline it is a small 2D table (F0 * A + B).
// This is an analytic fit of that table (Karis, "Physically Based Shading on Mobile", 2014). It rises towards 1 at
// grazing angles (Fresnel) and drops for rough surfaces seen edge-on (microfacets hiding each other).
float3 EnvironmentBrdf(SurfaceBrdf surface, float NdotV)
{
    const float4 c0 = float4(-1.0f, -0.0275f, -0.572f, 0.022f);
    const float4 c1 = float4(1.0f, 0.0425f, 1.04f, -0.04f);
    const float4 r = surface.roughness * c0 + c1;
    const float a004 = min(r.x * r.x, exp2(-9.28f * NdotV)) * r.x + r.y;
    const float2 AB = float2(-1.04f, 1.04f) * a004 + r.zw;
    return surface.f0 * AB.x + AB.y;
}

#endif
