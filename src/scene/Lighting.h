#pragma once

#include <DirectXMath.h>

namespace walnut
{
    // Where point lights get their shadows. Values must match Lighting.hlsli.
    enum class PointShadowTechnique : uint32_t
    {
        Off = 0,       // lamps light through everything
        CubeMaps = 1,  // the nearest lamps (the budget) get a cube shadow map
        RayTraced = 2, // every lamp: one shadow ray per lamp that reaches the pixel (needs inline ray tracing)
    };

    // How surfaces respond to light. Values must match scene.hlsl.
    enum class ShadingModel : uint32_t
    {
        Lambert = 0, // matte only: baseColor / pi (Step 1-2)
        Pbr = 1,     // glTF metallic-roughness: Lambert diffuse + Cook-Torrance GGX specular (Brdf.hlsli)
    };

    // The scene's light: one sun (a directional light: infinitely far away, so every point receives it from the
    // same direction) and an ambient sky/ground term standing in for all the light bouncing around.
    struct SceneLighting
    {
        // Where the sun is in the sky. Elevation: angle above the horizon. Azimuth: angle around +Y, 0 = towards +Z.
        float sunAzimuth = DirectX::XMConvertToRadians(-35.0f);
        float sunElevation = DirectX::XMConvertToRadians(65.0f);
        DirectX::XMFLOAT3 sunColor = { 1.0f, 0.95f, 0.85f }; // linear, slightly warm
        float sunIntensity = 8.0f;
        // Ambient: light from the sky above the horizon, and from the ground (sunlight bounced off it) below.
        DirectX::XMFLOAT3 skyColor = { 0.55f, 0.65f, 0.85f };    // linear, sky blue
        DirectX::XMFLOAT3 groundColor = { 0.40f, 0.35f, 0.28f }; // linear, warm stone
        float ambientIntensity = 0.25f;
        ShadingModel shadingModel = ShadingModel::Pbr;

        // Point lights (PointLights): how many of the generated lamps to use, and their shared range and intensity.
        // Range and intensity are set to fit the scene's size when it loads.
        int pointLightCount = 128;
        float pointLightRange = 4.0f;     // meters
        float pointLightIntensity = 8.0f; // at 1 m, a surface facing the lamp receives this much (falls off as 1/d^2)
        bool animatePointLights = true;
        bool showPointLightMarkers = true; // a small glowing dot at each lamp
        bool clusteredLighting = true;     // each pixel visits only its cluster's lights (false: every light)

        // Point light shadows (Step 6): the lamps nearest the camera get a cube shadow map, up to this many
        // (at most PointShadowMaps::kMaxShadowedLights). 0: no point light shadows.
        int pointShadowBudget = 16;
        PointShadowTechnique pointShadowTechnique = PointShadowTechnique::CubeMaps;

        // Shadow-map bias, in shadow-map texels (see SunShadow in scene.hlsl).
        float shadowConstantBias = 0.5f;
        float shadowSlopeBias = 1.0f;
        float shadowNormalOffset = 1.0f;
        // Shadow edge softness: -1 = hard (one texel, yes/no), 0 = hardware 2x2 filter, r = (2r+1)^2 filtered taps.
        int shadowFilterRadius = 1;

        // Unit vector from a surface towards the sun.
        DirectX::XMFLOAT3 GetSunDirection() const;

        // The camera the sun "sees" the scene with, for the shadow map. A directional light's rays are parallel, so
        // the projection is orthographic (a box, not a pyramid). The box is the bounding sphere of the given world
        // bounds, viewed along the sun direction: it holds the whole scene for any sun angle, and doesn't depend on
        // the player camera (so the shadows don't shimmer when it moves). Reversed-Z, like the main camera.
        DirectX::XMMATRIX GetSunViewProjection(const DirectX::XMFLOAT3& boundsMin, const DirectX::XMFLOAT3& boundsMax) const;
    };

    // Size of the sun camera's box (its width, height and depth) for the given bounds: the bounding sphere's diameter.
    float GetSunShadowExtent(const DirectX::XMFLOAT3& boundsMin, const DirectX::XMFLOAT3& boundsMax);
}
