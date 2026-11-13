#pragma once

#include "renderer/DebugViews.h"
#include "shared/Constants.h"

#include <cstdint>

// The renderer's settings, split by what they are:
//   RenderSettings  the techniques: how the frame is rendered (what the UI's View and Lighting sections choose)
//   DebugSettings   what to look at: debug views and inspection aids, not part of how the image is made
//   RenderCaps      what the GPU and the loaded scene allow, decided once
//   FrameSettings   this frame's decisions, derived from the three by ResolveFrameSettings: every pass reads these
// What the world contains (the sun, sky, lamps) is SceneLighting (scene/Lighting.h).
namespace walnut
{
    // How the scene's pixels get their color.
    enum class RenderPath : uint32_t
    {
        Forward,  // one pass: each draw's pixel shader samples the material and lights it
        Deferred, // the draws only write the material to the G-buffer; a fullscreen pass reads it (K2/K3)
    };

    // How surfaces respond to light. Values shared with the shaders (shared/Constants.h).
    enum class ShadingModel : uint32_t
    {
        Lambert = kShadingLambert, // matte only: baseColor / pi (Step 1-2)
        Pbr = kShadingPbr,         // glTF metallic-roughness: Lambert diffuse + Cook-Torrance GGX specular (Brdf.hlsli)
    };

    // Where the tangent frame for normal mapping comes from. Values shared with scene.hlsl (shared/Constants.h).
    enum class NormalMapping : uint32_t
    {
        Off = kNormalMappingOff,         // ignore normal maps: the interpolated vertex normal only
        Mesh = kNormalMappingMesh,       // TBN from the glTF TANGENT attribute (derived per pixel for meshes without one)
        Derived = kNormalMappingDerived, // TBN always derived per pixel from position/uv screen-space derivatives
    };

    // Where point lights get their shadows. Values shared with the shaders (shared/Constants.h).
    enum class PointShadowTechnique : uint32_t
    {
        Off = kPointShadowOff,             // lamps light through everything
        CubeMaps = kPointShadowCubeMaps,   // the nearest lamps (the budget) get a cube shadow map
        RayTraced = kPointShadowRayTraced, // every lamp: one shadow ray per lamp that reaches the pixel (needs inline ray tracing)
    };

    struct SunShadowSettings
    {
        bool enabled = true;
        bool raytraced = false; // one shadow ray per pixel instead of the shadow map (needs RenderCaps::raytracing)
        // Shadow-map bias, in shadow-map texels (see SunShadow in shaders/common/SunShadow.hlsli).
        float constantBias = 0.5f;
        float slopeBias = 1.0f;
        float normalOffset = 1.0f;
        // Shadow edge softness: -1 = hard (one texel, yes/no), 0 = hardware 2x2 filter, r = (2r+1)^2 filtered taps.
        int filterRadius = 1;
    };

    struct PointShadowSettings
    {
        PointShadowTechnique technique = PointShadowTechnique::CubeMaps;
        // With cube maps, the lamps nearest the camera get one, up to this many (at most
        // PointShadowMaps::kMaxShadowedLights). 0: no point light shadows.
        int budget = 16;
    };

    struct RenderSettings
    {
        RenderPath path = RenderPath::Forward;
        bool depthPrepass = true; // draw depth first, then shade each pixel once (depth EQUAL); deferred always does
        ShadingModel shading = ShadingModel::Pbr;
        NormalMapping normalMapping = NormalMapping::Mesh;
        bool alphaTest = true;         // cut out alpha-mask materials; off draws them as opaque, to compare
        bool backfaceCulling = true;   // for single-sided materials; double-sided ones are never culled
        bool clusteredLighting = true; // each pixel visits only its cluster's point lights (false: every light)
        SunShadowSettings sunShadows;
        PointShadowSettings pointShadows;
    };

    struct DebugSettings
    {
        SceneDebugView view = SceneDebugView::Lit;
        // Negative: use the material's value. 0..1: every material uses this instead (sent as frame constants).
        float roughnessOverride = -1.0f;
        float metallicOverride = -1.0f;
        bool lightMarkers = true; // a small glowing dot at each point light
        float clearColor[4] = {}; // the scene target's background, linear (starts as Renderer::kDefaultClearColor)
    };

    struct RenderCaps
    {
        // Rays can be traced: inline ray tracing is supported and the scene's acceleration structures exist. Without
        // it, ray-traced shadows fall back to the shadow map / cube maps.
        bool raytracing = false;
    };

    struct FrameSettings
    {
        bool deferred = false;
        bool depthPrepass = false;  // also always with deferred (its G-buffer pass tests depth EQUAL)
        bool buildClusters = false; // clustered lighting, or the clusters' heat map view
        bool sunShadows = false;    // sun shadows on at all
        bool sunRaytraced = false;  // the sun's shadows from rays
        bool compareSunShadows = false; // the shadow difference view: the shadow map AND rays
        bool drawSunCasters = false;    // render the shadow map's casters (nothing reads the map when rays replace it)
        PointShadowTechnique pointShadows = PointShadowTechnique::Off; // cube maps in the comparison view
        bool comparePointShadows = false; // the point shadow difference view: cube maps AND rays
        uint32_t cubeShadowBudget = 0;    // how many lamps get a cube shadow map (0 unless cube maps)
        bool raytracedVariant = false;    // the shaders that trace rays (for the sun, the points, or a comparison)

        // How each draw is shaded.
        SceneDebugView view = SceneDebugView::Lit;
        ShadingModel shading = ShadingModel::Pbr;
        NormalMapping normalMapping = NormalMapping::Mesh;
        bool alphaTest = true;
        bool backfaceCulling = true;
    };

    // Every "should this frame..." decision, in one place.
    FrameSettings ResolveFrameSettings(const RenderSettings& settings, const DebugSettings& debug, const RenderCaps& caps);
}
