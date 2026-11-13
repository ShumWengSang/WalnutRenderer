// Debug views (View > Show) and shading models, shared by the forward (scene.hlsl) and deferred (deferred.hlsl)
// shaders. Must match SceneDebugView in ScenePass.h and ShadingModel in Lighting.h.
#ifndef WALNUT_DEBUG_VIEWS_HLSLI
#define WALNUT_DEBUG_VIEWS_HLSLI

static const uint kViewLit          = 0; // SceneDebugView in ScenePass.h
static const uint kViewLightingOnly = 1;
static const uint kViewBaseColor    = 2;
static const uint kViewNormals      = 3;
static const uint kViewTexcoords    = 4;
static const uint kViewInstances    = 5;
static const uint kViewMaterials    = 6;
static const uint kViewAlphaMode    = 7;
static const uint kViewSunShadow    = 8;
static const uint kViewShadowDifference = 9;
static const uint kViewRoughness    = 10;
static const uint kViewMetallic     = 11;
static const uint kViewNormalMap    = 12;
static const uint kViewOcclusion    = 13;
static const uint kViewEmissive     = 14;
static const uint kViewSunSpecular  = 15;
static const uint kViewAmbient      = 16;
static const uint kViewGeometricNormals = 17;
static const uint kViewPointLights  = 18;
static const uint kViewClusterLights = 19;
static const uint kViewPointShadows = 20;
static const uint kViewPointShadowDifference = 21;

static const uint kShadingLambert = 0; // ShadingModel in Lighting.h
static const uint kShadingPbr     = 1;

#endif
