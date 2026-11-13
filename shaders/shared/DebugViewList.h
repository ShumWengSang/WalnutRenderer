// The debug views (View > Show), written once for C++ and HLSL. Not an ordinary header (no include guard): define
// WALNUT_VIEW before including it, and it expands once per view, in value order:
//
//   WALNUT_VIEW(name, value, setting, label, flags, group)
//     name     SceneDebugView::name in C++ (renderer/DebugViews.h), kView<name> in HLSL (common/DebugViews.hlsli)
//     value    what the shaders receive in the draw constants
//     setting  the value for --set=view.show=...
//     label    the UI's text
//     flags    kDebugViewForwardOnly:     needs a draw's own inputs (texture coordinates, instance and material ids,
//                                         the raw normal map, face orientation), which the G-buffer doesn't keep
//              kDebugViewNeedsRayTracing: computed by the ray-traced shader variant
//     group    a heading the UI shows above this view ("" for none)

// Base color lit by the sun plus ambient (and the point lights).
WALNUT_VIEW(Lit,                   0,  "lit",             "Lit (sun + ambient)",                     0, "")
// The same lighting on a white surface: shows the light without the textures.
WALNUT_VIEW(LightingOnly,          1,  "lighting",        "Lighting only",                           0, "")
// The material's base color: texture * factor (unlit).
WALNUT_VIEW(BaseColor,             2,  "basecolor",       "Base color (unlit)",                      0, "")
// World-space normal, mapped from -1..1 to 0..1 colors.
WALNUT_VIEW(Normals,               3,  "normals",         "Normals (world)",                         0, "")
// UV as red/green.
WALNUT_VIEW(Texcoords,             4,  "texcoords",       "Texture coordinates",                     kDebugViewForwardOnly, "")
// A random color per instance (= per draw).
WALNUT_VIEW(Instances,             5,  "instances",       "Instance colors",                         kDebugViewForwardOnly, "")
// A random color per material.
WALNUT_VIEW(Materials,             6,  "materials",       "Material colors",                         kDebugViewForwardOnly, "")
// Opaque gray, mask yellow, blend red; back faces of double-sided materials darker.
WALNUT_VIEW(AlphaMode,             7,  "alphamode",       "Alpha mode / back faces",                 kDebugViewForwardOnly, "")
// The sun's shadow test: white = the sun reaches this point, black = blocked.
WALNUT_VIEW(SunShadow,             8,  "sunshadow",       "Sun shadow",                              0, "")
// Ray traced vs shadow map: red = only the map shadows, blue = only the ray shadows.
WALNUT_VIEW(ShadowDifference,      9,  "shadowdiff",      "Shadow difference (ray traced vs map)",   kDebugViewNeedsRayTracing, "")
// Metallic-roughness map G * roughnessFactor (black = mirror, white = matte).
WALNUT_VIEW(Roughness,             10, "roughness",       "Roughness",                               0, "Material maps")
// Metallic-roughness map B * metallicFactor (white = metal).
WALNUT_VIEW(Metallic,              11, "metallic",        "Metallic",                                0, "")
// The raw tangent-space normal map (light blue = flat).
WALNUT_VIEW(NormalMap,             12, "normalmap",       "Normal map (tangent space)",              kDebugViewForwardOnly, "")
// Ambient occlusion map, scaled by its strength.
WALNUT_VIEW(Occlusion,             13, "occlusion",       "Occlusion",                               0, "")
// Emitted light: emissive map * factor.
WALNUT_VIEW(Emissive,              14, "emissive",        "Emissive",                                0, "")
// Only the sun's GGX specular reflection (the highlights).
WALNUT_VIEW(SunSpecular,           15, "sunspecular",     "Sun specular (highlights)",               0, "Lighting terms")
// Only the ambient light: sky/ground diffuse + reflection, times occlusion.
WALNUT_VIEW(Ambient,               16, "ambient",         "Ambient (sky + ground, occluded)",        0, "")
// The surface's own normal, without the normal map (used for shadows).
WALNUT_VIEW(GeometricNormals,      17, "geonormals",      "Geometric normals (no normal map)",       0, "Geometry")
// Only the point lights' contribution (diffuse + specular).
WALNUT_VIEW(PointLights,           18, "pointlights",     "Point lights only",                       0, "Point lights")
// Heat map: how many lights the pixel's cluster lists.
WALNUT_VIEW(ClusterLights,         19, "clusterlights",   "Lights per cluster (heat map)",           0, "")
// Average visibility of the shadowed lamps reaching the pixel.
WALNUT_VIEW(PointShadows,          20, "pointshadows",    "Point light shadows (shadowed lamps)",    0, "")
// Cube maps vs rays for the lamps with a cube (red: only cubes, blue: only rays).
WALNUT_VIEW(PointShadowDifference, 21, "pointshadowdiff", "Point shadow difference (cube maps vs rays)", kDebugViewNeedsRayTracing, "")
