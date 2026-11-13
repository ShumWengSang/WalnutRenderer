// Lighting shared by forward shading (scene.hlsl) and the deferred lighting pass (deferred.hlsl). One include for
// everything, split into:
//   Material.hlsli         MaterialSample: the material at one pixel
//   SunShadow.hlsli        the sun's shadow map lookup (bias, PCF)
//   ShadowRays.hlsli       inline-ray-tracing shadow rays, sun and point lights (RAYTRACED_SHADOWS variants only)
//   PointLight.hlsli       the point light layout and falloff
//   PointShadows.hlsli     point light cube shadow maps
//   Shading.hlsli          hemisphere ambient, AddPointLight, ShadeSurface
//   DebugViewColors.hlsli  the debug views both paths can show
//
// Expects RAYTRACED_SHADOWS to be defined (0 or 1) before it's included.
#ifndef WALNUT_LIGHTING_HLSLI
#define WALNUT_LIGHTING_HLSLI

#include "common/Material.hlsli"
#include "common/SunShadow.hlsli"
#include "common/ShadowRays.hlsli"
#include "common/PointLight.hlsli"
#include "common/PointShadows.hlsli"
#include "common/Shading.hlsli"
#include "common/DebugViewColors.hlsli"

#endif
