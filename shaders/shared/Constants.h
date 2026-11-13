// Enum values and sizes that the C++ code and the shaders both use (see Shared.h). The C++ enums take their values
// from here, e.g. ShadingModel::Pbr = kShadingPbr.
#ifndef WALNUT_SHARED_CONSTANTS_H
#define WALNUT_SHARED_CONSTANTS_H

#include "shared/Shared.h"

WALNUT_NAMESPACE_BEGIN

// ShadingModel (scene/Lighting.h): how surfaces respond to light.
WALNUT_CONST(kShadingLambert, 0);
WALNUT_CONST(kShadingPbr, 1);

// PointShadowTechnique (scene/Lighting.h): where point lights get their shadows.
WALNUT_CONST(kPointShadowOff, 0);
WALNUT_CONST(kPointShadowCubeMaps, 1);
WALNUT_CONST(kPointShadowRayTraced, 2);

// NormalMapping (renderer/ScenePass.h): where the tangent frame for normal mapping comes from.
WALNUT_CONST(kNormalMappingOff, 0);
WALNUT_CONST(kNormalMappingMesh, 1);
WALNUT_CONST(kNormalMappingDerived, 2);

// TonemapOperator (renderer/TonemapPass.h): the curve from HDR scene light to displayable 0..1.
WALNUT_CONST(kTonemapClamp, 0);
WALNUT_CONST(kTonemapReinhard, 1);
WALNUT_CONST(kTonemapAcesFitted, 2);

// AlphaMode (scene/Scene.h): how a material's alpha is used (glTF alphaMode).
WALNUT_CONST(kAlphaOpaque, 0);
WALNUT_CONST(kAlphaMask, 1);
WALNUT_CONST(kAlphaBlend, 2);

// "None" values of indices.
WALNUT_CONST(kNoTexture, 0xFFFFFFFF);    // a material slot without a texture: use the factor alone
WALNUT_CONST(kNoShadowSlot, 0xFFFFFFFF); // a point light without a cube shadow map (beyond the budget, or not cubes)

// The cluster grid of clustered lighting (renderer/ClusterGrid.h, common/Clusters.hlsli): 16 x 9 screen tiles x 24
// depth slices. The build (clusters.hlsl) runs one thread per cluster, kClusterThreadsPerGroup per group.
WALNUT_CONST(kClusterCountX, 16);
WALNUT_CONST(kClusterCountY, 9);
WALNUT_CONST(kClusterCountZ, 24);
WALNUT_CONST(kClusterCount, kClusterCountX * kClusterCountY * kClusterCountZ);
WALNUT_CONST(kClusterThreadsPerGroup, 64);

// Width and height of each face of a point light's cube shadow map (renderer/PointShadowMaps.h), in texels.
WALNUT_CONST(kPointShadowFaceSize, 256);

WALNUT_NAMESPACE_END

#endif
